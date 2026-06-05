#pragma once

#include <filesystem>

#include "common/runtime_types.h"

namespace chimera::config {

struct OverlayConfig final {
    bool enabled{true};
    bool show_confidence_details{true};
};

struct FrameInterpolationConfig final {
    bool enabled{true};
    float max_latency_ms{16.0F};
    float jitter_threshold_ms{4.0F};
    std::uint32_t stability_window_frames{32U};
};

struct SampleHarnessConfig final {
    std::uint32_t window_width{1600U};
    std::uint32_t window_height{900U};
    bool enable_debug_layer{true};
    bool enable_vsync{true};
    chimera::common::QualityMode quality_mode{chimera::common::QualityMode::kQuality};
    float sharpen_strength{0.20F};
};

struct RuntimeConfig final {
    std::filesystem::path log_file{"chimera.log"};
    OverlayConfig overlay{};
    SampleHarnessConfig sample_harness{};
    FrameInterpolationConfig frame_interpolation{};
    chimera::common::TierThresholds tier_thresholds{};
};

}  // namespace chimera::config
