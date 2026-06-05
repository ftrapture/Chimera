#include "config/config_loader.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace chimera::config {

namespace {

using chimera::common::ErrorCode;
using chimera::common::Result;
using chimera::common::Status;

}  // namespace

RuntimeConfig MakeDefaultRuntimeConfig() {
    return {};
}

Result<RuntimeConfig> LoadRuntimeConfigFromString(std::string_view json_text) {
    auto config = MakeDefaultRuntimeConfig();

    try {
        const auto json = nlohmann::json::parse(json_text.begin(), json_text.end());

        if (json.contains("log_file")) {
            config.log_file = json.at("log_file").get<std::string>();
        }

        if (json.contains("overlay")) {
            const auto& overlay_json = json.at("overlay");
            if (overlay_json.contains("enabled")) {
                config.overlay.enabled = overlay_json.at("enabled").get<bool>();
            }
            if (overlay_json.contains("show_confidence_details")) {
                config.overlay.show_confidence_details = overlay_json.at("show_confidence_details").get<bool>();
            }
        }

        if (json.contains("sample_harness")) {
            const auto& sample_json = json.at("sample_harness");
            if (sample_json.contains("window_width")) {
                config.sample_harness.window_width = sample_json.at("window_width").get<std::uint32_t>();
            }
            if (sample_json.contains("window_height")) {
                config.sample_harness.window_height = sample_json.at("window_height").get<std::uint32_t>();
            }
            if (sample_json.contains("enable_debug_layer")) {
                config.sample_harness.enable_debug_layer = sample_json.at("enable_debug_layer").get<bool>();
            }
            if (sample_json.contains("enable_vsync")) {
                config.sample_harness.enable_vsync = sample_json.at("enable_vsync").get<bool>();
            }
            if (sample_json.contains("sharpen_strength")) {
                config.sample_harness.sharpen_strength = sample_json.at("sharpen_strength").get<float>();
            }
            if (sample_json.contains("quality_mode")) {
                auto quality_mode = chimera::common::ParseQualityMode(sample_json.at("quality_mode").get<std::string>());
                if (!quality_mode.ok()) {
                    return quality_mode.status();
                }
                config.sample_harness.quality_mode = quality_mode.value();
            }
        }

        if (json.contains("frame_interpolation")) {
            const auto& fi_json = json.at("frame_interpolation");
            if (fi_json.contains("enabled")) {
                config.frame_interpolation.enabled = fi_json.at("enabled").get<bool>();
            }
            if (fi_json.contains("max_latency_ms")) {
                config.frame_interpolation.max_latency_ms = fi_json.at("max_latency_ms").get<float>();
            }
            if (fi_json.contains("jitter_threshold_ms")) {
                config.frame_interpolation.jitter_threshold_ms = fi_json.at("jitter_threshold_ms").get<float>();
            }
            if (fi_json.contains("stability_window_frames")) {
                config.frame_interpolation.stability_window_frames = fi_json.at("stability_window_frames").get<std::uint32_t>();
            }
        }

        if (json.contains("tier_thresholds")) {
            const auto& tier_json = json.at("tier_thresholds");
            auto& thresholds = config.tier_thresholds;
            if (tier_json.contains("temporal_color_min")) {
                thresholds.temporal_color_min = tier_json.at("temporal_color_min").get<float>();
            }
            if (tier_json.contains("temporal_depth_min")) {
                thresholds.temporal_depth_min = tier_json.at("temporal_depth_min").get<float>();
            }
            if (tier_json.contains("temporal_motion_min")) {
                thresholds.temporal_motion_min = tier_json.at("temporal_motion_min").get<float>();
            }
            if (tier_json.contains("temporal_jitter_min")) {
                thresholds.temporal_jitter_min = tier_json.at("temporal_jitter_min").get<float>();
            }
            if (tier_json.contains("fi_color_min")) {
                thresholds.fi_color_min = tier_json.at("fi_color_min").get<float>();
            }
            if (tier_json.contains("fi_ui_min")) {
                thresholds.fi_ui_min = tier_json.at("fi_ui_min").get<float>();
            }
            if (tier_json.contains("fi_depth_min")) {
                thresholds.fi_depth_min = tier_json.at("fi_depth_min").get<float>();
            }
            if (tier_json.contains("fi_motion_min")) {
                thresholds.fi_motion_min = tier_json.at("fi_motion_min").get<float>();
            }
        }
    } catch (const std::exception& exception) {
        return Status::Error(ErrorCode::kConfigError, exception.what());
    }

    return config;
}

Result<RuntimeConfig> LoadRuntimeConfigFromFile(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return Status::Error(ErrorCode::kIoError, "Failed to open config file.");
    }

    std::stringstream buffer;
    buffer << stream.rdbuf();
    return LoadRuntimeConfigFromString(buffer.str());
}

}  // namespace chimera::config
