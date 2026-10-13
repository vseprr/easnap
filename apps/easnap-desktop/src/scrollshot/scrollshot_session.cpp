// Easnap - scrolling-screenshot capture session implementation.
#include "scrollshot/scrollshot_session.h"

#include <algorithm>
#include <cstring>

namespace easnap::scrollshot {
namespace {

using Clock = std::chrono::steady_clock;

/// A region on one duplicated output waits this long for the desktop to
/// change, so an idle capture wakes ten times a second and reads no pixels.
constexpr uint32_t kChangeWaitMilliseconds = 100;
/// Shortest time between two readings while the content is changing.
constexpr auto kActivePeriod = std::chrono::milliseconds(30);
/// Reading period once several successive pictures were identical. It applies
/// to the paths that cannot wait for a change and must poll.
constexpr auto kIdlePeriod = std::chrono::milliseconds(200);
constexpr uint32_t kIdleAfterIdenticalFrames = 4;
constexpr auto kPublishPeriod = std::chrono::milliseconds(80);
constexpr auto kFailureRetry = std::chrono::milliseconds(100);
/// About two seconds without a readable picture is reported as a failure.
constexpr uint32_t kFailureLimit = 20;
/// A picture that could not be placed is reported as lost tracking only when
/// nothing was placed for this long: one doubtful frame in the middle of a
/// scroll is normal and corrects itself.
constexpr auto kLostAfter = std::chrono::milliseconds(400);

}  // namespace

ScrollshotSession::ScrollshotSession(std::unique_ptr<capture::IRegionCaptureProvider> provider, Options options)
    : provider_(std::move(provider)), options_(options) {
    progress_.generation = options_.generation;
}

ScrollshotSession::~ScrollshotSession() {
    {
        const std::scoped_lock lock(mutex_);
        stop_requested_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) { thread_.join(); }
}

bool ScrollshotSession::Start(std::chrono::milliseconds deadline) {
    {
        std::unique_lock lock(mutex_);
        if (provider_ == nullptr || thread_.joinable() || stop_requested_ ||
            platform::IsEmptyRect(options_.region)) {
            return false;
        }
        try {
            thread_ = std::thread(&ScrollshotSession::ThreadMain, this);
        } catch (...) {
            return false;
        }
        changed_.wait_for(lock, deadline, [this] { return first_frame_seen_ || worker_finished_; });
        if (first_frame_seen_ && !worker_finished_) { return true; }
        stop_requested_ = true;
    }
    changed_.notify_all();
    thread_.join();
    return false;
}

bool ScrollshotSession::Stop(std::chrono::milliseconds deadline) {
    std::unique_lock lock(mutex_);
    stop_requested_ = true;
    changed_.notify_all();
    if (!thread_.joinable()) { return true; }
    if (!changed_.wait_for(lock, deadline, [this] { return worker_finished_; })) { return false; }
    lock.unlock();
    thread_.join();  // Returns at once: setting the flag was the worker's last act.
    return true;
}

ScrollshotProgress ScrollshotSession::progress() const {
    notification_pending_.store(false, std::memory_order_release);
    const std::scoped_lock lock(mutex_);
    return progress_;
}

void ScrollshotSession::CopyPreview(int max_rows, ScrollshotPreview& out) const {
    const std::scoped_lock lock(mutex_);
    out.width = preview_width_;
    out.total_height = preview_rows_;
    out.height = std::clamp(max_rows, 0, preview_rows_);
    out.version = progress_.preview_version;
    const size_t count = static_cast<size_t>(out.width) * static_cast<size_t>(out.height);
    out.pixels.resize(count);
    if (count != 0) {
        std::memcpy(out.pixels.data(),
                    preview_.data() + static_cast<size_t>(preview_rows_ - out.height) * preview_width_,
                    count * sizeof(uint32_t));
    }
}

platform::BgraImage ScrollshotSession::TakeResult() {
    const std::scoped_lock lock(mutex_);
    return std::move(result_);
}

bool ScrollshotSession::WaitInterruptibly(std::chrono::milliseconds duration) {
    std::unique_lock lock(mutex_);
    return !changed_.wait_for(lock, duration, [this] { return stop_requested_; });
}

void ScrollshotSession::ThreadMain() noexcept {
    try {
        CaptureLoop();
    } catch (...) {
        capture_failed_ = true;  // Out of memory: keep what was stitched.
    }
    Finish();
}

void ScrollshotSession::CaptureLoop() {
    engine_ = std::make_unique<StitchEngine>(options_.limits);
    capture::RegionImageOptions capture_options;
    capture_options.include_cursor = false;  // The pointer is never part of a scrollshot.
    capture_options.wait_milliseconds = kChangeWaitMilliseconds;
    capture_options.require_every_output = true;
    // The desktop usually stands still when a capture starts.
    capture_options.await_first_composed_frame = true;

    platform::BgraImage frame;
    platform::BgraImage previous;
    uint32_t identical_run = 0;
    for (;;) {
        {
            const std::scoped_lock lock(mutex_);
            if (stop_requested_) { return; }
        }
        const auto started = Clock::now();
        const capture::RegionCaptureResult captured =
            provider_->CaptureRegionImage(options_.region, capture_options, frame);
        if (captured.status == capture::RegionCaptureStatus::Failed) {
            if (failure_run_ < kFailureLimit && ++failure_run_ == kFailureLimit) {
                capture_failed_ = true;
                progress_changed_ = true;
                Publish(true);
            }
            if (!WaitInterruptibly(kFailureRetry)) { return; }
            continue;
        }
        failure_run_ = 0;
        if (capture_failed_) {
            capture_failed_ = false;
            progress_changed_ = true;
        }
        if (captured.status == capture::RegionCaptureStatus::FrameAcquired && !frame.empty()) {
            if (captured.primary_failure.stage != capture::RegionCaptureFailureStage::None) {
                const std::scoped_lock lock(mutex_);
                ++progress_.fallback_frames;
            }
            const bool identical =
                frame.width == previous.width && frame.height == previous.height &&
                std::memcmp(frame.pixels.data(), previous.pixels.data(),
                            frame.pixels.size() * sizeof(uint32_t)) == 0;
            if (identical) {
                if (identical_run < kIdleAfterIdenticalFrames) { ++identical_run; }
            } else {
                identical_run = 0;
                Examine(frame);
                std::swap(frame, previous);
            }
        }
        Publish(false);

        const auto period = identical_run >= kIdleAfterIdenticalFrames ? kIdlePeriod : kActivePeriod;
        const auto elapsed = Clock::now() - started;
        if (elapsed < period &&
            !WaitInterruptibly(std::chrono::duration_cast<std::chrono::milliseconds>(period - elapsed))) {
            return;
        }
    }
}

void ScrollshotSession::Examine(const platform::BgraImage& frame) {
    const FrameResult result =
        engine_->Submit(FrameView{frame.pixels.data(), frame.width, frame.height, frame.width});
    if (result.outcome == FrameOutcome::Invalid) { return; }
    if (result.placed) {
        unplaced_ = false;
    } else if (!unplaced_) {
        unplaced_ = true;
        unplaced_since_ = Clock::now();
    }
    const bool wrote = result.placed && result.position >= 0 && result.outcome != FrameOutcome::Unchanged;
    if (wrote) {
        const int begin = result.position;
        const int end = std::min(result.position + frame.height, engine_->height());
        if (dirty_end_ <= dirty_begin_) {
            dirty_begin_ = begin;
            dirty_end_ = end;
        } else {
            dirty_begin_ = std::min(dirty_begin_, begin);
            dirty_end_ = std::max(dirty_end_, end);
        }
    }
    bool first = false;
    {
        const std::scoped_lock lock(mutex_);
        progress_changed_ = progress_changed_ || progress_.height != engine_->height() ||
                            progress_.last_outcome != result.outcome || wrote;
        progress_.width = engine_->width();
        progress_.height = engine_->height();
        progress_.last_outcome = result.outcome;
        if (wrote) { ++progress_.placed_frames; }
        if (result.outcome == FrameOutcome::First) {
            first_frame_seen_ = true;
            first = true;
        }
    }
    if (first) {
        changed_.notify_all();
        Publish(true);
    }
}

void ScrollshotSession::Publish(bool force) {
    ScrollshotCondition condition = ScrollshotCondition::Following;
    if (capture_failed_) { condition = ScrollshotCondition::CaptureFailed; }
    else if (engine_ != nullptr && engine_->at_limit()) { condition = ScrollshotCondition::AtLimit; }
    const auto now = Clock::now();
    if (condition == ScrollshotCondition::Following && unplaced_ && now - unplaced_since_ >= kLostAfter) {
        condition = ScrollshotCondition::Lost;
    }
    {
        const std::scoped_lock lock(mutex_);
        if (progress_.condition != condition) { progress_changed_ = true; }
        if (!progress_changed_ || (!force && now - last_publish_ < kPublishPeriod)) { return; }
        progress_.condition = condition;

        // Bring the reduced image up to date for the rows that changed.
        const int width = engine_ != nullptr ? engine_->width() : 0;
        const int height = engine_ != nullptr ? engine_->height() : 0;
        if (width > 0 && height > 0 && dirty_end_ > dirty_begin_) {
            const int reduced_width = std::clamp(options_.preview_width, 1, width);
            const int reduced_rows = static_cast<int>((static_cast<int64_t>(height) * reduced_width + width - 1) / width);
            preview_width_ = reduced_width;
            preview_.resize(static_cast<size_t>(reduced_width) * reduced_rows);
            preview_rows_ = reduced_rows;
            const int first_row = static_cast<int>(static_cast<int64_t>(dirty_begin_) * reduced_width / width);
            const int end_row = std::min(reduced_rows, static_cast<int>(
                (static_cast<int64_t>(dirty_end_) * reduced_width + width - 1) / width));
            for (int row = first_row; row < end_row; ++row) {
                const int top = static_cast<int>(static_cast<int64_t>(row) * width / reduced_width);
                const int bottom = std::min(height, std::max(top + 1, static_cast<int>(
                    static_cast<int64_t>(row + 1) * width / reduced_width)));
                uint32_t* target = preview_.data() + static_cast<size_t>(row) * reduced_width;
                for (int column = 0; column < reduced_width; ++column) {
                    const int left = static_cast<int>(static_cast<int64_t>(column) * width / reduced_width);
                    const int right = std::max(left + 1, static_cast<int>(
                        static_cast<int64_t>(column + 1) * width / reduced_width));
                    // Nine samples per box are enough for a thumbnail.
                    uint32_t blue = 0, green = 0, red = 0, samples = 0;
                    for (int step_y = 0; step_y < 3; ++step_y) {
                        const uint32_t* source = engine_->Row(top + (bottom - top) * step_y / 3);
                        if (source == nullptr) { continue; }
                        for (int step_x = 0; step_x < 3; ++step_x) {
                            const uint32_t pixel = source[left + (right - left) * step_x / 3];
                            blue += pixel & 0xFFu;
                            green += (pixel >> 8) & 0xFFu;
                            red += (pixel >> 16) & 0xFFu;
                            ++samples;
                        }
                    }
                    target[column] = samples == 0 ? 0xFF000000u
                        : 0xFF000000u | ((red / samples) << 16) | ((green / samples) << 8) | (blue / samples);
                }
            }
            ++progress_.preview_version;
        }
        dirty_begin_ = dirty_end_ = 0;
        progress_changed_ = false;
        last_publish_ = now;
    }
    if (options_.notify_window != nullptr &&
        !notification_pending_.exchange(true, std::memory_order_acq_rel) &&
        ::PostMessageW(options_.notify_window, options_.notify_message,
                       static_cast<WPARAM>(options_.generation), 0) == FALSE) {
        notification_pending_.store(false, std::memory_order_release);
    }
}

void ScrollshotSession::Finish() noexcept {
    platform::BgraImage result;
    try {
        if (engine_ != nullptr && engine_->height() > 0) {
            progress_changed_ = true;
            Publish(true);
            result.Reset(engine_->width(), engine_->height());
            if (!engine_->CopyRows(0, engine_->height(), result.pixels.data(), result.width)) { result = {}; }
            result.MakeOpaque();
        }
    } catch (...) {
        result = {};
    }
    engine_.reset();
    if (provider_ != nullptr) { provider_->Release(); }
    {
        const std::scoped_lock lock(mutex_);
        result_ = std::move(result);
        progress_.finished = true;
        worker_finished_ = true;
    }
    changed_.notify_all();
    if (options_.notify_window != nullptr) {
        notification_pending_.store(true, std::memory_order_release);
        if (::PostMessageW(options_.notify_window, options_.notify_message,
                           static_cast<WPARAM>(options_.generation), 0) == FALSE) {
            notification_pending_.store(false, std::memory_order_release);
        }
    }
}

}  // namespace easnap::scrollshot
