#pragma once

#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

#include "common/result.h"

namespace chimera::fg {

struct FiInputs final {
    ID3D12Resource* frame_n{nullptr};
    ID3D12Resource* frame_n1{nullptr};
    ID3D12Resource* forward_flow{nullptr};
    ID3D12Resource* backward_flow{nullptr};
    ID3D12Resource* flow_confidence{nullptr};
    float interpolation_factor{0.5F};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    DXGI_FORMAT color_format{DXGI_FORMAT_R8G8B8A8_UNORM};
};

struct FiStats final {
    float warp_ms{0.0F};
    float blend_ms{0.0F};
    float total_ms{0.0F};
};

class FrameInterpolationPipeline final {
public:
    [[nodiscard]] chimera::common::Result<void> Initialize(ID3D12Device* device, DXGI_FORMAT output_format);

    [[nodiscard]] chimera::common::Result<FiStats> Execute(
        ID3D12GraphicsCommandList* command_list,
        const FiInputs& inputs,
        D3D12_CPU_DESCRIPTOR_HANDLE output_rtv);

    [[nodiscard]] ID3D12Resource* interpolated_frame() const noexcept { return interpolated_.Get(); }

private:
    struct alignas(256) WarpConstants final {
        std::uint32_t size[2];
        float texel_size[2];
        float interpolation_t;
        float pad[3];
    };

    struct alignas(256) BlendConstants final {
        std::uint32_t size[2];
        float texel_size[2];
        float interpolation_t;
        float pad[3];
    };

    [[nodiscard]] chimera::common::Result<void> CreatePipelines(ID3D12Device* device);
    [[nodiscard]] chimera::common::Result<void> EnsureResources(ID3D12Device* device, std::uint32_t width, std::uint32_t height);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> warp_root_sig_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> warp_pso_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> blend_root_sig_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> blend_pso_{};

    // Copy-to-backbuffer pass (simple fullscreen blit)
    Microsoft::WRL::ComPtr<ID3D12RootSignature> blit_root_sig_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> blit_pso_{};

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_{};
    UINT descriptor_size_{0U};

    Microsoft::WRL::ComPtr<ID3D12Resource> cb_warp_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> cb_blend_{};
    WarpConstants* mapped_warp_{nullptr};
    BlendConstants* mapped_blend_{nullptr};

    // Intermediate textures
    Microsoft::WRL::ComPtr<ID3D12Resource> warped_from_n_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> warped_from_n1_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> occlusion_map_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> interpolated_{};

    std::uint32_t width_{0U};
    std::uint32_t height_{0U};
    DXGI_FORMAT output_format_{DXGI_FORMAT_R8G8B8A8_UNORM};
};

}  // namespace chimera::fg
