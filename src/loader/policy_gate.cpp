#include "loader/policy_gate.h"

#include <algorithm>
#include <array>
#include <cwctype>

namespace chimera::loader {

namespace {

[[nodiscard]] std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

[[nodiscard]] bool ContainsBlockedToken(const std::wstring& text) {
    static constexpr std::array<std::wstring_view, 12> kBlockedTokens = {
        L"easyanticheat",
        L"battleye",
        L"vgk",
        L"vanguard",
        L"xigncode",
        L"equ8",
        L"faceit",
        L"ricochet",
        L"hyperion",
        L"nprotect",
        L"mhyprot",
        L"denuvo",
    };

    for (const auto token : kBlockedTokens) {
        if (text.find(token) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool LooksLikeStoreLauncher(const std::wstring& file_name) {
    static constexpr std::array<std::wstring_view, 6> kLaunchers = {
        L"steam.exe",
        L"epicgameslauncher.exe",
        L"ubisoftconnect.exe",
        L"ea app.exe",
        L"eadesktop.exe",
        L"gog galaxy.exe",
    };

    for (const auto launcher_name : kLaunchers) {
        if (file_name == launcher_name) {
            return true;
        }
    }
    return false;
}

}  // namespace

chimera::common::PolicyDecision EvaluatePolicy(const LaunchRequest& request) {
    chimera::common::PolicyDecision decision{};

    if (request.executable_path.empty()) {
        decision.allowed = false;
        decision.reasons.emplace_back("No target executable was provided.");
        return decision;
    }

    const auto absolute_path = std::filesystem::absolute(request.executable_path);
    if (!std::filesystem::exists(absolute_path)) {
        decision.allowed = false;
        decision.reasons.emplace_back("Target executable does not exist.");
        return decision;
    }

    if (absolute_path.extension() != ".exe" && absolute_path.extension() != ".EXE") {
        decision.allowed = false;
        decision.reasons.emplace_back("Target must be a Windows executable.");
        return decision;
    }

    if (!request.declared_offline) {
        decision.allowed = false;
        decision.reasons.emplace_back("Launcher requires explicit --offline consent for user-mode attachment.");
        return decision;
    }

    const auto lowered_file_name = Lowercase(absolute_path.filename().native());
    const auto lowered_full_path = Lowercase(absolute_path.native());

    if (LooksLikeStoreLauncher(lowered_file_name)) {
        decision.allowed = false;
        decision.reasons.emplace_back("Refusing to attach to a store/launcher executable; point Chimera at the actual game .exe.");
        return decision;
    }

    if (ContainsBlockedToken(lowered_file_name) || ContainsBlockedToken(lowered_full_path)) {
        decision.allowed = false;
        decision.reasons.emplace_back("PolicyGate detected anti-cheat, anti-tamper, or legally uncertain markers in the target path.");
        return decision;
    }

    decision.allowed = true;
    return decision;
}

}  // namespace chimera::loader
