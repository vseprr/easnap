// Easnap - fixed-capacity, thread-safe frame transport.
#pragma once

#include <algorithm>
#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace easnap::media {

/// A fixed-capacity FIFO for ownership transfer between recording threads.
///
/// The queue owns every successfully pushed value until it is popped. Calls may
/// be made concurrently by multiple producers and consumers. Full and empty are
/// reported immediately by the try operations; the queue never grows or
/// allocates after construction. A rejected push does not move from the
/// supplied value. A consumer may wait without polling until a value is
/// available or the producer closes the queue.
template <typename T>
class FrameQueue final {
public:
    explicit FrameQueue(std::size_t capacity) : storage_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("FrameQueue capacity must be positive");
        }
    }

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;
    FrameQueue(FrameQueue&&) = delete;
    FrameQueue& operator=(FrameQueue&&) = delete;

    [[nodiscard]] bool try_push(T&& value) {
        {
            const std::scoped_lock lock(mutex_);
            if (closed_ || size_ == storage_.size()) {
                return false;
            }
            storage_[tail_].emplace(std::move(value));
            tail_ = (tail_ + 1) % storage_.size();
            ++size_;
            peak_size_ = (std::max)(peak_size_, size_);
        }
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        const std::scoped_lock lock(mutex_);
        if (size_ == 0) {
            return std::nullopt;
        }
        std::optional<T> value(std::move(storage_[head_]));
        storage_[head_].reset();
        head_ = (head_ + 1) % storage_.size();
        --size_;
        return value;
    }

    /// Waits for and removes the next value. A closed queue continues to drain
    /// accepted values and returns nullopt only after it becomes empty.
    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return size_ != 0 || closed_; });
        if (size_ == 0) {
            return std::nullopt;
        }
        std::optional<T> value(std::move(storage_[head_]));
        storage_[head_].reset();
        head_ = (head_ + 1) % storage_.size();
        --size_;
        return value;
    }

    /// Permanently prevents further pushes and wakes a waiting consumer.
    /// Already accepted values remain available in FIFO order.
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

private:
    std::vector<std::optional<T>> storage_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
    std::size_t peak_size_{0};
    bool closed_{false};
};

}  // namespace easnap::media
