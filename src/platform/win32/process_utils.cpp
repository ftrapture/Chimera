#include "platform/win32/process_utils.h"

#include <TlHelp32.h>

#include <array>
#include <cwctype>
#include <sstream>
#include <vector>

namespace chimera::platform::win32 {

namespace {

using chimera::common::ErrorCode;
using chimera::common::Result;
using chimera::common::Status;

[[nodiscard]] Status LastErrorStatus(const ErrorCode code, const char* context) {
    std::ostringstream stream;
    stream << context << " failed with Win32 error " << ::GetLastError() << '.';
    return Status::Error(code, stream.str());
}

}  // namespace

UniqueHandle::~UniqueHandle() {
    reset();
}

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

HANDLE UniqueHandle::release() noexcept {
    const auto handle = handle_;
    handle_ = nullptr;
    return handle;
}

void UniqueHandle::reset(const HANDLE handle) noexcept {
    if (valid()) {
        ::CloseHandle(handle_);
    }
    handle_ = handle;
}

Result<std::filesystem::path> GetExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    auto copied = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

    while (copied == buffer.size()) {
        buffer.resize(buffer.size() * 2U);
        copied = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }

    if (copied == 0U) {
        return Status::Error(ErrorCode::kIoError, "GetModuleFileNameW failed.");
    }

    return std::filesystem::path(buffer.data());
}

Result<std::filesystem::path> GetExecutableDirectory() {
    auto path = GetExecutablePath();
    if (!path.ok()) {
        return path.status();
    }

    return path.value().parent_path();
}

std::wstring GetCommandLineString() {
    return ::GetCommandLineW();
}

bool IsDebuggerAttached() noexcept {
    return ::IsDebuggerPresent() != 0;
}

std::wstring QuoteCommandLineArgument(const std::wstring_view value) {
    if (value.empty()) {
        return L"\"\"";
    }

    bool requires_quotes = false;
    for (const auto ch : value) {
        if (std::iswspace(ch) != 0 || ch == L'"') {
            requires_quotes = true;
            break;
        }
    }

    if (!requires_quotes) {
        return std::wstring(value);
    }

    std::wstring result;
    result.push_back(L'"');
    for (const auto ch : value) {
        if (ch == L'"') {
            result.append(L"\\\"");
        } else {
            result.push_back(ch);
        }
    }
    result.push_back(L'"');
    return result;
}

Result<OwnedProcess> CreateSuspendedProcess(
    const std::filesystem::path& executable_path,
    std::wstring command_line,
    const std::filesystem::path& working_directory) {
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);

    PROCESS_INFORMATION process_info{};
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    const auto created = ::CreateProcessW(
        executable_path.c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup_info,
        &process_info);
    if (created == FALSE) {
        return LastErrorStatus(ErrorCode::kIoError, "CreateProcessW");
    }

    OwnedProcess owned_process{};
    owned_process.process = UniqueHandle(process_info.hProcess);
    owned_process.thread = UniqueHandle(process_info.hThread);
    owned_process.process_id = process_info.dwProcessId;
    owned_process.thread_id = process_info.dwThreadId;
    return owned_process;
}

Result<void> ResumeProcessMainThread(const HANDLE thread_handle) {
    const auto resume_result = ::ResumeThread(thread_handle);
    if (resume_result == static_cast<DWORD>(-1)) {
        return LastErrorStatus(ErrorCode::kIoError, "ResumeThread");
    }
    return {};
}

Result<void> InjectDllWithLoadLibrary(
    const HANDLE process_handle,
    const std::filesystem::path& dll_path,
    const DWORD timeout_ms) {
    const auto absolute_dll_path = std::filesystem::absolute(dll_path);
    if (!std::filesystem::exists(absolute_dll_path)) {
        return Status::Error(ErrorCode::kNotFound, "Runtime DLL path does not exist.");
    }

    const auto dll_wstring = absolute_dll_path.native();
    const auto buffer_size = (dll_wstring.size() + 1U) * sizeof(wchar_t);

    auto* remote_buffer = ::VirtualAllocEx(process_handle, nullptr, buffer_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (remote_buffer == nullptr) {
        return LastErrorStatus(ErrorCode::kIoError, "VirtualAllocEx");
    }

    const auto write_ok = ::WriteProcessMemory(process_handle, remote_buffer, dll_wstring.c_str(), buffer_size, nullptr);
    if (write_ok == FALSE) {
        ::VirtualFreeEx(process_handle, remote_buffer, 0U, MEM_RELEASE);
        return LastErrorStatus(ErrorCode::kIoError, "WriteProcessMemory");
    }

    const auto kernel32_module = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel32_module == nullptr) {
        ::VirtualFreeEx(process_handle, remote_buffer, 0U, MEM_RELEASE);
        return LastErrorStatus(ErrorCode::kIoError, "GetModuleHandleW(kernel32)");
    }

    const auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(kernel32_module, "LoadLibraryW"));
    if (load_library == nullptr) {
        ::VirtualFreeEx(process_handle, remote_buffer, 0U, MEM_RELEASE);
        return Status::Error(ErrorCode::kIoError, "GetProcAddress(LoadLibraryW) failed.");
    }

    UniqueHandle remote_thread(::CreateRemoteThread(process_handle, nullptr, 0U, load_library, remote_buffer, 0U, nullptr));
    if (!remote_thread.valid()) {
        ::VirtualFreeEx(process_handle, remote_buffer, 0U, MEM_RELEASE);
        return LastErrorStatus(ErrorCode::kIoError, "CreateRemoteThread");
    }

    const auto wait_result = ::WaitForSingleObject(remote_thread.get(), timeout_ms);
    ::VirtualFreeEx(process_handle, remote_buffer, 0U, MEM_RELEASE);
    if (wait_result != WAIT_OBJECT_0) {
        return Status::Error(ErrorCode::kTimeout, "Timed out waiting for remote LoadLibraryW.");
    }

    DWORD remote_exit_code = 0U;
    if (::GetExitCodeThread(remote_thread.get(), &remote_exit_code) == FALSE) {
        return LastErrorStatus(ErrorCode::kIoError, "GetExitCodeThread");
    }

    if (remote_exit_code == 0U) {
        return Status::Error(ErrorCode::kIoError, "Remote LoadLibraryW failed to load Chimera runtime.");
    }

    return {};
}

Result<ProcessProtectionInfo> QueryProcessProtection(const HANDLE process_handle) {
    ProcessProtectionInfo info{};
    PROCESS_PROTECTION_LEVEL_INFORMATION protection_info{};
    const auto query_ok = ::GetProcessInformation(
        process_handle,
        ProcessProtectionLevelInfo,
        &protection_info,
        sizeof(protection_info));
    if (query_ok == FALSE) {
        return info;
    }

    info.query_supported = true;
    info.protection_level = protection_info.ProtectionLevel;
    info.is_protected = protection_info.ProtectionLevel != PROTECTION_LEVEL_NONE;

    return info;
}

std::vector<std::wstring> EnumerateProcessImageNames() {
    std::vector<std::wstring> result;

    UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U));
    if (!snapshot.valid()) {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot.get(), &entry) == FALSE) {
        return result;
    }

    do {
        result.emplace_back(entry.szExeFile);
    } while (::Process32NextW(snapshot.get(), &entry) != FALSE);

    return result;
}

}  // namespace chimera::platform::win32
