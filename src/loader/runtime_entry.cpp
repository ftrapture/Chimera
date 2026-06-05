#include <Windows.h>

#include <filesystem>
#include <string>
#include <sstream>

#include "common/log.h"
#include "loader/runtime_hooks.h"

namespace {

DWORD WINAPI RuntimeBootstrapThread(LPVOID) {
    wchar_t temp_path[MAX_PATH]{};
    const auto temp_length = ::GetTempPathW(static_cast<DWORD>(MAX_PATH), temp_path);
    std::filesystem::path log_path = temp_length > 0U ? std::filesystem::path(temp_path) : std::filesystem::temp_directory_path();
    log_path /= "chimera_runtime_" + std::to_string(::GetCurrentProcessId()) + ".log";

    (void)chimera::common::Logger::Get().Initialize(
        {log_path, chimera::common::LogLevel::kInfo, true});

    std::ostringstream stream;
    stream << "Injected into process " << ::GetCurrentProcessId() << '.';
    chimera::common::LogInfo("runtime", stream.str());

    const auto hook_result = chimera::loader::runtime::InstallHooks();
    if (!hook_result.ok()) {
        chimera::common::LogError("runtime", hook_result.status().message);
    }

    return 0U;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module_handle, const DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            ::DisableThreadLibraryCalls(module_handle);
            const HANDLE thread_handle = ::CreateThread(nullptr, 0U, &RuntimeBootstrapThread, nullptr, 0U, nullptr);
            if (thread_handle != nullptr) {
                ::CloseHandle(thread_handle);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            chimera::loader::runtime::ShutdownHooks();
            chimera::common::Logger::Get().Shutdown();
            break;
        default:
            break;
    }

    return TRUE;
}
