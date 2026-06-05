#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <d3d12.h>

#include "common/runtime_types.h"

namespace chimera::resource_inspection {

struct ObservedDescriptor final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t sample_count{1U};
    bool is_depth{false};
};

struct InspectionSnapshot final {
    chimera::common::FrameSignals signals{};
    std::uint32_t suggested_render_width{0U};
    std::uint32_t suggested_render_height{0U};
};

class SignalInspector final {
public:
    void RegisterDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE handle,
        std::uintptr_t resource_id,
        const ObservedDescriptor& descriptor);

    void ObserveRenderTargets(
        std::uint32_t render_target_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE* render_target_views,
        bool single_handle_to_descriptor_range,
        const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil_view,
        std::uint32_t rtv_descriptor_increment_size);

    [[nodiscard]] InspectionSnapshot Evaluate(
        std::uint32_t display_width,
        std::uint32_t display_height,
        DXGI_FORMAT swapchain_format) const;

    void Reset() noexcept;

private:
    struct DescriptorRecord final {
        std::uintptr_t resource_id{0U};
        ObservedDescriptor descriptor{};
    };

    struct CandidateRecord final {
        ObservedDescriptor descriptor{};
        std::uint64_t bind_count{0U};
        std::uint64_t last_seen_tick{0U};
    };

    mutable std::mutex mutex_{};
    std::unordered_map<std::uint64_t, DescriptorRecord> descriptors_{};
    std::unordered_map<std::uintptr_t, CandidateRecord> color_candidates_{};
    std::unordered_map<std::uintptr_t, CandidateRecord> depth_candidates_{};
    std::uint64_t observation_tick_{0U};
};

}  // namespace chimera::resource_inspection
