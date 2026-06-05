#include "pacing/frame_pacer.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace chimera::pacing {

void FramePacer::RecordFrameTime(const float frame_time_ms) {
    frame_times_.push_back(frame_time_ms);
}

void FramePacer::RecordFiCost(const float fi_cost_ms) {
    last_fi_cost_ms_ = fi_cost_ms;
}

float FramePacer::MeanFrameTimeMs() const {
    if (frame_times_.empty()) {
        return 16.67F;
    }

    const auto count = frame_times_.size();
    const auto window = (std::min)(count, static_cast<std::size_t>(config_.stability_window_frames));
    float sum = 0.0F;
    for (std::size_t i = count - window; i < count; ++i) {
        sum += frame_times_[i];
    }
    return sum / static_cast<float>(window);
}

float FramePacer::StdDevFrameTimeMs() const {
    if (frame_times_.size() < 2U) {
        return 0.0F;
    }

    const auto count = frame_times_.size();
    const auto window = (std::min)(count, static_cast<std::size_t>(config_.stability_window_frames));
    const auto mean = MeanFrameTimeMs();

    float variance_sum = 0.0F;
    for (std::size_t i = count - window; i < count; ++i) {
        const auto delta = frame_times_[i] - mean;
        variance_sum += delta * delta;
    }
    return std::sqrt(variance_sum / static_cast<float>(window));
}

bool FramePacer::IsStable() const {
    if (frame_times_.size() < config_.stability_window_frames) {
        return false;
    }
    return StdDevFrameTimeMs() < config_.jitter_threshold_ms;
}

float FramePacer::EstimatedBaseFps() const {
    const auto mean = MeanFrameTimeMs();
    if (mean < 0.001F) {
        return 0.0F;
    }
    return 1000.0F / mean;
}

PacingDecision FramePacer::MakeDecision() const {
    PacingDecision decision{};
    decision.mean_frame_time_ms = MeanFrameTimeMs();
    decision.stddev_frame_time_ms = StdDevFrameTimeMs();

    const auto base_fps = EstimatedBaseFps();

    // ── Check minimum frame count ───────────────────────────────────────
    if (frame_times_.size() < 8U) {
        decision.reason = "Insufficient frame history.";
        decision.effective_fps = base_fps;
        return decision;
    }

    // ── Check scene transition ──────────────────────────────────────────
    if (scene_transition_) {
        decision.reason = "Scene transition detected.";
        decision.effective_fps = base_fps;
        return decision;
    }

    // ── Check cadence stability ─────────────────────────────────────────
    if (!IsStable()) {
        decision.reason = "Frame time jitter exceeds threshold.";
        decision.effective_fps = base_fps;
        return decision;
    }

    // ── Check latency budget ────────────────────────────────────────────
    const auto total_latency = decision.mean_frame_time_ms + last_fi_cost_ms_;
    decision.estimated_latency_ms = total_latency;

    if (total_latency > config_.max_latency_budget_ms * 2.0F) {
        decision.reason = "Total latency exceeds budget.";
        decision.effective_fps = base_fps;
        return decision;
    }

    // ── Check if base FPS is high enough to benefit from FI ─────────────
    // Don't interpolate if already running > 120 FPS
    if (base_fps > 120.0F) {
        decision.reason = "Base FPS already high enough.";
        decision.effective_fps = base_fps;
        return decision;
    }

    // Don't interpolate if base FPS is too low (< 20 FPS)
    if (base_fps < 20.0F) {
        decision.reason = "Base FPS too low for quality interpolation.";
        decision.effective_fps = base_fps;
        return decision;
    }

    // ── All checks passed: enable FI ────────────────────────────────────
    decision.insert_interpolated_frame = true;

    // Present delay: insert the interpolated frame at the midpoint
    // between two real frames for even cadence
    decision.present_delay_ms = decision.mean_frame_time_ms * 0.5F;

    // Effective FPS is ~2× base
    decision.effective_fps = base_fps * 2.0F;
    decision.reason = "FI enabled.";

    return decision;
}

void FramePacer::Reset() noexcept {
    frame_times_ = {};
    last_fi_cost_ms_ = 0.0F;
    scene_transition_ = false;
    consecutive_unstable_frames_ = 0U;
}

}  // namespace chimera::pacing
