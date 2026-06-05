#pragma once

#include <Windows.h>

namespace chimera::platform::win32 {

class QpcTimer final {
public:
    QpcTimer() {
        ::QueryPerformanceFrequency(&frequency_);
        Reset();
    }

    void Reset() { ::QueryPerformanceCounter(&start_); }

    [[nodiscard]] double ElapsedMilliseconds() const {
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        const auto ticks = static_cast<double>(now.QuadPart - start_.QuadPart);
        return (ticks * 1000.0) / static_cast<double>(frequency_.QuadPart);
    }

private:
    LARGE_INTEGER frequency_{};
    LARGE_INTEGER start_{};
};

}  // namespace chimera::platform::win32
