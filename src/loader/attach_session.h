#pragma once

#include <filesystem>

#include "common/result.h"
#include "loader/policy_gate.h"
#include "platform/win32/process_utils.h"

namespace chimera::loader {

class AttachSession final {
public:
    AttachSession() = default;
    ~AttachSession();

    AttachSession(const AttachSession&) = delete;
    AttachSession& operator=(const AttachSession&) = delete;

    [[nodiscard]] chimera::common::Result<void> LaunchAndAttach(const LaunchRequest& request);
    [[nodiscard]] chimera::common::Result<DWORD> WaitForExit(DWORD timeout_ms = INFINITE) const;
    [[nodiscard]] chimera::common::Result<void> Terminate(UINT exit_code = 1U);

    [[nodiscard]] DWORD process_id() const noexcept { return process_.process_id; }
    [[nodiscard]] bool active() const noexcept { return process_.process.valid(); }

private:
    [[nodiscard]] chimera::common::Result<std::filesystem::path> ResolveRuntimeDllPath() const;

    LaunchRequest request_{};
    chimera::common::PolicyDecision policy_decision_{};
    chimera::platform::win32::OwnedProcess process_{};
};

}  // namespace chimera::loader
