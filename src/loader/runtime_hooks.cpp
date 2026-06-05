#include "loader/runtime_hooks.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <sstream>

#include <MinHook.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "common/log.h"
#include "loader/runtime_d3d12_overlay.h"
#include "platform/win32/process_utils.h"

namespace chimera::loader::runtime {

namespace {

using chimera::common::ErrorCode;
using chimera::common::Result;
using chimera::common::Status;

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using FactoryCreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using FactoryCreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using FactoryCreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using SwapChainPresent1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using SwapChainResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using DeviceCreateCommandListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using DeviceCreateRenderTargetViewFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using DeviceCreateDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using GraphicsCommandListOMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
using CreateProcessAFn = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);

[[nodiscard]] std::string NarrowAscii(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t ch : text) {
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

D3D12CreateDeviceFn g_original_d3d12_create_device = nullptr;
CreateDXGIFactory1Fn g_original_create_dxgi_factory1 = nullptr;
CreateDXGIFactory2Fn g_original_create_dxgi_factory2 = nullptr;
FactoryCreateSwapChainFn g_original_factory_create_swap_chain = nullptr;
FactoryCreateSwapChainForHwndFn g_original_factory_create_swap_chain_for_hwnd = nullptr;
FactoryCreateSwapChainForCompositionFn g_original_factory_create_swap_chain_for_composition = nullptr;
SwapChainPresentFn g_original_swap_chain_present = nullptr;
SwapChainPresent1Fn g_original_swap_chain_present1 = nullptr;
SwapChainResizeBuffersFn g_original_swap_chain_resize_buffers = nullptr;
DeviceCreateCommandListFn g_original_device_create_command_list = nullptr;
DeviceCreateRenderTargetViewFn g_original_device_create_render_target_view = nullptr;
DeviceCreateDepthStencilViewFn g_original_device_create_depth_stencil_view = nullptr;
GraphicsCommandListOMSetRenderTargetsFn g_original_command_list_om_set_render_targets = nullptr;
CreateProcessWFn g_original_create_process_w = nullptr;
CreateProcessAFn g_original_create_process_a = nullptr;

std::atomic<bool> g_minhook_initialized{false};
std::atomic<bool> g_factory_methods_hooked{false};
std::atomic<bool> g_swap_chain_methods_hooked{false};
std::atomic<bool> g_device_methods_hooked{false};
std::atomic<bool> g_command_list_methods_hooked{false};
std::atomic<int> g_present_log_budget{8};
std::mutex g_hook_mutex{};

[[nodiscard]] Status MhStatusToStatus(const MH_STATUS status, const char* context) {
    std::ostringstream stream;
    stream << context << " failed with MinHook status " << static_cast<int>(status) << '.';
    return Status::Error(ErrorCode::kDeviceError, stream.str());
}

void LogHookMessage(const char* category, const std::string& message) {
    chimera::common::LogInfo(category, message);
}

[[nodiscard]] Result<void> EnableHook(void* target) {
    const auto status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        return MhStatusToStatus(status, "MH_EnableHook");
    }
    return {};
}

[[nodiscard]] Result<void> CreateHook(void* target, void* detour, void** original, const char* context) {
    const auto status = MH_CreateHook(target, detour, original);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) {
        return MhStatusToStatus(status, context);
    }
    return {};
}

[[nodiscard]] Result<void> HookIat(
    HMODULE target_module,
    const char* function_name,
    void* detour_func,
    void** original_func) {
    
    if (target_module == nullptr || function_name == nullptr || detour_func == nullptr || original_func == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "Invalid arguments for HookIat.");
    }

    auto* dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(target_module);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        return Status::Error(ErrorCode::kInvalidArgument, "Invalid DOS signature.");
    }

    auto* nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<std::uintptr_t>(target_module) + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
        return Status::Error(ErrorCode::kInvalidArgument, "Invalid NT signature.");
    }

    DWORD imports_rva = nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (imports_rva == 0U) {
        return Status::Error(ErrorCode::kNotFound, "No import directory found.");
    }

    bool hooked_any = false;
    auto* import_desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(reinterpret_cast<std::uintptr_t>(target_module) + imports_rva);
    while (import_desc->Name != 0U) {
        auto* thunk_ilt = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<std::uintptr_t>(target_module) + import_desc->OriginalFirstThunk);
        auto* thunk_iat = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<std::uintptr_t>(target_module) + import_desc->FirstThunk);

        auto* lookup_thunk = thunk_ilt != nullptr && import_desc->OriginalFirstThunk != 0U ? thunk_ilt : thunk_iat;

        while (lookup_thunk->u1.Function != 0U) {
            const char* func_name = nullptr;
            if ((lookup_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) == 0U) {
                auto* import_by_name = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(reinterpret_cast<std::uintptr_t>(target_module) + lookup_thunk->u1.AddressOfData);
                func_name = import_by_name->Name;
            }

            if (func_name != nullptr && std::strcmp(func_name, function_name) == 0) {
                DWORD old_protect = 0;
                if (::VirtualProtect(&thunk_iat->u1.Function, sizeof(thunk_iat->u1.Function), PAGE_READWRITE, &old_protect) != FALSE) {
                    if (!hooked_any) {
                        *original_func = reinterpret_cast<void*>(thunk_iat->u1.Function);
                    }
                    thunk_iat->u1.Function = reinterpret_cast<std::uintptr_t>(detour_func);
                    ::VirtualProtect(&thunk_iat->u1.Function, sizeof(thunk_iat->u1.Function), old_protect, &old_protect);
                    hooked_any = true;
                }
            }

            lookup_thunk++;
            thunk_iat++;
        }

        import_desc++;
    }

    if (hooked_any) {
        return {};
    }
    return Status::Error(ErrorCode::kNotFound, "Function not found in imports.");
}

void HookSwapChain(IUnknown* unknown_swap_chain);
void HookFactory(IUnknown* unknown_factory);
void HookDevice(IUnknown* unknown_device);
void HookGraphicsCommandList(IUnknown* unknown_command_list);

void STDMETHODCALLTYPE HookedDeviceCreateRenderTargetView(
    ID3D12Device* device,
    ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    const D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle) {
    g_original_device_create_render_target_view(device, resource, desc, descriptor_handle);
    RegisterD3d12RenderTargetView(descriptor_handle, resource, desc);
}

void STDMETHODCALLTYPE HookedDeviceCreateDepthStencilView(
    ID3D12Device* device,
    ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
    const D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle) {
    g_original_device_create_depth_stencil_view(device, resource, desc, descriptor_handle);
    RegisterD3d12DepthStencilView(descriptor_handle, resource, desc);
}

HRESULT STDMETHODCALLTYPE HookedDeviceCreateCommandList(
    ID3D12Device* device,
    const UINT node_mask,
    const D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* command_allocator,
    ID3D12PipelineState* initial_state,
    REFIID riid,
    void** command_list) {
    const auto hr = g_original_device_create_command_list(
        device,
        node_mask,
        type,
        command_allocator,
        initial_state,
        riid,
        command_list);
    if (SUCCEEDED(hr) && command_list != nullptr && *command_list != nullptr) {
        HookGraphicsCommandList(static_cast<IUnknown*>(*command_list));
    }
    return hr;
}

void STDMETHODCALLTYPE HookedCommandListOMSetRenderTargets(
    ID3D12GraphicsCommandList* command_list,
    const UINT render_target_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* render_target_views,
    const BOOL single_handle_to_descriptor_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil_view) {
    ObserveD3d12RenderTargets(
        render_target_count,
        render_target_views,
        single_handle_to_descriptor_range != FALSE,
        depth_stencil_view);
    g_original_command_list_om_set_render_targets(
        command_list,
        render_target_count,
        render_target_views,
        single_handle_to_descriptor_range,
        depth_stencil_view);
}

HRESULT STDMETHODCALLTYPE HookedSwapChainPresent(IDXGISwapChain* swap_chain, const UINT sync_interval, const UINT flags) {
    const auto override_res = HandleD3d12Present(swap_chain, sync_interval, flags, nullptr);

    const auto remaining_budget = g_present_log_budget.fetch_sub(1U);
    if (remaining_budget > 0) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(swap_chain->GetDesc(&desc))) {
            std::ostringstream stream;
            stream
                << "Present syncInterval=" << sync_interval
                << " (overridden=" << override_res.overridden_sync_interval << ")"
                << " flags=0x" << std::hex << flags
                << " backBuffer=" << std::dec << desc.BufferDesc.Width << 'x' << desc.BufferDesc.Height
                << " handled=" << (override_res.handled ? "true" : "false");
            LogHookMessage("runtime.present", stream.str());
        } else {
            LogHookMessage("runtime.present", "Present intercepted.");
        }
    }

    if (override_res.handled) {
        return S_OK;
    }
    return g_original_swap_chain_present(swap_chain, override_res.overridden_sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE HookedSwapChainPresent1(
    IDXGISwapChain1* swap_chain,
    const UINT sync_interval,
    const UINT present_flags,
    const DXGI_PRESENT_PARAMETERS* present_parameters) {
    const auto override_res = HandleD3d12Present(swap_chain, sync_interval, present_flags, present_parameters);

    const auto remaining_budget = g_present_log_budget.fetch_sub(1U);
    if (remaining_budget > 0) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (SUCCEEDED(swap_chain->GetDesc1(&desc))) {
            std::ostringstream stream;
            stream
                << "Present1 syncInterval=" << sync_interval
                << " (overridden=" << override_res.overridden_sync_interval << ")"
                << " flags=0x" << std::hex << present_flags
                << " backBuffer=" << std::dec << desc.Width << 'x' << desc.Height
                << " handled=" << (override_res.handled ? "true" : "false");
            LogHookMessage("runtime.present", stream.str());
        } else {
            LogHookMessage("runtime.present", "Present1 intercepted.");
        }
    }

    if (override_res.handled) {
        return S_OK;
    }
    return g_original_swap_chain_present1(swap_chain, override_res.overridden_sync_interval, present_flags, present_parameters);
}

HRESULT STDMETHODCALLTYPE HookedSwapChainResizeBuffers(
    IDXGISwapChain* swap_chain,
    const UINT buffer_count,
    const UINT width,
    const UINT height,
    const DXGI_FORMAT new_format,
    const UINT swap_chain_flags) {
    std::ostringstream stream;
    stream
        << "ResizeBuffers buffers=" << buffer_count
        << " size=" << width << 'x' << height
        << " format=" << static_cast<int>(new_format)
        << " flags=0x" << std::hex << swap_chain_flags;
    LogHookMessage("runtime.swapchain", stream.str());

    HandleD3d12ResizeBuffers(swap_chain);
    return g_original_swap_chain_resize_buffers(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
}

HRESULT STDMETHODCALLTYPE HookedFactoryCreateSwapChain(
    IDXGIFactory* factory,
    IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** swap_chain) {
    const auto hr = g_original_factory_create_swap_chain(factory, device, desc, swap_chain);
    if (SUCCEEDED(hr) && swap_chain != nullptr && *swap_chain != nullptr) {
        LogHookMessage("runtime.factory", "CreateSwapChain intercepted.");
        HookSwapChain(*swap_chain);
        RegisterD3d12SwapChain(*swap_chain, device);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedFactoryCreateSwapChainForHwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND window_handle,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain) {
    const auto hr = g_original_factory_create_swap_chain_for_hwnd(
        factory,
        device,
        window_handle,
        desc,
        fullscreen_desc,
        restrict_to_output,
        swap_chain);
    if (SUCCEEDED(hr) && swap_chain != nullptr && *swap_chain != nullptr) {
        std::ostringstream stream;
        stream << "CreateSwapChainForHwnd intercepted for HWND=0x" << std::hex << reinterpret_cast<std::uintptr_t>(window_handle);
        LogHookMessage("runtime.factory", stream.str());
        HookSwapChain(*swap_chain);
        RegisterD3d12SwapChain(*swap_chain, device);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedFactoryCreateSwapChainForComposition(
    IDXGIFactory2* factory,
    IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain) {
    const auto hr = g_original_factory_create_swap_chain_for_composition(
        factory,
        device,
        desc,
        restrict_to_output,
        swap_chain);
    if (SUCCEEDED(hr) && swap_chain != nullptr && *swap_chain != nullptr) {
        LogHookMessage("runtime.factory", "CreateSwapChainForComposition intercepted.");
        HookSwapChain(*swap_chain);
        RegisterD3d12SwapChain(*swap_chain, device);
    }
    return hr;
}

HRESULT WINAPI HookedD3D12CreateDevice(
    IUnknown* adapter,
    const D3D_FEATURE_LEVEL minimum_feature_level,
    REFIID riid,
    void** device) {
    const auto hr = g_original_d3d12_create_device(adapter, minimum_feature_level, riid, device);
    if (SUCCEEDED(hr)) {
        std::ostringstream stream;
        stream
            << "D3D12CreateDevice succeeded featureLevel=0x"
            << std::hex << static_cast<unsigned>(minimum_feature_level)
            << " devicePtr=0x" << reinterpret_cast<std::uintptr_t>(device != nullptr ? *device : nullptr);
        LogHookMessage("runtime.d3d12", stream.str());
        if (device != nullptr && *device != nullptr) {
            HookDevice(static_cast<IUnknown*>(*device));
        }
    }
    return hr;
}

HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void** factory) {
    const auto hr = g_original_create_dxgi_factory1(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr) {
        LogHookMessage("runtime.dxgi", "CreateDXGIFactory1 intercepted.");
        HookFactory(static_cast<IUnknown*>(*factory));
    }
    return hr;
}

HRESULT WINAPI HookedCreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    const auto hr = g_original_create_dxgi_factory2(flags, riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr) {
        std::ostringstream stream;
        stream << "CreateDXGIFactory2 intercepted flags=0x" << std::hex << flags;
        LogHookMessage("runtime.dxgi", stream.str());
        HookFactory(static_cast<IUnknown*>(*factory));
    }
    return hr;
}

void HookFactory(IUnknown* unknown_factory) {
    if (unknown_factory == nullptr || g_factory_methods_hooked.load()) {
        return;
    }

    std::scoped_lock lock(g_hook_mutex);
    if (g_factory_methods_hooked.load()) {
        return;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory2{};
    if (FAILED(unknown_factory->QueryInterface(IID_PPV_ARGS(factory2.GetAddressOf())))) {
        return;
    }

    auto** vtable = *reinterpret_cast<void***>(factory2.Get());
    constexpr std::size_t kCreateSwapChainIndex = 10U;
    constexpr std::size_t kCreateSwapChainForHwndIndex = 15U;
    constexpr std::size_t kCreateSwapChainForCompositionIndex = 24U;

    if (!CreateHook(vtable[kCreateSwapChainIndex], reinterpret_cast<void*>(&HookedFactoryCreateSwapChain), reinterpret_cast<void**>(&g_original_factory_create_swap_chain), "MH_CreateHook(CreateSwapChain)").ok()) {
        return;
    }
    if (!CreateHook(vtable[kCreateSwapChainForHwndIndex], reinterpret_cast<void*>(&HookedFactoryCreateSwapChainForHwnd), reinterpret_cast<void**>(&g_original_factory_create_swap_chain_for_hwnd), "MH_CreateHook(CreateSwapChainForHwnd)").ok()) {
        return;
    }
    if (!CreateHook(vtable[kCreateSwapChainForCompositionIndex], reinterpret_cast<void*>(&HookedFactoryCreateSwapChainForComposition), reinterpret_cast<void**>(&g_original_factory_create_swap_chain_for_composition), "MH_CreateHook(CreateSwapChainForComposition)").ok()) {
        return;
    }

    (void)EnableHook(vtable[kCreateSwapChainIndex]);
    (void)EnableHook(vtable[kCreateSwapChainForHwndIndex]);
    (void)EnableHook(vtable[kCreateSwapChainForCompositionIndex]);
    g_factory_methods_hooked.store(true);
}

void HookSwapChain(IUnknown* unknown_swap_chain) {
    if (unknown_swap_chain == nullptr || g_swap_chain_methods_hooked.load()) {
        return;
    }

    std::scoped_lock lock(g_hook_mutex);
    if (g_swap_chain_methods_hooked.load()) {
        return;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain1{};
    if (FAILED(unknown_swap_chain->QueryInterface(IID_PPV_ARGS(swap_chain1.GetAddressOf())))) {
        return;
    }

    auto** vtable = *reinterpret_cast<void***>(swap_chain1.Get());
    constexpr std::size_t kPresentIndex = 8U;
    constexpr std::size_t kResizeBuffersIndex = 13U;
    constexpr std::size_t kPresent1Index = 22U;

    if (!CreateHook(vtable[kPresentIndex], reinterpret_cast<void*>(&HookedSwapChainPresent), reinterpret_cast<void**>(&g_original_swap_chain_present), "MH_CreateHook(Present)").ok()) {
        return;
    }
    if (!CreateHook(vtable[kResizeBuffersIndex], reinterpret_cast<void*>(&HookedSwapChainResizeBuffers), reinterpret_cast<void**>(&g_original_swap_chain_resize_buffers), "MH_CreateHook(ResizeBuffers)").ok()) {
        return;
    }
    if (!CreateHook(vtable[kPresent1Index], reinterpret_cast<void*>(&HookedSwapChainPresent1), reinterpret_cast<void**>(&g_original_swap_chain_present1), "MH_CreateHook(Present1)").ok()) {
        return;
    }

    (void)EnableHook(vtable[kPresentIndex]);
    (void)EnableHook(vtable[kResizeBuffersIndex]);
    (void)EnableHook(vtable[kPresent1Index]);
    g_swap_chain_methods_hooked.store(true);
}

void HookDevice(IUnknown* unknown_device) {
    if (unknown_device == nullptr || g_device_methods_hooked.load()) {
        return;
    }

    std::scoped_lock lock(g_hook_mutex);
    if (g_device_methods_hooked.load()) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    if (FAILED(unknown_device->QueryInterface(IID_PPV_ARGS(device.GetAddressOf())))) {
        return;
    }

    SetD3d12DescriptorIncrementSizes(
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV),
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV));

    auto** vtable = *reinterpret_cast<void***>(device.Get());
    constexpr std::size_t kCreateCommandListIndex = 12U;
    constexpr std::size_t kCreateRenderTargetViewIndex = 20U;
    constexpr std::size_t kCreateDepthStencilViewIndex = 21U;

    if (!CreateHook(vtable[kCreateCommandListIndex], reinterpret_cast<void*>(&HookedDeviceCreateCommandList), reinterpret_cast<void**>(&g_original_device_create_command_list), "MH_CreateHook(ID3D12Device::CreateCommandList)").ok()) {
        return;
    }
    if (!CreateHook(vtable[kCreateRenderTargetViewIndex], reinterpret_cast<void*>(&HookedDeviceCreateRenderTargetView), reinterpret_cast<void**>(&g_original_device_create_render_target_view), "MH_CreateHook(ID3D12Device::CreateRenderTargetView)").ok()) {
        return;
    }
    if (!CreateHook(vtable[kCreateDepthStencilViewIndex], reinterpret_cast<void*>(&HookedDeviceCreateDepthStencilView), reinterpret_cast<void**>(&g_original_device_create_depth_stencil_view), "MH_CreateHook(ID3D12Device::CreateDepthStencilView)").ok()) {
        return;
    }

    (void)EnableHook(vtable[kCreateCommandListIndex]);
    (void)EnableHook(vtable[kCreateRenderTargetViewIndex]);
    (void)EnableHook(vtable[kCreateDepthStencilViewIndex]);
    g_device_methods_hooked.store(true);
}

void HookGraphicsCommandList(IUnknown* unknown_command_list) {
    if (unknown_command_list == nullptr || g_command_list_methods_hooked.load()) {
        return;
    }

    std::scoped_lock lock(g_hook_mutex);
    if (g_command_list_methods_hooked.load()) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list{};
    if (FAILED(unknown_command_list->QueryInterface(IID_PPV_ARGS(command_list.GetAddressOf())))) {
        return;
    }

    auto** vtable = *reinterpret_cast<void***>(command_list.Get());
    constexpr std::size_t kOmSetRenderTargetsIndex = 46U;
    if (!CreateHook(vtable[kOmSetRenderTargetsIndex], reinterpret_cast<void*>(&HookedCommandListOMSetRenderTargets), reinterpret_cast<void**>(&g_original_command_list_om_set_render_targets), "MH_CreateHook(ID3D12GraphicsCommandList::OMSetRenderTargets)").ok()) {
        return;
    }

    (void)EnableHook(vtable[kOmSetRenderTargetsIndex]);
    g_command_list_methods_hooked.store(true);
}

BOOL WINAPI HookedCreateProcessW(
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    const DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation) {
    std::ostringstream start_stream;
    start_stream << "HookedCreateProcessW entry. AppName: " 
                 << (lpApplicationName ? NarrowAscii(lpApplicationName) : "null")
                 << " CommandLine: " 
                 << (lpCommandLine ? NarrowAscii(lpCommandLine) : "null");
    chimera::common::LogInfo("runtime.hooks", start_stream.str());

    const bool was_suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    const DWORD flags = dwCreationFlags | CREATE_SUSPENDED;

    const BOOL result = g_original_create_process_w(
        lpApplicationName,
        lpCommandLine,
        lpProcessAttributes,
        lpThreadAttributes,
        bInheritHandles,
        flags,
        lpEnvironment,
        lpCurrentDirectory,
        lpStartupInfo,
        lpProcessInformation);

    std::ostringstream result_stream;
    result_stream << "g_original_create_process_w returned: " << (result ? "TRUE" : "FALSE");
    if (result == FALSE) {
        result_stream << " GetLastError: " << ::GetLastError();
    }
    chimera::common::LogInfo("runtime.hooks", result_stream.str());

    if (result != FALSE && lpProcessInformation != nullptr) {
        HMODULE current_dll = nullptr;
        static int s_anchor = 0;
        if (::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&s_anchor),
                &current_dll) != FALSE) {
            wchar_t dll_path[MAX_PATH]{};
            if (::GetModuleFileNameW(current_dll, dll_path, MAX_PATH) > 0) {
                const auto inject_res = chimera::platform::win32::InjectDllWithLoadLibrary(
                    lpProcessInformation->hProcess,
                    std::filesystem::path(dll_path));
                if (!inject_res.ok()) {
                    std::ostringstream stream;
                    stream << "Failed to inject runtime DLL into child process " << lpProcessInformation->dwProcessId << ": " << inject_res.status().message;
                    chimera::common::LogError("runtime.hooks", stream.str());
                } else {
                    std::ostringstream stream;
                    stream << "Successfully injected runtime DLL into child process " << lpProcessInformation->dwProcessId << '.';
                    chimera::common::LogInfo("runtime.hooks", stream.str());
                }
            }
        }

        if (!was_suspended) {
            ::ResumeThread(lpProcessInformation->hThread);
        }
    }

    return result;
}

BOOL WINAPI HookedCreateProcessA(
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    const DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation) {
    std::ostringstream start_stream;
    start_stream << "HookedCreateProcessA entry. AppName: " 
                 << (lpApplicationName ? lpApplicationName : "null")
                 << " CommandLine: " 
                 << (lpCommandLine ? lpCommandLine : "null");
    chimera::common::LogInfo("runtime.hooks", start_stream.str());

    const bool was_suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    const DWORD flags = dwCreationFlags | CREATE_SUSPENDED;

    const BOOL result = g_original_create_process_a(
        lpApplicationName,
        lpCommandLine,
        lpProcessAttributes,
        lpThreadAttributes,
        bInheritHandles,
        flags,
        lpEnvironment,
        lpCurrentDirectory,
        lpStartupInfo,
        lpProcessInformation);

    if (result != FALSE && lpProcessInformation != nullptr) {
        HMODULE current_dll = nullptr;
        static int s_anchor = 0;
        if (::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&s_anchor),
                &current_dll) != FALSE) {
            wchar_t dll_path[MAX_PATH]{};
            if (::GetModuleFileNameW(current_dll, dll_path, MAX_PATH) > 0) {
                const auto inject_res = chimera::platform::win32::InjectDllWithLoadLibrary(
                    lpProcessInformation->hProcess,
                    std::filesystem::path(dll_path));
                if (!inject_res.ok()) {
                    std::ostringstream stream;
                    stream << "Failed to inject runtime DLL into child process " << lpProcessInformation->dwProcessId << ": " << inject_res.status().message;
                    chimera::common::LogError("runtime.hooks", stream.str());
                } else {
                    std::ostringstream stream;
                    stream << "Successfully injected runtime DLL into child process " << lpProcessInformation->dwProcessId << '.';
                    chimera::common::LogInfo("runtime.hooks", stream.str());
                }
            }
        }

        if (!was_suspended) {
            ::ResumeThread(lpProcessInformation->hThread);
        }
    }

    return result;
}

using GetCommandLineWFn = LPWSTR(WINAPI*)();
using GetCommandLineAFn = LPSTR(WINAPI*)();
GetCommandLineWFn g_original_get_command_line_w = nullptr;
GetCommandLineAFn g_original_get_command_line_a = nullptr;

void InitializeMinHookAndInstall() {
    if (MH_Initialize() == MH_OK) {
        g_minhook_initialized.store(true);
        const HMODULE d3d12_mod = ::GetModuleHandleW(L"d3d12.dll");
        const HMODULE dxgi_mod = ::GetModuleHandleW(L"dxgi.dll");

        if (d3d12_mod != nullptr) {
            (void)MH_CreateHookApi(
                L"d3d12.dll",
                "D3D12CreateDevice",
                reinterpret_cast<void*>(&HookedD3D12CreateDevice),
                reinterpret_cast<void**>(&g_original_d3d12_create_device));
        }
        if (dxgi_mod != nullptr) {
            (void)MH_CreateHookApi(
                L"dxgi.dll",
                "CreateDXGIFactory1",
                reinterpret_cast<void*>(&HookedCreateDXGIFactory1),
                reinterpret_cast<void**>(&g_original_create_dxgi_factory1));
            (void)MH_CreateHookApi(
                L"dxgi.dll",
                "CreateDXGIFactory2",
                reinterpret_cast<void*>(&HookedCreateDXGIFactory2),
                reinterpret_cast<void**>(&g_original_create_dxgi_factory2));
        }
        
        const auto status = MH_EnableHook(MH_ALL_HOOKS);
        LogHookMessage("runtime", "Game hooks enabled with status=" + std::to_string(status));
    }
}

LPWSTR WINAPI HookedGetCommandLineW() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        LogHookMessage("runtime", "HookedGetCommandLineW fired on main thread.");
        InitializeMinHookAndInstall();
    });

    return g_original_get_command_line_w();
}

LPSTR WINAPI HookedGetCommandLineA() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        LogHookMessage("runtime", "HookedGetCommandLineA fired on main thread.");
        InitializeMinHookAndInstall();
    });

    return g_original_get_command_line_a();
}

}  // namespace

Result<void> InstallHooks() {
    LogHookMessage("runtime", "InstallHooks started.");

    bool is_game = false;
    const auto exe_path_res = chimera::platform::win32::GetExecutablePath();
    if (exe_path_res.ok()) {
        std::wstring exe_lower = exe_path_res.value().filename().wstring();
        for (auto& c : exe_lower) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        if (exe_lower.find(L"shipping") != std::wstring::npos || 
            exe_lower.find(L"sample_app") != std::wstring::npos) {
            is_game = true;
        }
        std::ostringstream stream;
        stream << "Current executable: " << NarrowAscii(exe_path_res.value().filename().wstring()) << " (is_game=" << (is_game ? "true" : "false") << ")";
        LogHookMessage("runtime", stream.str());
    }

    if (is_game) {
        // Hook GetCommandLineW and GetCommandLineA via IAT to initialize MinHook hooks when the main thread starts running.
        // This completely avoids deadlocks or hangs due to suspended threads during remote injection.
        const auto hook_w_res = HookIat(
            ::GetModuleHandleW(nullptr),
            "GetCommandLineW",
            reinterpret_cast<void*>(&HookedGetCommandLineW),
            reinterpret_cast<void**>(&g_original_get_command_line_w));
        if (hook_w_res.ok()) {
            LogHookMessage("runtime", "GetCommandLineW hooked via IAT successfully.");
        } else {
            std::ostringstream stream;
            stream << "Failed to hook GetCommandLineW via IAT: " << hook_w_res.status().message;
            LogHookMessage("runtime", stream.str());
        }

        const auto hook_a_res = HookIat(
            ::GetModuleHandleW(nullptr),
            "GetCommandLineA",
            reinterpret_cast<void*>(&HookedGetCommandLineA),
            reinterpret_cast<void**>(&g_original_get_command_line_a));
        if (hook_a_res.ok()) {
            LogHookMessage("runtime", "GetCommandLineA hooked via IAT successfully.");
        } else {
            std::ostringstream stream;
            stream << "Failed to hook GetCommandLineA via IAT: " << hook_a_res.status().message;
            LogHookMessage("runtime", stream.str());
        }
    } else {
        // In the launcher/stub process, hook CreateProcessW and CreateProcessA directly via IAT during DllMain attach.
        // Since IAT hooking does not suspend threads or use MinHook, it is perfectly safe to do here.
        const auto hook_w_res = HookIat(
            ::GetModuleHandleW(nullptr),
            "CreateProcessW",
            reinterpret_cast<void*>(&HookedCreateProcessW),
            reinterpret_cast<void**>(&g_original_create_process_w));
        if (hook_w_res.ok()) {
            LogHookMessage("runtime", "CreateProcessW hooked via IAT successfully.");
        } else {
            std::ostringstream stream;
            stream << "Failed to hook CreateProcessW via IAT: " << hook_w_res.status().message;
            LogHookMessage("runtime", stream.str());
        }

        const auto hook_a_res = HookIat(
            ::GetModuleHandleW(nullptr),
            "CreateProcessA",
            reinterpret_cast<void*>(&HookedCreateProcessA),
            reinterpret_cast<void**>(&g_original_create_process_a));
        if (hook_a_res.ok()) {
            LogHookMessage("runtime", "CreateProcessA hooked via IAT successfully.");
        } else {
            std::ostringstream stream;
            stream << "Failed to hook CreateProcessA via IAT: " << hook_a_res.status().message;
            LogHookMessage("runtime", stream.str());
        }
    }

    LogHookMessage("runtime", "Bootstrap complete.");
    return {};
}

void ShutdownHooks() noexcept {
    if (!g_minhook_initialized.load()) {
        return;
    }

    ShutdownD3d12OverlayTracking();
    (void)MH_DisableHook(MH_ALL_HOOKS);
    (void)MH_Uninitialize();
    g_minhook_initialized.store(false);
}

HRESULT CallOriginalPresent(
    IUnknown* swap_chain,
    const UINT sync_interval,
    const UINT flags,
    const DXGI_PRESENT_PARAMETERS* present_parameters) {
    if (present_parameters != nullptr && g_original_swap_chain_present1 != nullptr) {
        Microsoft::WRL::ComPtr<IDXGISwapChain1> sc1{};
        if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(sc1.GetAddressOf())))) {
            return g_original_swap_chain_present1(sc1.Get(), sync_interval, flags, present_parameters);
        }
    }
    if (g_original_swap_chain_present != nullptr) {
        Microsoft::WRL::ComPtr<IDXGISwapChain> sc{};
        if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(sc.GetAddressOf())))) {
            return g_original_swap_chain_present(sc.Get(), sync_interval, flags);
        }
    }
    return E_FAIL;
}

}  // namespace chimera::loader::runtime
