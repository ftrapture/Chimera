#include "d3d12/device_context.h"

#include <sstream>

#include <d3d12sdklayers.h>

namespace chimera::d3d12 {

namespace {

using chimera::common::ErrorCode;
using chimera::common::Result;
using chimera::common::Status;

[[nodiscard]] Result<void> HrCheck(const HRESULT hr, const char* context) {
    if (SUCCEEDED(hr)) {
        return {};
    }

    std::ostringstream stream;
    stream << context << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return Status::Error(ErrorCode::kDeviceError, stream.str());
}

}  // namespace

DeviceContext::~DeviceContext() {
    if (fence_event_ != nullptr) {
        ::CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
}

Result<void> DeviceContext::Initialize(const bool enable_debug_layer) {
    CHIMERA_RETURN_IF_ERROR(CreateFactory(enable_debug_layer));
    CHIMERA_RETURN_IF_ERROR(CreateDevice(enable_debug_layer));
    CHIMERA_RETURN_IF_ERROR(CreateCommandObjects());
    return {};
}

Result<void> DeviceContext::BeginFrame(const std::uint32_t frame_index) {
    CHIMERA_RETURN_IF_ERROR(HrCheck(command_allocators_[frame_index]->Reset(), "ID3D12CommandAllocator::Reset"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(command_list_->Reset(command_allocators_[frame_index].Get(), nullptr), "ID3D12GraphicsCommandList::Reset"));
    return {};
}

Result<void> DeviceContext::ExecuteCommandList() {
    CHIMERA_RETURN_IF_ERROR(HrCheck(command_list_->Close(), "ID3D12GraphicsCommandList::Close"));
    ID3D12CommandList* lists[] = {command_list_.Get()};
    command_queue_->ExecuteCommandLists(1U, lists);
    return {};
}

Result<void> DeviceContext::Signal(const std::uint64_t value) {
    CHIMERA_RETURN_IF_ERROR(HrCheck(command_queue_->Signal(fence_.Get(), value), "ID3D12CommandQueue::Signal"));
    return {};
}

Result<void> DeviceContext::WaitForFenceValue(const std::uint64_t value) {
    if (fence_->GetCompletedValue() >= value) {
        return {};
    }

    CHIMERA_RETURN_IF_ERROR(HrCheck(fence_->SetEventOnCompletion(value, fence_event_), "ID3D12Fence::SetEventOnCompletion"));
    const auto wait_result = ::WaitForSingleObject(fence_event_, 5000U);
    if (wait_result != WAIT_OBJECT_0) {
        return Status::Error(ErrorCode::kTimeout, "Timed out while waiting for GPU fence.");
    }
    return {};
}

Result<void> DeviceContext::Flush() {
    const auto signal_value = next_fence_value_++;
    CHIMERA_RETURN_IF_ERROR(Signal(signal_value));
    CHIMERA_RETURN_IF_ERROR(WaitForFenceValue(signal_value));
    return {};
}

Result<void> DeviceContext::CreateFactory(const bool enable_debug_layer) {
    UINT flags = 0U;
    if (enable_debug_layer) {
        Microsoft::WRL::ComPtr<ID3D12Debug> debug_controller{};
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug_controller.GetAddressOf())))) {
            debug_controller->EnableDebugLayer();
            flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    CHIMERA_RETURN_IF_ERROR(HrCheck(CreateDXGIFactory2(flags, IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf())), "CreateDXGIFactory2"));
    return {};
}

Result<void> DeviceContext::CreateDevice(const bool enable_debug_layer) {
    if (enable_debug_layer) {
        Microsoft::WRL::ComPtr<ID3D12Debug1> debug_controller{};
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug_controller.GetAddressOf())))) {
            debug_controller->SetEnableGPUBasedValidation(FALSE);
        }
    }

    for (UINT adapter_index = 0U;; ++adapter_index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate{};
        const auto hr = factory_->EnumAdapterByGpuPreference(
            adapter_index,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(candidate.GetAddressOf()));
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        CHIMERA_RETURN_IF_ERROR(HrCheck(hr, "IDXGIFactory7::EnumAdapterByGpuPreference"));

        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
            adapter_ = candidate;
            break;
        }
    }

    if (!adapter_) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> warp_adapter{};
        CHIMERA_RETURN_IF_ERROR(HrCheck(factory_->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.GetAddressOf())), "IDXGIFactory7::EnumWarpAdapter"));
        CHIMERA_RETURN_IF_ERROR(HrCheck(warp_adapter.As(&adapter_), "WARP adapter cast"));
    }

    CHIMERA_RETURN_IF_ERROR(HrCheck(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device_.ReleaseAndGetAddressOf())), "D3D12CreateDevice"));
    return {};
}

Result<void> DeviceContext::CreateCommandObjects() {
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0U;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(command_queue_.ReleaseAndGetAddressOf())), "ID3D12Device::CreateCommandQueue"));

    for (auto& allocator : command_allocators_) {
        CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())), "ID3D12Device::CreateCommandAllocator"));
    }

    CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateCommandList(
        0U,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        command_allocators_[0].Get(),
        nullptr,
        IID_PPV_ARGS(command_list_.ReleaseAndGetAddressOf())),
        "ID3D12Device::CreateCommandList"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(command_list_->Close(), "ID3D12GraphicsCommandList::Close"));

    CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf())), "ID3D12Device::CreateFence"));

    fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event_ == nullptr) {
        return Status::Error(ErrorCode::kDeviceError, "CreateEventW failed for fence synchronization.");
    }

    return {};
}

}  // namespace chimera::d3d12
