#include "capture/recording_capture_worker.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace easnap::capture {
namespace {

uint64_t ElapsedMicroseconds(std::chrono::steady_clock::time_point start) noexcept {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto value = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    return value > 0 ? static_cast<uint64_t>(value) : 0;
}

}  // namespace

bool RecordingWorkerLifecycle::Start() noexcept {
    if (state_ != RecordingWorkerState::Stopped) {
        return false;
    }
    state_ = RecordingWorkerState::Running;
    return true;
}

bool RecordingWorkerLifecycle::Pause() noexcept {
    if (state_ != RecordingWorkerState::Running) {
        return false;
    }
    state_ = RecordingWorkerState::Paused;
    return true;
}

bool RecordingWorkerLifecycle::Resume() noexcept {
    if (state_ != RecordingWorkerState::Paused) {
        return false;
    }
    state_ = RecordingWorkerState::Running;
    return true;
}

bool RecordingWorkerLifecycle::RequestStop() noexcept {
    if (state_ != RecordingWorkerState::Running && state_ != RecordingWorkerState::Paused) {
        return false;
    }
    state_ = RecordingWorkerState::Stopping;
    return true;
}

bool RecordingWorkerLifecycle::CompleteStop() noexcept {
    if (state_ != RecordingWorkerState::Stopping) {
        return false;
    }
    state_ = RecordingWorkerState::Stopped;
    return true;
}

void SaturatingCounter::Add(uint64_t amount) noexcept {
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

void TimingAccumulator::Add(uint64_t microseconds) noexcept {
    count_.Increment();
    total_microseconds_.Add(microseconds);

    uint64_t minimum = minimum_microseconds_.load(std::memory_order_relaxed);
    while (microseconds < minimum &&
           !minimum_microseconds_.compare_exchange_weak(
               minimum, microseconds, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    uint64_t maximum = maximum_microseconds_.load(std::memory_order_relaxed);
    while (microseconds > maximum &&
           !maximum_microseconds_.compare_exchange_weak(
               maximum, microseconds, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void TimingAccumulator::Reset() noexcept {
    count_.Reset();
    total_microseconds_.Reset();
    minimum_microseconds_.store((std::numeric_limits<uint64_t>::max)(),
                                std::memory_order_relaxed);
    maximum_microseconds_.store(0, std::memory_order_relaxed);
}

AggregateTiming TimingAccumulator::snapshot() const noexcept {
    AggregateTiming result;
    result.count = count_.value();
    result.total_microseconds = total_microseconds_.value();
    result.minimum_microseconds =
        result.count == 0 ? 0 : minimum_microseconds_.load(std::memory_order_relaxed);
    result.maximum_microseconds = maximum_microseconds_.load(std::memory_order_relaxed);
    return result;
}

media::MediaTime MediaTimestampForFrame(uint64_t frame_index, uint32_t frame_rate) noexcept {
    if (frame_rate == 0) {
        return media::MediaTime::zero();
    }

    constexpr uint64_t kTicksPerSecond = 10'000'000;
    constexpr uint64_t kMaximumTicks = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    const uint64_t whole_seconds = frame_index / frame_rate;
    if (whole_seconds > kMaximumTicks / kTicksPerSecond) {
        return media::MediaTime::max();
    }

    const uint64_t remainder = frame_index % frame_rate;
    const uint64_t ticks = whole_seconds * kTicksPerSecond +
                           remainder * kTicksPerSecond / frame_rate;
    if (ticks > kMaximumTicks) {
        return media::MediaTime::max();
    }
    return media::MediaTime{static_cast<int64_t>(ticks)};
}

bool TrySubmitVideoFrameOrDropNewest(media::FrameQueue<media::VideoFrame>& queue,
                                     media::VideoFrame&& frame,
                                     SaturatingCounter& rejected_frames) {
    if (queue.try_push(std::move(frame))) {
        return true;
    }
    rejected_frames.Increment();
    return false;
}

RecordingCaptureWorker::RecordingCaptureWorker(
    std::unique_ptr<IRegionCaptureProvider> provider,
    media::RecordingOptions options,
    media::FrameQueue<media::VideoFrame>& output_queue)
    : provider_(std::move(provider)), options_(options), output_queue_(output_queue) {}

RecordingCaptureWorker::~RecordingCaptureWorker() {
    Stop();
}

bool RecordingCaptureWorker::Start() {
    const std::scoped_lock lifecycle_lock(lifecycle_call_mutex_);
    std::unique_lock command_lock(command_mutex_);
    if (provider_ == nullptr || !OptionsAreValid() || thread_.joinable() || !lifecycle_.Start()) {
        return false;
    }

    failed_.store(false, std::memory_order_relaxed);
    capture_attempts_.Reset();
    acquired_frames_.Reset();
    no_new_frames_.Reset();
    reused_frames_.Reset();
    failure_reused_frames_.Reset();
    accepted_frames_.Reset();
    queue_full_rejections_.Reset();
    pacing_slots_missed_.Reset();
    capture_failures_.Reset();
    capture_attempt_timing_.Reset();
    acquisition_timing_.Reset();
    region_copy_timing_.Reset();
    queue_submission_timing_.Reset();
    capture_failure_diagnostics_ = {};
    capture_in_progress_ = false;
    pacing_reset_required_ = true;
    accumulated_active_time_ = std::chrono::steady_clock::duration::zero();
    active_interval_start_ = std::chrono::steady_clock::now();
    try {
        thread_ = std::thread(&RecordingCaptureWorker::ThreadMain, this);
    } catch (...) {
        static_cast<void>(lifecycle_.RequestStop());
        static_cast<void>(lifecycle_.CompleteStop());
        failed_.store(true, std::memory_order_relaxed);
        return false;
    }
    // A successful Start must have finished its initial capture attempt before
    // Pause/Stop can overtake it. Otherwise a quick click can discard the only
    // in-flight frame and ask the MP4 writer to finalize an empty video stream.
    command_changed_.wait(command_lock, [this] {
        return (capture_attempts_.value() != 0 && !capture_in_progress_) ||
               lifecycle_.state() == RecordingWorkerState::Stopping;
    });
    return lifecycle_.state() == RecordingWorkerState::Running;
}

bool RecordingCaptureWorker::Pause() {
    const std::scoped_lock lifecycle_lock(lifecycle_call_mutex_);
    std::unique_lock command_lock(command_mutex_);
    if (!lifecycle_.Pause()) {
        return false;
    }
    accumulated_active_time_ += std::chrono::steady_clock::now() - active_interval_start_;
    pacing_reset_required_ = true;
    command_changed_.notify_all();
    command_changed_.wait(command_lock, [this] {
        return !capture_in_progress_ || lifecycle_.state() == RecordingWorkerState::Stopping;
    });
    return lifecycle_.state() == RecordingWorkerState::Paused;
}

bool RecordingCaptureWorker::Resume() {
    const std::scoped_lock lifecycle_lock(lifecycle_call_mutex_);
    const std::scoped_lock command_lock(command_mutex_);
    if (!lifecycle_.Resume()) {
        return false;
    }
    active_interval_start_ = std::chrono::steady_clock::now();
    command_changed_.notify_all();
    return true;
}

void RecordingCaptureWorker::Stop() noexcept {
    const std::scoped_lock lifecycle_lock(lifecycle_call_mutex_);
    {
        const std::scoped_lock command_lock(command_mutex_);
        if (lifecycle_.state() == RecordingWorkerState::Running) {
            accumulated_active_time_ += std::chrono::steady_clock::now() - active_interval_start_;
        }
        static_cast<void>(lifecycle_.RequestStop());
        command_changed_.notify_all();
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    const std::scoped_lock command_lock(command_mutex_);
    if (lifecycle_.state() == RecordingWorkerState::Stopping) {
        static_cast<void>(lifecycle_.CompleteStop());
    }
    capture_in_progress_ = false;
    command_changed_.notify_all();
}

RecordingWorkerState RecordingCaptureWorker::state() const noexcept {
    const std::scoped_lock lock(command_mutex_);
    return lifecycle_.state();
}

media::MediaTime RecordingCaptureWorker::active_duration() const noexcept {
    const std::scoped_lock command_lock(command_mutex_);
    auto active = accumulated_active_time_;
    if (lifecycle_.state() == RecordingWorkerState::Running) {
        active += std::chrono::steady_clock::now() - active_interval_start_;
    }
    return std::chrono::duration_cast<media::MediaTime>(active);
}

CaptureFailureDiagnostics
RecordingCaptureWorker::capture_failure_diagnostics() const noexcept {
    const std::scoped_lock command_lock(command_mutex_);
    return capture_failure_diagnostics_;
}

void RecordingCaptureWorker::RecordCaptureFailureLocked(
    media::MediaTime timestamp,
    const RegionCaptureResult& result) noexcept {
    if (capture_failure_diagnostics_.count <
        capture_failure_diagnostics_.entries.size()) {
        auto& entry = capture_failure_diagnostics_.entries[
            capture_failure_diagnostics_.count++];
        entry.capture_attempt = capture_attempts_.value();
        entry.timestamp = timestamp;
        entry.primary_failure = result.primary_failure;
        entry.fallback_failure = result.fallback_failure;
    } else if (capture_failure_diagnostics_.omitted !=
               (std::numeric_limits<uint64_t>::max)()) {
        ++capture_failure_diagnostics_.omitted;
    }
}

bool RecordingCaptureWorker::OptionsAreValid() const noexcept {
    const int64_t width = static_cast<int64_t>(options_.region.right) - options_.region.left;
    const int64_t height = static_cast<int64_t>(options_.region.bottom) - options_.region.top;
    return width > 0 && height > 0 &&
           width <= (std::numeric_limits<int>::max)() &&
           height <= (std::numeric_limits<int>::max)() &&
           static_cast<uint64_t>(width) <=
               (std::numeric_limits<std::size_t>::max)() / sizeof(uint32_t) /
                   static_cast<uint64_t>(height) &&
           options_.target_frame_rate > 0 &&
           options_.target_frame_rate <= kMaximumTargetFrameRate;
}

void RecordingCaptureWorker::ThreadMain() noexcept {
    try {
        CaptureLoop();
    } catch (...) {
        failed_.store(true, std::memory_order_relaxed);
        output_queue_.close();
        const std::scoped_lock lock(command_mutex_);
        static_cast<void>(lifecycle_.RequestStop());
        capture_in_progress_ = false;
        command_changed_.notify_all();
    }
    provider_->Release();
}

void RecordingCaptureWorker::CaptureLoop() {
    using Clock = std::chrono::steady_clock;
    static_assert(Clock::is_steady, "recording pacing requires a monotonic clock");

    const auto frame_period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / options_.target_frame_rate));
    auto next_due = Clock::now();
    uint64_t frame_index = 0;
    std::shared_ptr<const platform::BgraImage> latest_pixels;

    for (;;) {
        std::unique_lock command_lock(command_mutex_);
        if (lifecycle_.state() == RecordingWorkerState::Stopping) {
            return;
        }
        if (lifecycle_.state() == RecordingWorkerState::Paused) {
            command_changed_.wait(command_lock, [this] {
                return lifecycle_.state() != RecordingWorkerState::Paused;
            });
            continue;
        }
        if (pacing_reset_required_) {
            const auto now = Clock::now();
            const auto active = accumulated_active_time_ +
                                (now - active_interval_start_);
            const auto slot = std::chrono::duration_cast<Clock::duration>(
                MediaTimestampForFrame(frame_index, options_.target_frame_rate));
            // Resume continues the same active-time frame grid. Capturing
            // immediately on every resume rounds each short segment up to a
            // full frame and eventually puts PTS beyond the Stop endpoint.
            next_due = now + (std::max)(Clock::duration::zero(), slot - active);
            pacing_reset_required_ = false;
        }

        if (Clock::now() < next_due) {
            command_changed_.wait_until(command_lock, next_due, [this] {
                return lifecycle_.state() != RecordingWorkerState::Running ||
                       pacing_reset_required_;
            });
            continue;
        }

        const media::MediaTime timestamp =
            MediaTimestampForFrame(frame_index, options_.target_frame_rate);
        const uint64_t next_frame_index =
            frame_index == (std::numeric_limits<uint64_t>::max)() ? frame_index : frame_index + 1;
        const media::MediaTime next_timestamp =
            MediaTimestampForFrame(next_frame_index, options_.target_frame_rate);
        capture_in_progress_ = true;
        command_lock.unlock();

        media::VideoFrame frame;
        capture_attempts_.Increment();
        const auto capture_start = Clock::now();
        const RegionCaptureResult capture_result = provider_->CaptureRegionFrame(
            options_.region, options_.include_cursor, timestamp,
            next_timestamp - timestamp, frame);
        capture_attempt_timing_.Add(ElapsedMicroseconds(capture_start));
        acquisition_timing_.Add(capture_result.metrics.acquisition_microseconds);
        region_copy_timing_.Add(capture_result.metrics.region_copy_microseconds);

        command_lock.lock();
        capture_in_progress_ = false;
        command_changed_.notify_all();

        bool have_sample = false;
        if (capture_result.status == RegionCaptureStatus::FrameAcquired &&
            frame.image != nullptr && !frame.image->empty()) {
            acquired_frames_.Increment();
            latest_pixels = frame.image;
            have_sample = true;
        } else if (capture_result.status == RegionCaptureStatus::NoNewFrame) {
            no_new_frames_.Increment();
            if (latest_pixels != nullptr) {
                frame = media::VideoFrame{latest_pixels, timestamp, next_timestamp - timestamp};
                reused_frames_.Increment();
                have_sample = true;
            }
        } else if (capture_result.status == RegionCaptureStatus::Failed) {
            capture_failures_.Increment();
            RecordCaptureFailureLocked(timestamp, capture_result);
            if (latest_pixels != nullptr) {
                frame = media::VideoFrame{latest_pixels, timestamp, next_timestamp - timestamp};
                reused_frames_.Increment();
                failure_reused_frames_.Increment();
                have_sample = true;
            }
        } else {
            capture_failures_.Increment();
            RecordCaptureFailureLocked(timestamp, capture_result);
        }

        if (lifecycle_.state() != RecordingWorkerState::Running) {
            continue;
        }

        if (have_sample) {
            const auto submission_start = Clock::now();
            if (TrySubmitVideoFrameOrDropNewest(
                    output_queue_, std::move(frame), queue_full_rejections_)) {
                accepted_frames_.Increment();
            }
            queue_submission_timing_.Add(ElapsedMicroseconds(submission_start));
        }

        if (frame_index != (std::numeric_limits<uint64_t>::max)()) {
            ++frame_index;
        }
        next_due += frame_period;
        const auto now = Clock::now();
        if (next_due <= now) {
            const uint64_t missed = static_cast<uint64_t>((now - next_due) / frame_period) + 1;
            pacing_slots_missed_.Add(missed);
            const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
            frame_index = missed > maximum - frame_index ? maximum : frame_index + missed;
            next_due += frame_period * static_cast<Clock::duration::rep>(missed);
        }
    }
}

}  // namespace easnap::capture
