#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/runtime_types.h"
#include "loader/attach_session.h"

namespace {

[[nodiscard]] std::string NarrowAscii(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t ch : text) {
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

[[nodiscard]] std::filesystem::path RuntimeLogPathForPid(const DWORD process_id) {
    wchar_t temp_path[MAX_PATH]{};
    const auto temp_length = ::GetTempPathW(static_cast<DWORD>(MAX_PATH), temp_path);
    std::filesystem::path path = temp_length > 0U ? std::filesystem::path(temp_path) : std::filesystem::temp_directory_path();
    path /= "chimera_runtime_" + std::to_string(process_id) + ".log";
    return path;
}

void PrintUsage() {
    std::wcerr
        << L"Usage:\n"
        << L"  chimera_launcher.exe --offline [--quality-mode <mode>] <game.exe> [game args...]\n"
        << L"  chimera_launcher.exe --offline --cwd <dir> [--stream-log] [--quality-mode <mode>] <game.exe> [game args...]\n"
        << L"Options:\n"
        << L"  --offline   Required. Confirms the target is an offline/unprotected title.\n"
        << L"  --no-wait   Return after launch instead of waiting for target exit.\n"
        << L"  --stream-log  Follow the injected runtime log in this terminal.\n"
        << L"  --quality-mode <mode>  UltraQuality | Quality | Balanced | Performance.\n";
}

int StreamRuntimeLog(chimera::loader::AttachSession& session, const std::filesystem::path& log_path) {
    std::cout << "Streaming runtime log from " << log_path.string() << '\n';

    std::ifstream stream{};
    std::streamoff last_position = 0;

    auto flush_new_lines = [&]() {
        if (!stream.is_open()) {
            stream.open(log_path);
            if (!stream.is_open()) {
                return;
            }
            last_position = 0;
        }

        stream.clear();
        stream.seekg(last_position);

        std::string line;
        while (std::getline(stream, line)) {
            std::cout << line << '\n';
        }

        const auto position = stream.tellg();
        if (position >= 0) {
            last_position = position;
        } else if (std::filesystem::exists(log_path)) {
            last_position = static_cast<std::streamoff>(std::filesystem::file_size(log_path));
        }
    };

    while (true) {
        flush_new_lines();

        const auto exit_result = session.WaitForExit(250U);
        if (exit_result.ok()) {
            flush_new_lines();
            return static_cast<int>(exit_result.value());
        }
        if (exit_result.status().code != chimera::common::ErrorCode::kTimeout) {
            std::cerr << exit_result.status().message << '\n';
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    chimera::loader::LaunchRequest request{};
    std::filesystem::path executable_path{};
    std::wstringstream target_arguments{};
    bool stream_runtime_log = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--offline") {
            request.declared_offline = true;
            continue;
        }
        if (argument == L"--no-wait") {
            request.wait_for_exit = false;
            continue;
        }
        if (argument == L"--stream-log") {
            stream_runtime_log = true;
            continue;
        }
        if (argument == L"--cwd") {
            if (index + 1 >= argc) {
                std::wcerr << L"--cwd requires a directory argument.\n";
                return 1;
            }
            request.working_directory = argv[++index];
            continue;
        }
        if (argument == L"--quality-mode" || argument == L"--mode") {
            if (index + 1 >= argc) {
                std::wcerr << L"--quality-mode requires a mode argument.\n";
                return 1;
            }

            const auto parse_result = chimera::common::ParseQualityMode(NarrowAscii(argv[++index]));
            if (!parse_result.ok()) {
                std::cerr << parse_result.status().message << '\n';
                return 1;
            }
            request.requested_quality_mode = parse_result.value();
            continue;
        }

        if (executable_path.empty()) {
            executable_path = argument;
        } else {
            if (target_arguments.tellp() > 0) {
                target_arguments << L' ';
            }
            target_arguments << chimera::platform::win32::QuoteCommandLineArgument(argument);
        }
    }

    if (executable_path.empty()) {
        PrintUsage();
        return 1;
    }

    request.executable_path = executable_path;
    request.target_arguments = target_arguments.str();

    const auto logger_status = chimera::common::Logger::Get().Initialize(
        {std::filesystem::path("chimera_launcher.log"), chimera::common::LogLevel::kInfo, true});
    if (!logger_status.ok()) {
        std::wcerr << L"Failed to initialize launcher logger.\n";
        return 1;
    }

    chimera::loader::AttachSession session{};
    const auto launch_result = session.LaunchAndAttach(request);
    if (!launch_result.ok()) {
        std::cerr << launch_result.status().message << '\n';
        chimera::common::Logger::Get().Shutdown();
        return 1;
    }

    std::wcout << L"Chimera attached to process " << session.process_id() << L".\n";
    const auto runtime_log_path = RuntimeLogPathForPid(session.process_id());
    std::wcout << L"Runtime log: " << runtime_log_path.wstring() << L'\n';
    std::wcout << L"Requested quality mode: " << std::wstring(chimera::common::ToString(request.requested_quality_mode).begin(), chimera::common::ToString(request.requested_quality_mode).end()) << L'\n';

    if (!request.wait_for_exit && !stream_runtime_log) {
        chimera::common::Logger::Get().Shutdown();
        return 0;
    }

    if (stream_runtime_log) {
        request.wait_for_exit = true;
        const auto exit_code = StreamRuntimeLog(session, runtime_log_path);
        chimera::common::Logger::Get().Shutdown();
        return exit_code;
    }

    const auto exit_result = session.WaitForExit();
    chimera::common::Logger::Get().Shutdown();
    if (!exit_result.ok()) {
        std::cerr << exit_result.status().message << '\n';
        return 1;
    }

    return static_cast<int>(exit_result.value());
}
