#pragma once

#include <array>
#include <cstdint>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "common/result.h"

namespace chimera::d3d12 {

class DeviceContext final {
public:
    static constexpr std::uint32_t kFrameCount = 2U;

    DeviceContext() = default;
    ~DeviceContext();

    [[nodiscard]] chimera::common::Result<void> Initialize(bool enable_debug_layer);
    [[nodiscard]] chimera::common::Result<void> BeginFrame(std::uint32_t frame_index);
    [[nodiscard]] chimera::common::Result<void> ExecuteCommandList();
    [[nodiscard]] chimera::common::Result<void> Signal(std::uint64_t value);
    [[nodiscard]] chimera::common::Result<void> WaitForFenceValue(std::uint64_t value);
    [[nodiscard]] chimera::common::Result<void> Flush();

    [[nodiscard]] ID3D12Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* command_queue() const noexcept { return command_queue_.Get(); }
    [[nodiscard]] IDXGIFactory7* factory() const noexcept { return factory_.Get(); }
    [[nodiscard]] ID3D12GraphicsCommandList* command_list() const noexcept { return command_list_.Get(); }
    [[nodiscard]] ID3D12Fence* fence() const noexcept { return fence_.Get(); }

private:
    [[nodiscard]] chimera::common::Result<void> CreateFactory(bool enable_debug_layer);
    [[nodiscard]] chimera::common::Result<void> CreateDevice(bool enable_debug_layer);
    [[nodiscard]] chimera::common::Result<void> CreateCommandObjects();

    Microsoft::WRL::ComPtr<IDXGIFactory7> factory_{};
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_{};
    Microsoft::WRL::ComPtr<ID3D12Device> device_{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> command_queue_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kFrameCount> command_allocators_{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_{};
    HANDLE fence_event_{nullptr};
    std::uint64_t next_fence_value_{1U};
};

}  // namespace chimera::d3d12
