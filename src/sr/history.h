#pragma once

#include <array>
#include <cstdint>

namespace chimera::sr {

struct HistoryState final {
    bool valid{false};
    bool reset_requested{false};
    std::uint64_t frame_index{0U};
    std::array<float, 2> previous_jitter{0.0F, 0.0F};

    void Reset() noexcept {
        valid = false;
        reset_requested = true;
    }

    void Advance(const std::uint64_t new_frame_index, const std::array<float, 2>& jitter) noexcept {
        frame_index = new_frame_index;
        previous_jitter = jitter;
        valid = true;
        reset_requested = false;
    }
};

}  // namespace chimera::sr
