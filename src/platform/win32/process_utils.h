#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

#include "common/result.h"

namespace chimera::platform::win32 {

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle();

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept;
    UniqueHandle& operator=(UniqueHandle&& other) noexcept;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE release() noexcept;
    void reset(HANDLE handle = nullptr) noexcept;

private:
    HANDLE handle_{nullptr};
};

struct OwnedProcess final {
    UniqueHandle process{};
    UniqueHandle thread{};
    DWORD process_id{0U};
    DWORD thread_id{0U};
};

struct ProcessProtectionInfo final {
    bool query_supported{false};
    bool is_protected{false};
    std::uint32_t protection_level{0U};
};

[[nodiscard]] chimera::common::Result<std::filesystem::path> GetExecutablePath();
[[nodiscard]] chimera::common::Result<std::filesystem::path> GetExecutableDirectory();
[[nodiscard]] std::wstring GetCommandLineString();
[[nodiscard]] bool IsDebuggerAttached() noexcept;
[[nodiscard]] std::wstring QuoteCommandLineArgument(std::wstring_view value);
[[nodiscard]] chimera::common::Result<OwnedProcess> CreateSuspendedProcess(
    const std::filesystem::path& executable_path,
    std::wstring command_line,
    const std::filesystem::path& working_directory);
[[nodiscard]] chimera::common::Result<void> ResumeProcessMainThread(HANDLE thread_handle);
[[nodiscard]] chimera::common::Result<void> InjectDllWithLoadLibrary(
    HANDLE process_handle,
    const std::filesystem::path& dll_path,
    DWORD timeout_ms = 5000U);
[[nodiscard]] chimera::common::Result<ProcessProtectionInfo> QueryProcessProtection(HANDLE process_handle);
[[nodiscard]] std::vector<std::wstring> EnumerateProcessImageNames();

}  // namespace chimera::platform::win32
