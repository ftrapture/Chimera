#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/result.h"
#include "common/runtime_types.h"

namespace chimera::loader {

struct LaunchRequest final {
    std::filesystem::path executable_path{};
    std::wstring target_arguments{};
    std::filesystem::path working_directory{};
    chimera::common::QualityMode requested_quality_mode{chimera::common::QualityMode::kQuality};
    bool declared_offline{false};
    bool wait_for_exit{true};
};

[[nodiscard]] chimera::common::PolicyDecision EvaluatePolicy(const LaunchRequest& request);

}  // namespace chimera::loader
