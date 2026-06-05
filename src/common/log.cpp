#include "common/log.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace chimera::common {

Logger& Logger::Get() {
    static Logger logger;
    return logger;
}

Result<void> Logger::Initialize(const LoggerConfig& config) {
    std::scoped_lock lock(mutex_);
    minimum_level_ = config.minimum_level;
    log_to_console_ = config.log_to_console;

    if (!config.file_path.empty()) {
        file_stream_.open(config.file_path, std::ios::out | std::ios::trunc);
        if (!file_stream_.is_open()) {
            return Status::Error(ErrorCode::kIoError, "Failed to open log file.");
        }
    }

    return {};
}

void Logger::Shutdown() {
    std::scoped_lock lock(mutex_);
    if (file_stream_.is_open()) {
        file_stream_.flush();
        file_stream_.close();
    }
}

void Logger::Log(const LogLevel level, std::string_view category, std::string_view message) {
    if (level < minimum_level_) {
        return;
    }

    const auto line = TimestampString() + " [" + std::string(LevelToString(level)) + "] [" + std::string(category) + "] " + std::string(message);

    std::scoped_lock lock(mutex_);
    if (log_to_console_) {
        std::clog << line << '\n';
    }

    if (file_stream_.is_open()) {
        file_stream_ << line << '\n';
        file_stream_.flush();
    }
}

std::string_view Logger::LevelToString(const LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kTrace:
            return "TRACE";
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarning:
            return "WARN";
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kFatal:
            return "FATAL";
    }

    return "INFO";
}

std::string Logger::TimestampString() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_s(&local_time, &time);

    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

}  // namespace chimera::common
