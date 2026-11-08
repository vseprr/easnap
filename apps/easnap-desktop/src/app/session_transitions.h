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

/// The state's name in log lines.
[[nodiscard]] const wchar_t* ToString(SessionState state) noexcept;
/// True while a recording session holds data that must not be discarded.
[[nodiscard]] bool IsRecordingState(SessionState state) noexcept;
/// True from the moment Scrollshot is chosen until its image has been handed on.
[[nodiscard]] bool IsScrollshotState(SessionState state) noexcept;

/// The current session state and the only way to change it (ADR-039). The
/// coordinator owns one; everything that reads or changes the state goes
/// through it, and nothing keeps a copy.
class SessionStateMachine final {
public:
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    /// Moves to `next` along an edge of the production graph and logs the
    /// change. An illegal edge is rejected and logged; the state stays.
    bool RequestTransition(SessionState next);
    /// Back to Idle without a transition: only for the start and the end of
    /// the application, when no session can exist.
    void Reset() noexcept { state_ = SessionState::Idle; }

private:
    SessionState state_{SessionState::Idle};
};

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
