// Easnap - what a display change does to the running session.
//
// The coordinator gathers the facts when Windows reports a changed display
// layout, scale or work area, asks DecideDisplayChange what follows from them,
// and carries that out. The order of the rules is the decision: a running
// export defers everything, a Scrollshot or an edited stitched image keeps its
// image, a visible capture workspace is cancelled before a recording is looked
// at, and a recording ends only when its region left the desktop.
#pragma once

#include "app/session_transitions.h"

namespace easnap::app {

/// What the coordinator knows about the session when the display changed.
struct DisplayChangeFacts {
    SessionState state{SessionState::Idle};
    /// The session's image is being copied or saved.
    bool export_running{false};
    /// The monitors' bounds or scale differ from those the session began with.
    bool layout_changed{false};
    /// The editor shows a document (a stitched image), not a desktop region.
    bool editing_document{false};
    /// The capture workspace (frozen desktop and frame) is on screen.
    bool workspace_visible{false};
    /// The recording region still lies wholly on the virtual screen.
    bool recording_region_on_desktop{true};
    /// The trim window of a finished recording is on screen.
    bool trim_window_visible{false};
};

/// What the coordinator does about a display change.
enum class DisplayChangeAction {
    None,                            ///< Nothing changes.
    RecheckAfterExport,              ///< Look again once the running export has ended.
    KeepScrollshotImage,             ///< Stop a Scrollshot capture, keep its image, island to a monitor.
    ReturnDocumentToReview,          ///< The edited stitched image goes back to Scrollshot review.
    CancelSession,                   ///< The frozen desktop is stale: cancel the selection.
    StopRecording,                   ///< The recording region left the desktop: stop and keep the video.
    CancelRecordingConfiguration,    ///< The region left the desktop before recording began.
    RefreshTrimLayout,               ///< The recording stays; reposition the trim window.
    RefreshRecordingControlsLayout,  ///< The recording stays; reposition its controls.
};

/// The action for `facts`; the first rule that applies decides.
[[nodiscard]] DisplayChangeAction DecideDisplayChange(const DisplayChangeFacts& facts) noexcept;

}  // namespace easnap::app
