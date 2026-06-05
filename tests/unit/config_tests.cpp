#include <catch2/catch_test_macros.hpp>

#include "config/config_loader.h"

TEST_CASE("Runtime config loader parses overrides", "[config]") {
    constexpr auto kJson = R"json(
{
  "log_file": "chimera_test.log",
  "overlay": {
    "enabled": false
  },
  "sample_harness": {
    "window_width": 1280,
    "window_height": 720,
    "quality_mode": "Balanced",
    "sharpen_strength": 0.33
  }
}
)json";

    const auto config_result = chimera::config::LoadRuntimeConfigFromString(kJson);
    REQUIRE(config_result.ok());
    CHECK(config_result.value().log_file == "chimera_test.log");
    CHECK(config_result.value().overlay.enabled == false);
    CHECK(config_result.value().sample_harness.window_width == 1280U);
    CHECK(config_result.value().sample_harness.window_height == 720U);
    CHECK(config_result.value().sample_harness.sharpen_strength == 0.33F);
    CHECK(config_result.value().sample_harness.quality_mode == chimera::common::QualityMode::kBalanced);
}

TEST_CASE("Runtime config loader rejects invalid quality modes", "[config]") {
    constexpr auto kJson = R"json(
{
  "sample_harness": {
    "quality_mode": "Impossible"
  }
}
)json";

    const auto config_result = chimera::config::LoadRuntimeConfigFromString(kJson);
    REQUIRE_FALSE(config_result.ok());
    CHECK(config_result.status().code == chimera::common::ErrorCode::kConfigError);
}

TEST_CASE("Quality mode parser accepts launcher-friendly aliases", "[config]") {
    const auto balanced = chimera::common::ParseQualityMode("balance");
    REQUIRE(balanced.ok());
    CHECK(balanced.value() == chimera::common::QualityMode::kBalanced);

    const auto high_performance = chimera::common::ParseQualityMode("high-performance");
    REQUIRE(high_performance.ok());
    CHECK(high_performance.value() == chimera::common::QualityMode::kPerformance);
}
