#pragma once

#include <cstdint>
#include <string>

#include "common/ring_buffer.h"
#include "common/time.h"

namespace chimera::pacing {

struct PacingConfig final {
    float max_latency_budget_ms{16.0F};
    float target_fps_headroom{0.85F};
    std::uint32_t stability_window_frames{32U};
    float jitter_threshold_ms{4.0F};
    bool allow_vrr{true};
};

struct PacingDecision final {
    bool insert_interpolated_frame{false};
    float present_delay_ms{0.0F};
    float estimated_latency_ms{0.0F};
    float mean_frame_time_ms{0.0F};
    float stddev_frame_time_ms{0.0F};
    float effective_fps{0.0F};
    std::string reason{};
};

class FramePacer final {
public:
    explicit FramePacer(PacingConfig config = {}) : config_(config) {}

    void RecordFrameTime(float frame_time_ms);
    void RecordFiCost(float fi_cost_ms);
    void SetSceneTransition(bool is_transition) noexcept { scene_transition_ = is_transition; }

    [[nodiscard]] PacingDecision MakeDecision() const;
    [[nodiscard]] float MeanFrameTimeMs() const;
    [[nodiscard]] float StdDevFrameTimeMs() const;
    [[nodiscard]] bool IsStable() const;
    [[nodiscard]] float EstimatedBaseFps() const;

    void Reset() noexcept;

private:
    PacingConfig config_{};
    chimera::common::RingBuffer<float, 128> frame_times_{};
    float last_fi_cost_ms_{0.0F};
    bool scene_transition_{false};
    std::uint32_t consecutive_unstable_frames_{0U};
};

}  // namespace chimera::pacing
