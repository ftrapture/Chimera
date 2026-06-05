#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

#include "common/result.h"

namespace chimera::common {

enum class LogLevel : int {
    kTrace = 0,
    kDebug,
    kInfo,
    kWarning,
    kError,
    kFatal,
};

struct LoggerConfig final {
    std::filesystem::path file_path{};
    LogLevel minimum_level{LogLevel::kInfo};
    bool log_to_console{true};
};

class Logger final {
public:
    static Logger& Get();

    [[nodiscard]] Result<void> Initialize(const LoggerConfig& config);
    void Shutdown();
    void Log(LogLevel level, std::string_view category, std::string_view message);

private:
    Logger() = default;

    [[nodiscard]] static std::string_view LevelToString(LogLevel level) noexcept;
    [[nodiscard]] static std::string TimestampString();

    mutable std::mutex mutex_{};
    std::ofstream file_stream_{};
    LogLevel minimum_level_{LogLevel::kInfo};
    bool log_to_console_{true};
};

template <typename... Args>
[[nodiscard]] std::string BuildLogMessage(Args&&... args) {
    std::ostringstream stream;
    (stream << ... << std::forward<Args>(args));
    return stream.str();
}

template <typename... Args>
void LogInfo(std::string_view category, Args&&... args) {
    Logger::Get().Log(LogLevel::kInfo, category, BuildLogMessage(std::forward<Args>(args)...));
}

template <typename... Args>
void LogWarning(std::string_view category, Args&&... args) {
    Logger::Get().Log(LogLevel::kWarning, category, BuildLogMessage(std::forward<Args>(args)...));
}

template <typename... Args>
void LogError(std::string_view category, Args&&... args) {
    Logger::Get().Log(LogLevel::kError, category, BuildLogMessage(std::forward<Args>(args)...));
}

}  // namespace chimera::common
