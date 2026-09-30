#include "app/recording_finalizer.h"

#include <limits>
#include <utility>

namespace easnap::app {

RecordingFinalizer::~RecordingFinalizer() { Join(); }

bool RecordingFinalizer::Start(HWND owner, UINT message, std::function<bool()> stop) {
    if (worker_.joinable() || !stop ||
        generation_ == (std::numeric_limits<Generation>::max)()) {
        return false;
    }
    const Generation generation = ++generation_;
    completed_.store(0, std::memory_order_relaxed);
    result_ = {};
    try {
        worker_ = std::thread([this, owner, message, generation, stop = std::move(stop)] {
            try {
                result_.finalized = stop();
            } catch (...) {
                result_.worker_exception = true;
            }
            // Release publishes the completed work. TryTake joins before reading
            // any result, including the notification error written after this store.
            completed_.store(generation, std::memory_order_release);
            if (::PostMessageW(owner, message, static_cast<WPARAM>(generation), 0) == FALSE) {
                result_.notification_error = ::GetLastError();
            }
        });
    } catch (...) {
        return false;
    }
    return true;
}

std::optional<RecordingFinalizationResult> RecordingFinalizer::TryTake(Generation generation) {
    if (generation == 0 || generation != generation_ ||
        completed_.load(std::memory_order_acquire) != generation || !worker_.joinable()) {
        return std::nullopt;
    }
    worker_.join();
    completed_.store(0, std::memory_order_relaxed);
    return result_;
}

void RecordingFinalizer::Join() {
    if (worker_.joinable()) {
        worker_.join();
    }
    completed_.store(0, std::memory_order_relaxed);
}

}  // namespace easnap::app
