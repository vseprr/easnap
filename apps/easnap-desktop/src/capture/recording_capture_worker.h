// Easnap - production recording frame-capture worker.
#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

#include "capture/capture_provider.h"
#include "media/frame_queue.h"
#include "media/media_interfaces.h"

namespace easnap::capture {

/// Three queued 4K BGRA frames consume 99,532,800 bytes (94.921875 MiB) of
/// pixel payload. Callers may choose another fixed capacity deliberately.
inline constexpr std::size_t kDefaultVideoFrameQueueCapacity = 3;
inline constexpr uint32_t kMaximumTargetFrameRate = 240;
inline constexpr std::size_t kCaptureFailureDiagnosticCapacity = 8;

enum class RecordingWorkerState { Stopped, Running, Paused, Stopping };

/// Deterministic lifecycle state used by the worker. The owner supplies
/// synchronization; this type contains no hidden command or event queue.
class RecordingWorkerLifecycle final {
public:
    [[nodiscard]] RecordingWorkerState state() const noexcept { return state_; }
    [[nodiscard]] bool Start() noexcept;
    [[nodiscard]] bool Pause() noexcept;
    [[nodiscard]] bool Resume() noexcept;
    [[nodiscard]] bool RequestStop() noexcept;
    [[nodiscard]] bool CompleteStop() noexcept;

private:
    RecordingWorkerState state_{RecordingWorkerState::Stopped};
};

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

struct AggregateTiming {
    uint64_t count{0};
    uint64_t total_microseconds{0};
    uint64_t minimum_microseconds{0};
    uint64_t maximum_microseconds{0};

    [[nodiscard]] uint64_t average_microseconds() const noexcept {
        return count == 0 ? 0 : total_microseconds / count;
    }
};

/// Constant-space aggregate timing. All values saturate rather than wrapping.
class TimingAccumulator final {
public:
    void Add(uint64_t microseconds) noexcept;
    void Reset() noexcept;
    [[nodiscard]] AggregateTiming snapshot() const noexcept;

private:
    SaturatingCounter count_;
    SaturatingCounter total_microseconds_;
    std::atomic<uint64_t> minimum_microseconds_{(std::numeric_limits<uint64_t>::max)()};
    std::atomic<uint64_t> maximum_microseconds_{0};
};

[[nodiscard]] media::MediaTime MediaTimestampForFrame(uint64_t frame_index,
                                                       uint32_t frame_rate) noexcept;

struct CaptureFailureDiagnosticEntry {
    uint64_t capture_attempt{0};
    media::MediaTime timestamp{};
    RegionCaptureFailureDiagnostic primary_failure;
    RegionCaptureFailureDiagnostic fallback_failure;
};

struct CaptureFailureDiagnostics {
    std::array<CaptureFailureDiagnosticEntry,
               kCaptureFailureDiagnosticCapacity> entries{};
    std::size_t count{0};
    uint64_t omitted{0};
};

/// Explicit overload policy: retain queued frames and drop the newest frame.
[[nodiscard]] bool TrySubmitVideoFrameOrDropNewest(media::FrameQueue<media::VideoFrame>& queue,
                                                   media::VideoFrame&& frame,
                                                   SaturatingCounter& rejected_frames);

/// Owns the capture provider and its worker thread. The output queue is not
/// owned and must outlive this object. Lifecycle commands are serialized and
/// may be issued without touching the Win32 UI/message thread's capture path.
class RecordingCaptureWorker final {
public:
    RecordingCaptureWorker(std::unique_ptr<IRegionCaptureProvider> provider,
                           media::RecordingOptions options,
                           media::FrameQueue<media::VideoFrame>& output_queue);
    ~RecordingCaptureWorker();

    RecordingCaptureWorker(const RecordingCaptureWorker&) = delete;
    RecordingCaptureWorker& operator=(const RecordingCaptureWorker&) = delete;

    [[nodiscard]] bool Start();
    [[nodiscard]] bool Pause();
    [[nodiscard]] bool Resume();
    void Stop() noexcept;

    [[nodiscard]] RecordingWorkerState state() const noexcept;
    [[nodiscard]] uint64_t capture_attempts() const noexcept { return capture_attempts_.value(); }
    [[nodiscard]] uint64_t acquired_frames() const noexcept { return acquired_frames_.value(); }
    [[nodiscard]] uint64_t no_new_frames() const noexcept { return no_new_frames_.value(); }
    [[nodiscard]] uint64_t reused_frames() const noexcept { return reused_frames_.value(); }
    [[nodiscard]] uint64_t failure_reused_frames() const noexcept {
        return failure_reused_frames_.value();
    }
    [[nodiscard]] uint64_t accepted_frames() const noexcept { return accepted_frames_.value(); }
    [[nodiscard]] uint64_t queue_full_rejections() const noexcept {
        return queue_full_rejections_.value();
    }
    [[nodiscard]] uint64_t pacing_slots_missed() const noexcept {
        return pacing_slots_missed_.value();
    }
    [[nodiscard]] uint64_t capture_failures() const noexcept { return capture_failures_.value(); }
    [[nodiscard]] CaptureFailureDiagnostics capture_failure_diagnostics() const noexcept;
    [[nodiscard]] AggregateTiming capture_attempt_timing() const noexcept {
        return capture_attempt_timing_.snapshot();
    }
    [[nodiscard]] AggregateTiming acquisition_timing() const noexcept {
        return acquisition_timing_.snapshot();
    }
    [[nodiscard]] AggregateTiming region_copy_timing() const noexcept {
        return region_copy_timing_.snapshot();
    }
    [[nodiscard]] AggregateTiming queue_submission_timing() const noexcept {
        return queue_submission_timing_.snapshot();
    }
    [[nodiscard]] media::MediaTime active_duration() const noexcept;
    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_relaxed); }

private:
    [[nodiscard]] bool OptionsAreValid() const noexcept;
    void ThreadMain() noexcept;
    void CaptureLoop();
    void RecordCaptureFailureLocked(
        media::MediaTime timestamp,
        const RegionCaptureResult& result) noexcept;

    std::unique_ptr<IRegionCaptureProvider> provider_;
    media::RecordingOptions options_;
    media::FrameQueue<media::VideoFrame>& output_queue_;

    mutable std::mutex command_mutex_;
    std::mutex lifecycle_call_mutex_;
    std::condition_variable command_changed_;
    RecordingWorkerLifecycle lifecycle_;
    std::thread thread_;
    bool capture_in_progress_{false};
    bool pacing_reset_required_{false};
    std::chrono::steady_clock::time_point active_interval_start_{};
    std::chrono::steady_clock::duration accumulated_active_time_{};
    CaptureFailureDiagnostics capture_failure_diagnostics_;

    SaturatingCounter capture_attempts_;
    SaturatingCounter acquired_frames_;
    SaturatingCounter no_new_frames_;
    SaturatingCounter reused_frames_;
    SaturatingCounter failure_reused_frames_;
    SaturatingCounter accepted_frames_;
    SaturatingCounter queue_full_rejections_;
    SaturatingCounter pacing_slots_missed_;
    SaturatingCounter capture_failures_;
    TimingAccumulator capture_attempt_timing_;
    TimingAccumulator acquisition_timing_;
    TimingAccumulator region_copy_timing_;
    TimingAccumulator queue_submission_timing_;
    std::atomic<bool> failed_{false};
};

}  // namespace easnap::capture
