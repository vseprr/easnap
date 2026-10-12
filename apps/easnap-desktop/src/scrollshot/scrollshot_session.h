// Easnap - one scrolling-screenshot capture: worker, stitching and preview.
//
// The session owns a capture provider of its own and a worker thread. The
// worker reads the fixed screen rectangle whenever it changes, hands each
// picture to the stitching engine and keeps a small preview up to date.
// Nothing here runs on the UI thread except the cheap accessors below; the UI
// learns about progress through throttled window messages that carry only the
// session generation.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "capture/capture_provider.h"
#include "platform/platform_utils.h"
#include "scrollshot/stitch_engine.h"

namespace easnap::scrollshot {

enum class ScrollshotCondition : uint8_t {
    Following,      ///< The picture last examined was placed.
    Lost,           ///< Recent pictures could not be placed; nothing is being added.
    AtLimit,        ///< The image reached its size limit; nothing more is added.
    CaptureFailed,  ///< The screen rectangle can no longer be read.
};

struct ScrollshotProgress {
    uint64_t generation{0};
    int width{0};
    int height{0};  ///< Stitched rows so far.
    ScrollshotCondition condition{ScrollshotCondition::Following};
    FrameOutcome last_outcome{FrameOutcome::Invalid};
    uint32_t placed_frames{0};
    uint64_t preview_version{0};
    bool finished{false};  ///< The worker has stopped and the result is ready.
};

/// The lower end of the stitched image, reduced to the preview width.
struct ScrollshotPreview {
    int width{0};
    int height{0};
    int total_height{0};  ///< Height of the whole reduced image.
    uint64_t version{0};
    std::vector<uint32_t> pixels;  ///< width x height, top-down BGRA.
};

class ScrollshotSession final {
public:
    struct Options {
        RECT region{};
        /// Echoed in every notification so a stale message is recognisable.
        uint64_t generation{0};
        HWND notify_window{nullptr};
        UINT notify_message{0};
        StitchLimits limits{};
        int preview_width{160};
    };

    ScrollshotSession(std::unique_ptr<capture::IRegionCaptureProvider> provider, Options options);
    ~ScrollshotSession();

    ScrollshotSession(const ScrollshotSession&) = delete;
    ScrollshotSession& operator=(const ScrollshotSession&) = delete;

    /// Starts the worker and waits until the first picture is on the canvas.
    /// False when the rectangle could not be captured within `deadline`.
    [[nodiscard]] bool Start(std::chrono::milliseconds deadline = std::chrono::milliseconds(3000));
    /// Asks the worker to finish and waits until it has composed the result.
    /// False when `deadline` passed first; the request stays and the call may
    /// be repeated.
    [[nodiscard]] bool Stop(std::chrono::milliseconds deadline);

    [[nodiscard]] ScrollshotProgress progress() const;
    /// Copies at most `max_rows` rows from the end of the preview.
    void CopyPreview(int max_rows, ScrollshotPreview& out) const;
    /// The stitched image. Available once, after Stop returned true.
    [[nodiscard]] platform::BgraImage TakeResult();

private:
    void ThreadMain() noexcept;
    void CaptureLoop();
    void Examine(const platform::BgraImage& frame);
    void Publish(bool force);
    void Finish() noexcept;
    [[nodiscard]] bool WaitInterruptibly(std::chrono::milliseconds duration);

    std::unique_ptr<capture::IRegionCaptureProvider> provider_;
    const Options options_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    bool stop_requested_{false};
    bool first_frame_seen_{false};
    bool worker_finished_{false};
    ScrollshotProgress progress_;
    std::vector<uint32_t> preview_;  ///< preview_width_ x preview_rows_
    int preview_width_{0};
    int preview_rows_{0};
    platform::BgraImage result_;
    mutable std::atomic<bool> notification_pending_{false};

    // Worker-only state.
    std::unique_ptr<StitchEngine> engine_;
    int dirty_begin_{0};  ///< Canvas rows changed since the preview was built.
    int dirty_end_{0};
    bool unplaced_{false};  ///< The picture last examined could not be placed.
    std::chrono::steady_clock::time_point unplaced_since_{};
    uint32_t failure_run_{0};
    bool capture_failed_{false};
    bool progress_changed_{false};
    std::chrono::steady_clock::time_point last_publish_{};
};

}  // namespace easnap::scrollshot
