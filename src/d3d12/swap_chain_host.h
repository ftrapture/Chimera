#pragma once

#include <array>
#include <cstdint>
#include <sstream>
#include <string>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "common/result.h"
#include "d3d12/device_context.h"

namespace chimera::d3d12 {

class SwapChainHost final {
public:
    [[nodiscard]] chimera::common::Result<void> Initialize(
        HWND window_handle,
        DeviceContext& device_context,
        std::uint32_t width,
        std::uint32_t height);

    [[nodiscard]] chimera::common::Result<void> Resize(
        DeviceContext& device_context,
        std::uint32_t width,
        std::uint32_t height);

    [[nodiscard]] chimera::common::Result<void> WaitForFrame(DeviceContext& device_context) {
        const auto pending_value = fence_values_[current_back_buffer_index_];
        if (pending_value == 0U) {
            return {};
        }
        return device_context.WaitForFenceValue(pending_value);
    }

    [[nodiscard]] chimera::common::Result<void> Present(DeviceContext& device_context, bool enable_vsync) {
        const auto presenting_index = current_back_buffer_index_;
        const UINT present_flags = (!enable_vsync && tearing_supported_) ? DXGI_PRESENT_ALLOW_TEARING : 0U;
        const auto hr = swap_chain_->Present(enable_vsync ? 1U : 0U, present_flags);
        if (FAILED(hr)) {
            std::ostringstream stream;
            stream << "IDXGISwapChain::Present failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
            if (hr == DXGI_ERROR_DEVICE_REMOVED) {
                Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device;
                if (SUCCEEDED(swap_chain_->GetDevice(IID_PPV_ARGS(d3d12_device.GetAddressOf())))) {
                    HRESULT reason = d3d12_device->GetDeviceRemovedReason();
                    stream << " (Device Removed Reason: 0x" << std::hex << static_cast<unsigned long>(reason) << ")";
                }
            }
            return chimera::common::Status::Error(chimera::common::ErrorCode::kDeviceError, stream.str());
        }

        const auto signal_value = next_fence_value_++;
        auto signal_result = device_context.Signal(signal_value);
        if (!signal_result.ok()) {
            return signal_result.status();
        }

        fence_values_[presenting_index] = signal_value;
        current_back_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
        return {};
    }

    [[nodiscard]] DXGI_FORMAT format() const noexcept { return back_buffer_format_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t current_back_buffer_index() const noexcept { return current_back_buffer_index_; }
    [[nodiscard]] ID3D12Resource* CurrentBackBuffer() const noexcept { return back_buffers_[current_back_buffer_index_].Get(); }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CurrentRtv() const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(current_back_buffer_index_) * static_cast<SIZE_T>(rtv_descriptor_size_);
        return handle;
    }

    [[nodiscard]] D3D12_VIEWPORT viewport() const noexcept {
        return D3D12_VIEWPORT{0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_), 0.0F, 1.0F};
    }

    [[nodiscard]] D3D12_RECT scissor_rect() const noexcept {
        return D3D12_RECT{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    }

private:
    [[nodiscard]] static chimera::common::Result<bool> CheckTearingSupport(IDXGIFactory7* factory) {
        BOOL allow_tearing = FALSE;
        const auto hr = factory->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allow_tearing,
            sizeof(allow_tearing));
        if (FAILED(hr)) {
            std::ostringstream stream;
            stream << "DXGI tearing support query failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
            return chimera::common::Status::Error(chimera::common::ErrorCode::kDeviceError, stream.str());
        }
        return allow_tearing == TRUE;
    }

    [[nodiscard]] chimera::common::Result<void> CreateBackBuffers(ID3D12Device* device) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = DeviceContext::kFrameCount;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        auto hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            std::ostringstream stream;
            stream << "CreateDescriptorHeap(RTV) failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
            return chimera::common::Status::Error(chimera::common::ErrorCode::kDeviceError, stream.str());
        }

        rtv_descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t index = 0; index < DeviceContext::kFrameCount; ++index) {
            hr = swap_chain_->GetBuffer(index, IID_PPV_ARGS(back_buffers_[index].ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                std::ostringstream stream;
                stream << "IDXGISwapChain::GetBuffer failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
                return chimera::common::Status::Error(chimera::common::ErrorCode::kDeviceError, stream.str());
            }
            device->CreateRenderTargetView(back_buffers_[index].Get(), nullptr, handle);
            handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
        }
        return {};
    }

    HWND window_handle_{nullptr};
    std::uint32_t width_{0U};
    std::uint32_t height_{0U};
    bool tearing_supported_{false};
    DXGI_FORMAT back_buffer_format_{DXGI_FORMAT_R8G8B8A8_UNORM};
    std::uint32_t current_back_buffer_index_{0U};
    std::uint64_t next_fence_value_{1U};
    std::array<std::uint64_t, DeviceContext::kFrameCount> fence_values_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swap_chain_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, DeviceContext::kFrameCount> back_buffers_{};
    UINT rtv_descriptor_size_{0U};
};

inline chimera::common::Result<void> SwapChainHost::Initialize(
    HWND window_handle,
    DeviceContext& device_context,
    const std::uint32_t width,
    const std::uint32_t height) {
    window_handle_ = window_handle;
    width_ = width;
    height_ = height;

    auto tearing_support = CheckTearingSupport(device_context.factory());
    if (!tearing_support.ok()) {
        return tearing_support.status();
    }
    tearing_supported_ = tearing_support.value();

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = back_buffer_format_;
    desc.BufferCount = DeviceContext::kFrameCount;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1U;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.Flags = tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_1{};
    const auto hr = device_context.factory()->CreateSwapChainForHwnd(
        device_context.command_queue(),
        window_handle_,
        &desc,
        nullptr,
        nullptr,
        swap_chain_1.GetAddressOf());
    if (FAILED(hr)) {
        std::ostringstream stream;
        stream << "CreateSwapChainForHwnd failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
        return chimera::common::Status::Error(chimera::common::ErrorCode::kDeviceError, stream.str());
    }

    auto cast_hr = swap_chain_1.As(&swap_chain_);
    if (FAILED(cast_hr)) {
        std::ostringstream stream;
        stream << "Swap chain cast failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(cast_hr);
        return chimera::common::Status::Error(chimera::common::ErrorCode::kDeviceError, stream.str());
    }

    device_context.factory()->MakeWindowAssociation(window_handle_, DXGI_MWA_NO_ALT_ENTER);
    current_back_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
    return CreateBackBuffers(device_context.device());
}

inline chimera::common::Result<void> SwapChainHost::Resize(
    DeviceContext& device_context,
    const std::uint32_t width,
    const std::uint32_t height) {
    if (width == 0U || height == 0U) {
        return {};
    }

    auto flush_result = device_context.Flush();
    if (!flush_result.ok()) {
        return flush_result.status();
    }

    for (auto& back_buffer : back_buffers_) {
        back_buffer.Reset();
    }
    rtv_heap_.Reset();

    width_ = width;
    height_ = height;

    const auto hr = swap_chain_->ResizeBuffers(
        DeviceContext::kFrameCount,
        width_,
        height_,
        back_buffer_format_,
        tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U);
    if (FAILED(hr)) {
        std::ostringstream stream;
        stream << "ResizeBuffers failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
        return chimera::common::Status::Error(chimera::common::ErrorCode::kDeviceError, stream.str());
    }

    current_back_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
    return CreateBackBuffers(device_context.device());
}

}  // namespace chimera::d3d12
