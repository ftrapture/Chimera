#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace chimera::framegraph {

enum class QueueClass : std::uint8_t {
    kGraphics = 0,
    kAsyncCompute,
};

struct PassRecord final {
    std::string name{};
    QueueClass queue{QueueClass::kGraphics};
};

class FrameGraph final {
public:
    void BeginFrame(std::uint64_t frame_index) {
        frame_index_ = frame_index;
        passes_.clear();
    }

    void AddPass(std::string pass_name, QueueClass queue = QueueClass::kGraphics) {
        passes_.push_back(PassRecord{std::move(pass_name), queue});
    }

    [[nodiscard]] const std::vector<PassRecord>& passes() const noexcept { return passes_; }
    [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }

private:
    std::uint64_t frame_index_{0U};
    std::vector<PassRecord> passes_{};
};

}  // namespace chimera::framegraph
