#include "app/display_change.h"

namespace easnap::app {

DisplayChangeAction DecideDisplayChange(const DisplayChangeFacts& facts) noexcept {
    const SessionState state = facts.state;
    if (state == SessionState::Idle) {
        return DisplayChangeAction::None;
    }
    if (facts.export_running) {
        return DisplayChangeAction::RecheckAfterExport;
    }
    if (state == SessionState::ScrollshotCapturing || state == SessionState::ScrollshotReview) {
        return facts.layout_changed ? DisplayChangeAction::KeepScrollshotImage : DisplayChangeAction::None;
    }
    if (state == SessionState::ScreenshotEditing && facts.editing_document) {
        return facts.layout_changed ? DisplayChangeAction::ReturnDocumentToReview : DisplayChangeAction::None;
    }
    if (facts.workspace_visible) {
        return facts.layout_changed ? DisplayChangeAction::CancelSession : DisplayChangeAction::None;
    }
    if (IsRecordingState(state)) {
        const bool capturing = state == SessionState::RecordingConfiguring ||
                               state == SessionState::RecordingActive || state == SessionState::RecordingPaused;
        if (capturing && !facts.recording_region_on_desktop) {
            if (state == SessionState::RecordingActive || state == SessionState::RecordingPaused) {
                return DisplayChangeAction::StopRecording;
            }
            if (state == SessionState::RecordingConfiguring) {
                return DisplayChangeAction::CancelRecordingConfiguration;
            }
        }
        return facts.trim_window_visible ? DisplayChangeAction::RefreshTrimLayout
                                         : DisplayChangeAction::RefreshRecordingControlsLayout;
    }
    return DisplayChangeAction::None;
}

}  // namespace easnap::app
