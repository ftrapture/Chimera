#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>
#include "common/result.h"

namespace chimera::d3d12 {

class SampleSceneRenderer final {
public:
    SampleSceneRenderer() = default;
    ~SampleSceneRenderer() = default;

    [[nodiscard]] chimera::common::Result<void> Initialize(ID3D12Device* device, std::uint32_t width, std::uint32_t height);
    [[nodiscard]] chimera::common::Result<void> Resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] float Render(
        ID3D12GraphicsCommandList* command_list,
        float time_seconds,
        const std::array<float, 2>& current_jitter,
        const std::array<float, 2>& previous_jitter);

    [[nodiscard]] ID3D12Resource* color() const noexcept { return color_.Get(); }
    [[nodiscard]] ID3D12Resource* motion() const noexcept { return motion_.Get(); }
    [[nodiscard]] ID3D12Resource* depth() const noexcept { return depth_.Get(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct alignas(256) SceneConstants final {
        float render_size[2];
        float inv_render_size[2];
        float jitter_pixels[2];
        float previous_jitter_pixels[2];
        float time_seconds;
        float padding0;
        float camera_offset[2];
        float previous_camera_offset[2];
    };

    [[nodiscard]] chimera::common::Result<void> CreatePipeline();
    [[nodiscard]] chimera::common::Result<void> CreateConstantBuffer();
    [[nodiscard]] chimera::common::Result<void> CreateRenderTargets();

    ID3D12Device* device_{nullptr};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_{};
    SceneConstants* mapped_constants_{nullptr};
    Microsoft::WRL::ComPtr<ID3D12Resource> color_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_{};
    D3D12_RESOURCE_STATES color_state_{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    D3D12_RESOURCE_STATES motion_state_{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    D3D12_RESOURCE_STATES depth_state_{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    std::uint32_t width_{0U};
    std::uint32_t height_{0U};
    UINT rtv_descriptor_size_{0U};
};

} // namespace chimera::d3d12
