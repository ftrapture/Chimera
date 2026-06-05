#include <catch2/catch_test_macros.hpp>

#include "common/runtime_types.h"

TEST_CASE("Estimated motion is capped before validation", "[confidence]") {
    chimera::common::FrameSignals signals{};
    signals.color = {1.0F, true, true, chimera::common::SignalSource::kIntercepted, "color"};
    signals.depth = {0.9F, true, true, chimera::common::SignalSource::kIntercepted, "depth"};
    signals.motion = {0.95F, true, false, chimera::common::SignalSource::kEstimated, "estimated motion"};
    signals.jitter = {0.8F, true, true, chimera::common::SignalSource::kDerived, "derived jitter"};
    signals.ui = {0.9F, true, true, chimera::common::SignalSource::kDerived, "ui"};

    const auto decision = chimera::common::EvaluateTierDecision(signals, chimera::common::kSceneStateNone, true);
    CHECK(decision.tier == chimera::common::CapabilityTier::kTierC);
    CHECK_FALSE(decision.allow_temporal_sr);
}

TEST_CASE("Direct harness signals enable Tier A and FI", "[confidence]") {
    chimera::common::FrameSignals signals{};
    signals.color = {1.0F, true, true, chimera::common::SignalSource::kHarness, "color"};
    signals.depth = {0.95F, true, true, chimera::common::SignalSource::kHarness, "depth"};
    signals.motion = {0.95F, true, true, chimera::common::SignalSource::kHarness, "motion"};
    signals.jitter = {1.0F, true, true, chimera::common::SignalSource::kHarness, "jitter"};
    signals.ui = {0.92F, true, true, chimera::common::SignalSource::kHarness, "ui"};

    const auto decision = chimera::common::EvaluateTierDecision(signals, chimera::common::kSceneStateNone, true);
    CHECK(decision.tier == chimera::common::CapabilityTier::kTierA);
    CHECK(decision.allow_temporal_sr);
    CHECK(decision.allow_frame_interpolation);
}

TEST_CASE("Unsafe scene states disable FI", "[confidence]") {
    chimera::common::FrameSignals signals{};
    signals.color = {1.0F, true, true, chimera::common::SignalSource::kHarness, "color"};
    signals.depth = {0.95F, true, true, chimera::common::SignalSource::kHarness, "depth"};
    signals.motion = {0.95F, true, true, chimera::common::SignalSource::kHarness, "motion"};
    signals.jitter = {1.0F, true, true, chimera::common::SignalSource::kHarness, "jitter"};
    signals.ui = {0.92F, true, true, chimera::common::SignalSource::kHarness, "ui"};

    const auto decision = chimera::common::EvaluateTierDecision(signals, chimera::common::kSceneStateMenu, true);
    CHECK(decision.tier == chimera::common::CapabilityTier::kTierA);
    CHECK(decision.allow_temporal_sr);
    CHECK_FALSE(decision.allow_frame_interpolation);
}
