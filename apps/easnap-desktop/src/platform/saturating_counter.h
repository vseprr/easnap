// Easnap - diagnostic counter shared by capture, audio and recording.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

namespace easnap::platform {

/// Atomic diagnostic counter that saturates instead of wrapping.
class SaturatingCounter final {
public:
    explicit SaturatingCounter(uint64_t limit = (std::numeric_limits<uint64_t>::max)()) noexcept
        : limit_(limit) {}

    void Increment() noexcept { Add(1); }
    void Add(uint64_t amount) noexcept;
    void Reset() noexcept { value_.store(0, std::memory_order_relaxed); }
    [[nodiscard]] uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> value_{0};
    uint64_t limit_;
};

inline void SaturatingCounter::Add(uint64_t amount) noexcept {
    uint64_t current = value_.load(std::memory_order_relaxed);
    while (current < limit_) {
        const uint64_t available = limit_ - current;
        const uint64_t increment = (std::min)(amount, available);
        if (value_.compare_exchange_weak(current, current + increment,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            return;
        }
    }
}

}  // namespace easnap::platform
