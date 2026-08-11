#pragma once
#include <array>
#include <atomic>
#include <cstddef>

template <typename T, std::size_t Capacity>
class SpscQueue final {
    static_assert(Capacity >= 2);
public:
    bool push(const T& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1U) % Capacity;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        data_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = data_[tail];
        tail_.store((tail + 1U) % Capacity, std::memory_order_release);
        return true;
    }
    bool empty() const noexcept { return head_.load() == tail_.load(); }
private:
    std::array<T, Capacity> data_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};
