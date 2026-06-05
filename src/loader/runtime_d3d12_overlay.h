#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <unknwn.h>

namespace chimera::loader::runtime {

void SetD3d12DescriptorIncrementSizes(UINT rtv_increment_size, UINT dsv_increment_size) noexcept;
void RegisterD3d12RenderTargetView(
    D3D12_CPU_DESCRIPTOR_HANDLE handle,
    ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc);
void RegisterD3d12DepthStencilView(
    D3D12_CPU_DESCRIPTOR_HANDLE handle,
    ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc);
void ObserveD3d12RenderTargets(
    UINT render_target_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* render_target_views,
    bool single_handle_to_descriptor_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil_view);
struct PresentOverrideResult {
    bool handled{false};
    UINT overridden_sync_interval{0U};
};

void RegisterD3d12SwapChain(IUnknown* swap_chain, IUnknown* device_or_queue);
PresentOverrideResult HandleD3d12Present(
    IUnknown* swap_chain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* present_parameters);
void HandleD3d12ResizeBuffers(IUnknown* swap_chain);
void ShutdownD3d12OverlayTracking() noexcept;

}  // namespace chimera::loader::runtime
