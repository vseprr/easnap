// Easnap - bounded audio packet queue.
//
// Time-budget-based bounded FIFO for audio packets. The capacity is derived
// from a latency budget (default 500 ms) and the WASAPI buffer period, so the
// queue size is always proportional to a fixed time window and never grows with
// recording duration.
//
// Overrun policy: when the queue is full, try_push returns Overrun. The caller
// must flag the next successfully queued packet with AudioPacketFlags::Discontinuity
// so that downstream consumers (future AAC encoder / muxer) can handle the gap
// explicitly. No audio data loss is silently hidden.
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "media/audio_format.h"

namespace easnap::media {

/// 500 ms default latency budget. At 10 ms WASAPI buffer periods this yields
/// ~50 packet slots. For 48 kHz / stereo / 32-bit float this bounds memory at
/// approximately 50 × 480 × 8 = 192,000 bytes per source — negligible compared
/// to the video frame queue.
inline constexpr uint32_t kDefaultAudioQueueBudgetMs = 500;

/// Minimum sensible queue capacity (covers brief scheduling jitter).
inline constexpr std::size_t kMinimumAudioQueueCapacity = 4;

enum class AudioQueuePushResult {
    Accepted,   ///< Packet was successfully enqueued.
    Overrun,    ///< Queue is full; packet was NOT enqueued.
    Closed,     ///< Queue has been closed; packet was NOT enqueued.
};

/// Fixed-capacity FIFO for audio packets with explicit overrun detection.
///
/// Thread-safe. The producer calls try_push; the consumer calls wait_pop.
/// Close/drain semantics match FrameQueue: close prevents further pushes but
/// accepted packets remain available until drained.
class AudioQueue final {
public:
    /// Constructs with an explicit packet-slot capacity.
    explicit AudioQueue(std::size_t capacity)
        : storage_(capacity) {
        if (capacity < kMinimumAudioQueueCapacity) {
            throw std::invalid_argument("AudioQueue capacity must be >= kMinimumAudioQueueCapacity");
        }
    }

    /// Constructs from a time budget and packet period. The capacity is
    /// ceil(budget_ms / period_ms), clamped to the minimum.
    AudioQueue(uint32_t budget_ms, uint32_t period_ms)
        : AudioQueue(CapacityFromBudget(budget_ms, period_ms)) {}

    AudioQueue(const AudioQueue&) = delete;
    AudioQueue& operator=(const AudioQueue&) = delete;
    AudioQueue(AudioQueue&&) = delete;
    AudioQueue& operator=(AudioQueue&&) = delete;

    /// Attempts to enqueue a packet. Returns Overrun when full (the packet is
    /// NOT moved from). Returns Closed after close() has been called.
    [[nodiscard]] AudioQueuePushResult try_push(AudioPacket&& packet) {
        {
            const std::scoped_lock lock(mutex_);
            if (closed_) {
                return AudioQueuePushResult::Closed;
            }
            if (size_ == storage_.size()) {
                return AudioQueuePushResult::Overrun;
            }
            storage_[tail_].emplace(std::move(packet));
            tail_ = (tail_ + 1) % storage_.size();
            ++size_;
            peak_size_ = (std::max)(peak_size_, size_);
        }
        not_empty_.notify_one();
        return AudioQueuePushResult::Accepted;
    }

    /// Waits for and removes the next packet. Returns nullopt only after the
    /// queue is both closed AND empty.
    [[nodiscard]] std::optional<AudioPacket> wait_pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return size_ != 0 || closed_; });
        if (size_ == 0) {
            return std::nullopt;
        }
        std::optional<AudioPacket> value(std::move(storage_[head_]));
        storage_[head_].reset();
        head_ = (head_ + 1) % storage_.size();
        --size_;
        return value;
    }

    /// Permanently prevents further pushes and wakes a waiting consumer.
    void close() noexcept {
        {
            const std::scoped_lock lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

    [[nodiscard]] std::size_t size() const {
        const std::scoped_lock lock(mutex_);
        return size_;
    }

    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] bool full() const { return size() == capacity(); }

    [[nodiscard]] bool closed() const {
        const std::scoped_lock lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t peak_size() const {
        const std::scoped_lock lock(mutex_);
        return peak_size_;
    }

    /// Derives packet-slot capacity from a latency budget and packet period.
    [[nodiscard]] static std::size_t CapacityFromBudget(uint32_t budget_ms,
                                                         uint32_t period_ms) noexcept {
        if (period_ms == 0) {
            return kMinimumAudioQueueCapacity;
        }
        const std::size_t raw = (static_cast<std::size_t>(budget_ms) + period_ms - 1) / period_ms;
        return (std::max)(raw, kMinimumAudioQueueCapacity);
    }

private:
    std::vector<std::optional<AudioPacket>> storage_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
    std::size_t peak_size_{0};
    bool closed_{false};
};

}  // namespace easnap::media
