#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"

namespace chimera::common {

enum class CapabilityTier : std::uint8_t {
    kDisabled = 0,
    kTierA,
    kTierB,
    kTierC,
};

enum class QualityMode : std::uint8_t {
    kUltraQuality = 0,
    kQuality,
    kBalanced,
    kPerformance,
};

enum class SignalSource : std::uint8_t {
    kMissing = 0,
    kHarness,
    kEngine,
    kIntercepted,
    kEstimated,
    kDerived,
};

enum SceneStateFlags : std::uint32_t {
    kSceneStateNone = 0U,
    kSceneStateMenu = 1U << 0U,
    kSceneStatePause = 1U << 1U,
    kSceneStateLoading = 1U << 2U,
    kSceneStateCutscene = 1U << 3U,
    kSceneStateResolutionChange = 1U << 4U,
};

struct SignalScore final {
    float confidence{0.0F};
    bool available{false};
    bool validated{false};
    SignalSource source{SignalSource::kMissing};
    std::string note{"missing"};
};

struct FrameSignals final {
    SignalScore color{};
    SignalScore depth{};
    SignalScore motion{};
    SignalScore jitter{};
    SignalScore exposure{};
    SignalScore ui{};
};

struct TierThresholds final {
    float temporal_color_min{0.90F};
    float temporal_depth_min{0.75F};
    float temporal_motion_min{0.75F};
    float temporal_jitter_min{0.60F};
    float fi_color_min{0.95F};
    float fi_ui_min{0.70F};
    float fi_depth_min{0.80F};
    float fi_motion_min{0.80F};
};

struct TierDecision final {
    CapabilityTier tier{CapabilityTier::kDisabled};
    bool allow_temporal_sr{false};
    bool allow_frame_interpolation{false};
    std::vector<std::string> reasons{};
};

struct PolicyDecision final {
    bool allowed{true};
    std::vector<std::string> reasons{};
};

struct AttachConfig final {
    std::filesystem::path executable_path{};
    std::filesystem::path working_directory{};
    bool allow_vulkan{true};
    bool allow_d3d12{true};
    bool enable_overlay{true};
};

struct PresentPlan final {
    bool allow_interpolated_present{false};
    bool use_vrr{false};
    std::uint32_t target_display_multiple{1U};
};

struct TelemetryPacket final {
    std::uint64_t frame_index{0U};
    CapabilityTier tier{CapabilityTier::kDisabled};
    float frame_time_ms{0.0F};
    float sr_time_ms{0.0F};
    float overlay_time_ms{0.0F};
    FrameSignals signals{};
};

class IBackendCapture {
public:
    virtual ~IBackendCapture() = default;
    [[nodiscard]] virtual Result<void> StartCapture() = 0;
    [[nodiscard]] virtual Result<void> StopCapture() = 0;
};

class IResourceInspector {
public:
    virtual ~IResourceInspector() = default;
    [[nodiscard]] virtual TierDecision Evaluate(FrameSignals signals, std::uint32_t scene_flags, bool stable_pacing) const = 0;
};

class IOpticalFlowProvider {
public:
    virtual ~IOpticalFlowProvider() = default;
    [[nodiscard]] virtual bool IsAvailable() const = 0;
};

class IInferenceBackend {
public:
    virtual ~IInferenceBackend() = default;
    [[nodiscard]] virtual bool IsAvailable() const = 0;
};

class IPresentScheduler {
public:
    virtual ~IPresentScheduler() = default;
    [[nodiscard]] virtual PresentPlan MakePlan() const = 0;
};

[[nodiscard]] inline constexpr float RenderFraction(const QualityMode mode) noexcept {
    switch (mode) {
        case QualityMode::kUltraQuality:
            return 0.77F;
        case QualityMode::kQuality:
            return 0.67F;
        case QualityMode::kBalanced:
            return 0.59F;
        case QualityMode::kPerformance:
            return 0.50F;
    }

    return 0.67F;
}

[[nodiscard]] inline constexpr std::string_view ToString(const CapabilityTier tier) noexcept {
    switch (tier) {
        case CapabilityTier::kDisabled:
            return "Disabled";
        case CapabilityTier::kTierA:
            return "Tier A";
        case CapabilityTier::kTierB:
            return "Tier B";
        case CapabilityTier::kTierC:
            return "Tier C";
    }

    return "Unknown";
}

[[nodiscard]] inline constexpr std::string_view ToString(const QualityMode mode) noexcept {
    switch (mode) {
        case QualityMode::kUltraQuality:
            return "UltraQuality";
        case QualityMode::kQuality:
            return "Quality";
        case QualityMode::kBalanced:
            return "Balanced";
        case QualityMode::kPerformance:
            return "Performance";
    }

    return "Quality";
}

[[nodiscard]] inline std::string NormalizeQualityModeToken(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());

    for (const char ch : text) {
        if (ch == ' ' || ch == '-' || ch == '_') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    return normalized;
}

[[nodiscard]] inline Result<QualityMode> ParseQualityMode(std::string_view text) {
    const auto normalized = NormalizeQualityModeToken(text);
    if (normalized == "ultraquality" || normalized == "ultra") {
        return QualityMode::kUltraQuality;
    }
    if (normalized == "quality") {
        return QualityMode::kQuality;
    }
    if (normalized == "balanced" || normalized == "balance") {
        return QualityMode::kBalanced;
    }
    if (normalized == "performance" || normalized == "perf" || normalized == "highperformance" || normalized == "highperf") {
        return QualityMode::kPerformance;
    }

    return Status::Error(ErrorCode::kConfigError, "Unsupported quality mode.");
}

[[nodiscard]] inline FrameSignals SanitizeSignals(FrameSignals signals) {
    if (signals.motion.source == SignalSource::kEstimated && !signals.motion.validated && signals.motion.confidence > 0.60F) {
        signals.motion.confidence = 0.60F;
    }

    if (signals.jitter.source == SignalSource::kDerived && !signals.jitter.validated && signals.jitter.confidence > 0.70F) {
        signals.jitter.confidence = 0.70F;
    }

    if (signals.ui.source == SignalSource::kDerived && !signals.ui.validated && signals.ui.confidence > 0.65F) {
        signals.ui.confidence = 0.65F;
    }

    return signals;
}

[[nodiscard]] inline TierDecision EvaluateTierDecision(
    FrameSignals signals,
    const std::uint32_t scene_flags,
    const bool stable_pacing,
    const TierThresholds& thresholds = {}) {
    signals = SanitizeSignals(signals);

    TierDecision decision{};

    const auto temporal_ok =
        signals.color.confidence >= thresholds.temporal_color_min &&
        signals.depth.confidence >= thresholds.temporal_depth_min &&
        signals.motion.confidence >= thresholds.temporal_motion_min &&
        signals.jitter.confidence >= thresholds.temporal_jitter_min;

    const auto fi_scene_ok = scene_flags == kSceneStateNone;
    const auto fi_ok =
        temporal_ok &&
        stable_pacing &&
        fi_scene_ok &&
        signals.color.confidence >= thresholds.fi_color_min &&
        signals.ui.confidence >= thresholds.fi_ui_min &&
        signals.depth.confidence >= thresholds.fi_depth_min &&
        signals.motion.confidence >= thresholds.fi_motion_min;

    if (!signals.color.available) {
        decision.tier = CapabilityTier::kDisabled;
        decision.reasons.emplace_back("No usable color input.");
        return decision;
    }

    decision.allow_temporal_sr = temporal_ok;
    decision.allow_frame_interpolation = fi_ok;

    const auto direct_signals =
        (signals.color.source == SignalSource::kHarness || signals.color.source == SignalSource::kEngine) &&
        (signals.depth.source == SignalSource::kHarness || signals.depth.source == SignalSource::kEngine) &&
        (signals.motion.source == SignalSource::kHarness || signals.motion.source == SignalSource::kEngine) &&
        (signals.jitter.source == SignalSource::kHarness || signals.jitter.source == SignalSource::kEngine);

    if (temporal_ok && direct_signals) {
        decision.tier = CapabilityTier::kTierA;
    } else if (temporal_ok) {
        decision.tier = CapabilityTier::kTierB;
    } else {
        decision.tier = CapabilityTier::kTierC;
    }

    if (!temporal_ok) {
        decision.reasons.emplace_back("Temporal SR disabled due to weak depth, motion, or jitter confidence.");
    }

    if (!fi_ok) {
        if (!stable_pacing) {
            decision.reasons.emplace_back("FI disabled because pacing is unstable.");
        }
        if (!fi_scene_ok) {
            decision.reasons.emplace_back("FI disabled because scene state is not interpolation safe.");
        }
        if (signals.ui.confidence < thresholds.fi_ui_min) {
            decision.reasons.emplace_back("FI disabled because UI separation confidence is below threshold.");
        }
    }

    return decision;
}

}  // namespace chimera::common
