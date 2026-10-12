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

}  // namespace easnap::app
