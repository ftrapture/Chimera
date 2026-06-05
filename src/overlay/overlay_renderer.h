#pragma once

#include <cstdint>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "common/result.h"
#include "common/runtime_types.h"

namespace chimera::overlay {

struct OverlaySnapshot final {
    chimera::common::CapabilityTier tier{chimera::common::CapabilityTier::kDisabled};
    std::uint32_t render_width{0U};
    std::uint32_t render_height{0U};
    std::uint32_t display_width{0U};
    std::uint32_t display_height{0U};
    float scene_cpu_ms{0.0F};
    float sr_cpu_ms{0.0F};
    float overlay_cpu_ms{0.0F};
    bool history_reset{false};
    chimera::common::FrameSignals signals{};

    // Frame interpolation stats
    bool fi_enabled{false};
    float flow_cpu_ms{0.0F};
    float fi_cpu_ms{0.0F};
    float real_fps{0.0F};
    float effective_fps{0.0F};
    float pacing_latency_ms{0.0F};

    // User toggle states
    bool vsync_enabled{true};
    chimera::common::QualityMode quality_mode{chimera::common::QualityMode::kQuality};
};

class OverlayRenderer final {
public:
    [[nodiscard]] chimera::common::Result<void> Initialize(ID3D12Device* device, DXGI_FORMAT output_format);
    [[nodiscard]] chimera::common::Result<float> Draw(
        ID3D12GraphicsCommandList* command_list,
        const OverlaySnapshot& snapshot,
        D3D12_CPU_DESCRIPTOR_HANDLE output_rtv,
        std::uint32_t viewport_width,
        std::uint32_t viewport_height);

private:
    struct OverlayVertex final {
        float position[2];
        float color[4];
    };

    struct alignas(256) OverlayConstants final {
        float viewport_size[2];
        float padding[2];
    };

    void EmitQuad(float x0, float y0, float x1, float y1, const float color[4]);
    void EmitText(float x, float y, const char* text, const float color[4], float scale);
    void BuildGeometry(const OverlaySnapshot& snapshot);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_{};
    OverlayVertex* mapped_vertices_{nullptr};
    OverlayConstants* mapped_constants_{nullptr};
    std::vector<OverlayVertex> staging_vertices_{};
    std::uint32_t max_vertices_{0U};
};

}  // namespace chimera::overlay
