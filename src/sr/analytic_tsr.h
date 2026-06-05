#pragma once

#include <array>
#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

#include "common/result.h"
#include "sr/history.h"

namespace chimera::sr {

struct SrInputs final {
    ID3D12Resource* low_res_color{nullptr};
    ID3D12Resource* low_res_depth{nullptr};
    ID3D12Resource* low_res_motion{nullptr};
    std::uint32_t input_width{0U};
    std::uint32_t input_height{0U};
    std::uint32_t output_width{0U};
    std::uint32_t output_height{0U};
    std::array<float, 2> current_jitter_pixels{0.0F, 0.0F};
    std::array<float, 2> previous_jitter_pixels{0.0F, 0.0F};
    float exposure{1.0F};
    float sharpen_strength{0.20F};
    float variance_clip_gamma{1.0F};
    float anti_flicker_strength{0.50F};
    bool reset_history{false};
    std::uint64_t frame_index{0U};
};

struct SrPassStats final {
    float accumulate_cpu_ms{0.0F};
    float sharpen_cpu_ms{0.0F};
    bool history_reset{false};
};

class AnalyticTsrPipeline final {
public:
    [[nodiscard]] chimera::common::Result<void> Initialize(ID3D12Device* device, DXGI_FORMAT display_format);
    [[nodiscard]] chimera::common::Result<SrPassStats> Execute(
        ID3D12GraphicsCommandList* command_list,
        const SrInputs& inputs,
        D3D12_CPU_DESCRIPTOR_HANDLE output_rtv);

    void ResetHistory() noexcept { history_state_.Reset(); }

private:
    struct alignas(256) TsrConstants final {
        float input_size[2];
        float output_size[2];
        float current_jitter_pixels[2];
        float previous_jitter_pixels[2];
        float input_texel_size[2];
        float output_texel_size[2];
        float exposure{1.0F};
        float sharpen_strength{0.20F};
        float motion_vector_scale{1.0F};
        float history_valid{0.0F};
        float variance_clip_gamma{1.0F};
        float reactive_strength{0.0F};
        std::uint32_t frame_index{0U};
        float anti_flicker_strength{0.50F};
    };

    [[nodiscard]] chimera::common::Result<void> EnsureOutputResources(ID3D12Device* device, std::uint32_t width, std::uint32_t height);
    [[nodiscard]] chimera::common::Result<void> CreatePipelineObjects(ID3D12Device* device);

    ID3D12Resource* CurrentHistoryTexture() const noexcept { return history_textures_[write_history_index_].Get(); }
    ID3D12Resource* PreviousHistoryTexture() const noexcept { return history_textures_[1U - write_history_index_].Get(); }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> accumulate_root_signature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> accumulate_pso_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> sharpen_root_signature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> sharpen_pso_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_{};
    TsrConstants* mapped_constants_{nullptr};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> history_textures_{};
    std::uint32_t output_width_{0U};
    std::uint32_t output_height_{0U};
    std::uint32_t write_history_index_{0U};
    UINT srv_descriptor_size_{0U};
    UINT rtv_descriptor_size_{0U};
    DXGI_FORMAT display_format_{DXGI_FORMAT_R8G8B8A8_UNORM};
    HistoryState history_state_{};
};

}  // namespace chimera::sr
