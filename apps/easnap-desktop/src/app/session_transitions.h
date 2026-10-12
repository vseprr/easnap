// Easnap - authoritative session state transition rules.
#pragma once

#include <cstddef>

namespace easnap::app {

enum class SessionState {
    Idle,
    RegionSelecting,
    ScreenshotEditing,
    ScreenshotExporting,
    RecordingConfiguring,
    RecordingActive,
    RecordingPaused,
    RecordingFinalizing,
    RecordingTrimming,
    Exporting,
    /// Scrollshot is chosen and the frame can still be moved and resized.
    ScrollshotConfiguring,
    /// The frame is locked and its content is being captured and stitched.
    ScrollshotCapturing,
    /// Capture has stopped; the stitched image waits for Copy, Save, Edit or Discard.
    ScrollshotReview,
};

inline constexpr std::size_t kSessionStateCount = 13;
inline constexpr std::size_t kSessionTransitionCount = 35;

/// Returns true for identity transitions and every edge in the production
/// session graph. This is the sole authority for transition legality.
[[nodiscard]] bool IsSessionTransitionLegal(SessionState from, SessionState to) noexcept;

/// What ending the application right now would lose.
enum class PendingWork {
    None,
    Recording,         ///< A recording is running, paused or being finalized.
    UnsavedRecording,  ///< A finished recording is open and not saved or copied.
    Export,            ///< A finished recording is being exported.
    Image,             ///< An annotated screenshot or a stitched image is held.
};

/// `image_at_risk` tells whether the image being edited carries work: a plain
/// screenshot without annotations can be taken again and does not count.
[[nodiscard]] PendingWork PendingWorkFor(SessionState state, bool image_at_risk) noexcept;

}  // namespace easnap::app
