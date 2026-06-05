#pragma once

#include <array>
#include <cstddef>

namespace chimera::common {

template <typename T, std::size_t Capacity>
class RingBuffer final {
public:
    void push_back(const T& value) {
        data_[head_] = value;
        head_ = (head_ + 1U) % Capacity;
        if (size_ < Capacity) {
            ++size_;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return Capacity; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] bool full() const noexcept { return size_ == Capacity; }

    [[nodiscard]] const T& operator[](std::size_t index) const {
        const auto start = (head_ + Capacity - size_) % Capacity;
        return data_[(start + index) % Capacity];
    }

    [[nodiscard]] const T& back() const {
        return data_[(head_ + Capacity - 1U) % Capacity];
    }

private:
    std::array<T, Capacity> data_{};
    std::size_t head_{0U};
    std::size_t size_{0U};
};

}  // namespace chimera::common
