#pragma once

#include <chrono>

namespace chimera::common {

class Stopwatch final {
public:
    using Clock = std::chrono::steady_clock;

    void Reset() noexcept { start_ = Clock::now(); }

    [[nodiscard]] double ElapsedMilliseconds() const noexcept {
        const auto delta = Clock::now() - start_;
        return std::chrono::duration<double, std::milli>(delta).count();
    }

private:
    Clock::time_point start_{Clock::now()};
};

}  // namespace chimera::common
