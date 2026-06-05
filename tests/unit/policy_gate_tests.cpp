#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "loader/policy_gate.h"

namespace {

std::filesystem::path WriteFakeExe(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << "MZ";
    return path;
}

}  // namespace

TEST_CASE("PolicyGate requires offline consent", "[loader][policy]") {
    chimera::loader::LaunchRequest request{};
    request.executable_path = WriteFakeExe(std::filesystem::temp_directory_path() / "chimera_policy_tests" / "Game.exe");
    request.declared_offline = false;

    const auto decision = chimera::loader::EvaluatePolicy(request);
    CHECK_FALSE(decision.allowed);
    REQUIRE_FALSE(decision.reasons.empty());
}

TEST_CASE("PolicyGate rejects obvious anti-cheat markers", "[loader][policy]") {
    chimera::loader::LaunchRequest request{};
    request.executable_path = WriteFakeExe(std::filesystem::temp_directory_path() / "chimera_policy_tests" / "EasyAntiCheat_launcher.exe");
    request.declared_offline = true;

    const auto decision = chimera::loader::EvaluatePolicy(request);
    CHECK_FALSE(decision.allowed);
}

TEST_CASE("PolicyGate rejects store launchers", "[loader][policy]") {
    chimera::loader::LaunchRequest request{};
    request.executable_path = WriteFakeExe(std::filesystem::temp_directory_path() / "chimera_policy_tests" / "steam.exe");
    request.declared_offline = true;

    const auto decision = chimera::loader::EvaluatePolicy(request);
    CHECK_FALSE(decision.allowed);
}
