// Easnap - bounded audio callback delivery synchronization.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace easnap::media::detail {

enum class AudioDeliveryEnqueueResult : uint8_t {
    Accepted,
    Rejected,
    DeliveryPaused,
};

struct AudioDeliverySnapshot {
    uint64_t accepted_sequence{0};
    uint64_t completed_sequence{0};
    uint64_t paused_discards{0};
    bool enabled{false};
};

/// Coordinates a single producer and single callback consumer without adding
/// storage. The producer's bounded-queue decision and accepted sequence share
/// one mutex with the pause boundary; a pause therefore waits for exactly the
/// packets that were accepted before delivery was disabled.
class AudioDeliveryBarrier final {
public:
    AudioDeliveryBarrier() = default;

    AudioDeliveryBarrier(const AudioDeliveryBarrier&) = delete;
    AudioDeliveryBarrier& operator=(const AudioDeliveryBarrier&) = delete;

    void Reset() noexcept {
        const std::scoped_lock lock(mutex_);
        enabled_ = true;
        discontinuity_pending_ = false;
        accepted_sequence_ = 0;
        completed_sequence_ = 0;
        paused_discards_ = 0;
    }

    template <typename Enqueue>
    AudioDeliveryEnqueueResult TryEnqueue(Enqueue&& enqueue) {
        const std::scoped_lock lock(mutex_);
        if (!enabled_) {
            ++paused_discards_;
            discontinuity_pending_ = true;
            return AudioDeliveryEnqueueResult::DeliveryPaused;
        }

        if (!std::forward<Enqueue>(enqueue)(discontinuity_pending_)) {
            return AudioDeliveryEnqueueResult::Rejected;
        }

        discontinuity_pending_ = false;
        ++accepted_sequence_;
        return AudioDeliveryEnqueueResult::Accepted;
    }

    void CompleteOne() noexcept {
        {
            const std::scoped_lock lock(mutex_);
            if (completed_sequence_ < accepted_sequence_) {
                ++completed_sequence_;
            }
        }
        drained_.notify_all();
    }

    void PauseAndDrain() noexcept {
        std::unique_lock lock(mutex_);
        enabled_ = false;
        const uint64_t drain_target = accepted_sequence_;
        drained_.wait(lock, [this, drain_target] {
            return completed_sequence_ >= drain_target;
        });
    }

    void Resume() noexcept {
        const std::scoped_lock lock(mutex_);
        enabled_ = true;
    }

    [[nodiscard]] AudioDeliverySnapshot snapshot() const noexcept {
        const std::scoped_lock lock(mutex_);
        return AudioDeliverySnapshot{
            accepted_sequence_, completed_sequence_, paused_discards_, enabled_};
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable drained_;
    bool enabled_{false};
    bool discontinuity_pending_{false};
    uint64_t accepted_sequence_{0};
    uint64_t completed_sequence_{0};
    uint64_t paused_discards_{0};
};

}  // namespace easnap::media::detail
