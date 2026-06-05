#pragma once

#include <array>
#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

#include "common/result.h"

namespace chimera::ofa {

static constexpr std::uint32_t kPyramidLevels = 5U;
static constexpr std::uint32_t kIterationsPerLevel = 3U;

struct FlowInputs final {
    ID3D12Resource* current_color{nullptr};
    ID3D12Resource* previous_color{nullptr};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint64_t frame_index{0U};
    bool reset{false};
    DXGI_FORMAT color_format{DXGI_FORMAT_R8G8B8A8_UNORM};
};

struct FlowOutputs final {
    ID3D12Resource* forward_flow{nullptr};
    ID3D12Resource* backward_flow{nullptr};
    ID3D12Resource* confidence{nullptr};
    float compute_ms{0.0F};
};

class OpticalFlowPipeline final {
public:
    [[nodiscard]] chimera::common::Result<void> Initialize(ID3D12Device* device);

    [[nodiscard]] chimera::common::Result<FlowOutputs> Execute(
        ID3D12GraphicsCommandList* command_list,
        const FlowInputs& inputs);

    void Reset() noexcept;

    [[nodiscard]] ID3D12Resource* forward_flow() const noexcept { return forward_flow_.Get(); }
    [[nodiscard]] ID3D12Resource* backward_flow() const noexcept { return backward_flow_.Get(); }
    [[nodiscard]] ID3D12Resource* confidence() const noexcept { return confidence_.Get(); }

private:
    struct alignas(256) PrefilterConstants final {
        std::uint32_t src_size[2];
        std::uint32_t dst_size[2];
        float src_texel_size[2];
        float dst_texel_size[2];
        std::uint32_t pyramid_level;
        std::uint32_t pad[3];
    };

    struct alignas(256) EstimateConstants final {
        std::uint32_t size[2];
        float texel_size[2];
        std::uint32_t iteration_index;
        std::uint32_t pyramid_level;
        std::uint32_t pad[2];
    };

    struct alignas(256) RefineConstants final {
        std::uint32_t size[2];
        float texel_size[2];
        float consistency_threshold;
        float pad[3];
    };

    struct alignas(256) MedianConstants final {
        std::uint32_t size[2];
        float texel_size[2];
    };

    [[nodiscard]] chimera::common::Result<void> CreateComputePipelines(ID3D12Device* device);
    [[nodiscard]] chimera::common::Result<void> EnsureResources(ID3D12Device* device, std::uint32_t width, std::uint32_t height);

    // Compute PSOs
    Microsoft::WRL::ComPtr<ID3D12RootSignature> prefilter_root_sig_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> prefilter_pso_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> estimate_root_sig_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> estimate_pso_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> refine_root_sig_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> refine_pso_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> median_root_sig_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> median_pso_{};

    // Descriptor heaps
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_uav_heap_{};
    UINT descriptor_size_{0U};

    // Constant buffers
    Microsoft::WRL::ComPtr<ID3D12Resource> cb_prefilter_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> cb_estimate_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> cb_refine_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> cb_median_{};
    PrefilterConstants* mapped_prefilter_{nullptr};
    EstimateConstants* mapped_estimate_{nullptr};
    RefineConstants* mapped_refine_{nullptr};
    MedianConstants* mapped_median_{nullptr};

    // Pyramid textures: [level] for current and previous
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kPyramidLevels> current_pyramid_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kPyramidLevels> previous_pyramid_{};
    std::array<std::uint32_t, kPyramidLevels> pyramid_widths_{};
    std::array<std::uint32_t, kPyramidLevels> pyramid_heights_{};

    // Flow textures per level (for iterative refinement)
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kPyramidLevels> forward_flow_pyramid_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kPyramidLevels> backward_flow_pyramid_{};

    // Final outputs (display resolution)
    Microsoft::WRL::ComPtr<ID3D12Resource> forward_flow_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> backward_flow_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> confidence_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> temp_flow_{};

    std::uint32_t width_{0U};
    std::uint32_t height_{0U};
    bool has_previous_frame_{false};
};

}  // namespace chimera::ofa
