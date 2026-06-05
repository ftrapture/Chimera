#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace chimera::framegraph {

enum class ResourceLifetimeTag : std::uint8_t {
    kTransient = 0,
    kPersistent,
    kHistory,
    kPresentBound,
};

struct ResourceRecord final {
    std::string name{};
    ResourceLifetimeTag lifetime{ResourceLifetimeTag::kTransient};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
};

class ResourcePool final {
public:
    void RegisterPersistent(ResourceRecord record) { records_.push_back(std::move(record)); }
    [[nodiscard]] const std::vector<ResourceRecord>& records() const noexcept { return records_; }

private:
    std::vector<ResourceRecord> records_{};
};

}  // namespace chimera::framegraph
