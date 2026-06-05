#include "loader/attach_session.h"

#include <Windows.h>

#include <optional>
#include <sstream>
#include <vector>

#include "platform/win32/process_utils.h"

namespace chimera::loader {

namespace {

using chimera::common::ErrorCode;
using chimera::common::Result;
using chimera::common::Status;

[[nodiscard]] std::wstring QualityModeEnvironmentString(const chimera::common::QualityMode mode) {
    switch (mode) {
        case chimera::common::QualityMode::kUltraQuality:
            return L"UltraQuality";
        case chimera::common::QualityMode::kQuality:
            return L"Quality";
        case chimera::common::QualityMode::kBalanced:
            return L"Balanced";
        case chimera::common::QualityMode::kPerformance:
            return L"Performance";
    }

    return L"Quality";
}

[[nodiscard]] Result<std::optional<std::wstring>> GetEnvironmentVariableValue(const wchar_t* name) {
    DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0U);
    if (size == 0U) {
        if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::optional<std::wstring>{};
        }
        return Status::Error(ErrorCode::kIoError, "GetEnvironmentVariableW failed.");
    }

    std::vector<wchar_t> buffer(size);
    size = ::GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0U && ::GetLastError() != ERROR_SUCCESS) {
        return Status::Error(ErrorCode::kIoError, "GetEnvironmentVariableW failed.");
    }

    return std::optional<std::wstring>{std::wstring(buffer.data(), size)};
}

class ScopedEnvironmentVariableOverride final {
public:
    [[nodiscard]] static Result<ScopedEnvironmentVariableOverride> Create(const wchar_t* name, const std::wstring& value) {
        auto current_value = GetEnvironmentVariableValue(name);
        if (!current_value.ok()) {
            return current_value.status();
        }

        ScopedEnvironmentVariableOverride guard{};
        guard.name_ = name;
        guard.original_value_ = std::move(current_value.value());
        if (::SetEnvironmentVariableW(name, value.c_str()) == FALSE) {
            return Status::Error(ErrorCode::kIoError, "SetEnvironmentVariableW failed.");
        }

        guard.active_ = true;
        return guard;
    }

    ScopedEnvironmentVariableOverride() = default;
    ~ScopedEnvironmentVariableOverride() {
        Restore();
    }

    ScopedEnvironmentVariableOverride(const ScopedEnvironmentVariableOverride&) = delete;
    ScopedEnvironmentVariableOverride& operator=(const ScopedEnvironmentVariableOverride&) = delete;

    ScopedEnvironmentVariableOverride(ScopedEnvironmentVariableOverride&& other) noexcept
        : name_(other.name_),
          original_value_(std::move(other.original_value_)),
          active_(other.active_) {
        other.name_.clear();
        other.active_ = false;
    }

    ScopedEnvironmentVariableOverride& operator=(ScopedEnvironmentVariableOverride&& other) noexcept {
        if (this != &other) {
            Restore();
            name_ = other.name_;
            original_value_ = std::move(other.original_value_);
            active_ = other.active_;
            other.name_.clear();
            other.active_ = false;
        }
        return *this;
    }

private:
    void Restore() noexcept {
        if (!active_) {
            return;
        }

        const auto* name = name_.c_str();
        if (original_value_.has_value()) {
            (void)::SetEnvironmentVariableW(name, original_value_->c_str());
        } else {
            (void)::SetEnvironmentVariableW(name, nullptr);
        }
        active_ = false;
    }

    std::wstring name_{};
    std::optional<std::wstring> original_value_{};
    bool active_{false};
};

class ScopedPathPrepend final {
public:
    [[nodiscard]] static Result<ScopedPathPrepend> Create(const std::wstring& prefix) {
        auto current_path = GetEnvironmentVariableValue(L"PATH");
        if (!current_path.ok()) {
            return current_path.status();
        }

        std::wstring updated_path = prefix;
        if (current_path.value().has_value() && !current_path.value()->empty()) {
            updated_path.append(L";");
            updated_path.append(*current_path.value());
        }

        auto path_override = ScopedEnvironmentVariableOverride::Create(L"PATH", updated_path);
        if (!path_override.ok()) {
            return path_override.status();
        }

        ScopedPathPrepend guard{};
        guard.path_override_ = std::move(path_override.value());
        return guard;
    }

    ScopedPathPrepend() = default;
    ~ScopedPathPrepend() = default;

    ScopedPathPrepend(const ScopedPathPrepend&) = delete;
    ScopedPathPrepend& operator=(const ScopedPathPrepend&) = delete;
    ScopedPathPrepend(ScopedPathPrepend&&) noexcept = default;
    ScopedPathPrepend& operator=(ScopedPathPrepend&&) noexcept = default;

private:
    ScopedEnvironmentVariableOverride path_override_{};
};

}  // namespace

AttachSession::~AttachSession() = default;

chimera::common::Result<std::filesystem::path> AttachSession::ResolveRuntimeDllPath() const {
    auto executable_directory = platform::win32::GetExecutableDirectory();
    if (!executable_directory.ok()) {
        return executable_directory.status();
    }

    const auto runtime_dll_path = executable_directory.value() / "chimera_runtime.dll";
    if (!std::filesystem::exists(runtime_dll_path)) {
        return chimera::common::Status::Error(
            chimera::common::ErrorCode::kNotFound,
            "chimera_runtime.dll was not found next to the launcher executable.");
    }

    return runtime_dll_path;
}

chimera::common::Result<void> AttachSession::LaunchAndAttach(const LaunchRequest& request) {
    request_ = request;
    policy_decision_ = EvaluatePolicy(request);
    if (!policy_decision_.allowed) {
        std::ostringstream stream;
        stream << "PolicyGate refused target: ";
        for (std::size_t index = 0; index < policy_decision_.reasons.size(); ++index) {
            if (index > 0U) {
                stream << ' ';
            }
            stream << policy_decision_.reasons[index];
        }
        return chimera::common::Status::Error(chimera::common::ErrorCode::kPolicyRefused, stream.str());
    }

    auto runtime_dll_path = ResolveRuntimeDllPath();
    if (!runtime_dll_path.ok()) {
        return runtime_dll_path.status();
    }

    auto scoped_path = ScopedPathPrepend::Create(runtime_dll_path.value().parent_path().native());
    if (!scoped_path.ok()) {
        return scoped_path.status();
    }

    auto quality_override = ScopedEnvironmentVariableOverride::Create(
        L"CHIMERA_QUALITY_MODE",
        QualityModeEnvironmentString(request.requested_quality_mode));
    if (!quality_override.ok()) {
        return quality_override.status();
    }

    const auto absolute_executable_path = std::filesystem::absolute(request.executable_path);
    const auto working_directory = request.working_directory.empty() ? absolute_executable_path.parent_path() : request.working_directory;
    auto command_line = platform::win32::QuoteCommandLineArgument(absolute_executable_path.native());
    if (!request.target_arguments.empty()) {
        command_line.append(L" ");
        command_line.append(request.target_arguments);
    }

    auto launched_process = platform::win32::CreateSuspendedProcess(
        absolute_executable_path,
        std::move(command_line),
        working_directory);
    if (!launched_process.ok()) {
        return launched_process.status();
    }

    process_ = std::move(launched_process.value());

    auto protection_info = platform::win32::QueryProcessProtection(process_.process.get());
    if (!protection_info.ok()) {
        (void)Terminate(1U);
        return protection_info.status();
    }

    if (protection_info.value().query_supported && protection_info.value().is_protected) {
        (void)Terminate(1U);
        return chimera::common::Status::Error(
            chimera::common::ErrorCode::kPolicyRefused,
            "PolicyGate refused protected process target.");
    }

    auto injection_result = platform::win32::InjectDllWithLoadLibrary(process_.process.get(), runtime_dll_path.value());
    if (!injection_result.ok()) {
        (void)Terminate(1U);
        return injection_result.status();
    }

    auto resume_result = platform::win32::ResumeProcessMainThread(process_.thread.get());
    if (!resume_result.ok()) {
        (void)Terminate(1U);
        return resume_result.status();
    }

    return {};
}

chimera::common::Result<DWORD> AttachSession::WaitForExit(const DWORD timeout_ms) const {
    if (!process_.process.valid()) {
        return chimera::common::Status::Error(chimera::common::ErrorCode::kInvalidArgument, "No active attach session.");
    }

    const auto wait_result = ::WaitForSingleObject(process_.process.get(), timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        return chimera::common::Status::Error(chimera::common::ErrorCode::kTimeout, "Timed out waiting for target process exit.");
    }
    if (wait_result != WAIT_OBJECT_0) {
        return chimera::common::Status::Error(chimera::common::ErrorCode::kIoError, "WaitForSingleObject failed for target process.");
    }

    DWORD exit_code = 0U;
    if (::GetExitCodeProcess(process_.process.get(), &exit_code) == FALSE) {
        return chimera::common::Status::Error(chimera::common::ErrorCode::kIoError, "GetExitCodeProcess failed.");
    }

    return exit_code;
}

chimera::common::Result<void> AttachSession::Terminate(const UINT exit_code) {
    if (!process_.process.valid()) {
        return {};
    }

    if (::TerminateProcess(process_.process.get(), exit_code) == FALSE) {
        return chimera::common::Status::Error(chimera::common::ErrorCode::kIoError, "TerminateProcess failed.");
    }

    process_ = {};
    return {};
}

}  // namespace chimera::loader
