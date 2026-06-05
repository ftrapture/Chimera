#pragma once

#include <filesystem>
#include <string_view>

#include "common/result.h"
#include "config/config_types.h"

namespace chimera::config {

[[nodiscard]] RuntimeConfig MakeDefaultRuntimeConfig();
[[nodiscard]] chimera::common::Result<RuntimeConfig> LoadRuntimeConfigFromString(std::string_view json_text);
[[nodiscard]] chimera::common::Result<RuntimeConfig> LoadRuntimeConfigFromFile(const std::filesystem::path& path);

}  // namespace chimera::config
