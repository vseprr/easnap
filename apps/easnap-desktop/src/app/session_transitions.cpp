#include "app/session_transitions.h"

#include "platform/platform_utils.h"

#include <array>
#include <utility>

namespace easnap::app {
namespace {

constexpr std::array<std::pair<SessionState, SessionState>, kSessionTransitionCount> kTransitions{{
    {SessionState::Idle, SessionState::RegionSelecting},
    {SessionState::RegionSelecting, SessionState::Idle},

    {SessionState::RegionSelecting, SessionState::ScreenshotEditing},
    {SessionState::ScreenshotEditing, SessionState::RecordingConfiguring},
    {SessionState::RecordingConfiguring, SessionState::ScreenshotEditing},
    {SessionState::ScreenshotEditing, SessionState::ScreenshotExporting},
    {SessionState::ScreenshotEditing, SessionState::Idle},
    {SessionState::ScreenshotExporting, SessionState::Idle},
    {SessionState::ScreenshotExporting, SessionState::ScreenshotEditing},

    {SessionState::RegionSelecting, SessionState::RecordingConfiguring},
    {SessionState::RecordingConfiguring, SessionState::RecordingActive},
    {SessionState::RecordingConfiguring, SessionState::Idle},
    {SessionState::RecordingActive, SessionState::RecordingPaused},
    {SessionState::RecordingPaused, SessionState::RecordingActive},
    {SessionState::RecordingActive, SessionState::RecordingFinalizing},
    {SessionState::RecordingPaused, SessionState::RecordingFinalizing},
    // A recording that could not be finalized returns to Idle; a finalized one
    // goes on to trimming (StateAfterRecordingFinalization).
    {SessionState::RecordingFinalizing, SessionState::Idle},
    {SessionState::RecordingFinalizing, SessionState::RecordingTrimming},
    {SessionState::RecordingTrimming, SessionState::Idle},
    {SessionState::RecordingTrimming, SessionState::Exporting},
    {SessionState::Exporting, SessionState::RecordingTrimming},
    {SessionState::Exporting, SessionState::Idle},

    // Scrollshot shares the provisional workspace with the other two modes.
    {SessionState::RegionSelecting, SessionState::ScrollshotConfiguring},
    {SessionState::ScreenshotEditing, SessionState::ScrollshotConfiguring},
    {SessionState::ScrollshotConfiguring, SessionState::ScreenshotEditing},
    {SessionState::RecordingConfiguring, SessionState::ScrollshotConfiguring},
    {SessionState::ScrollshotConfiguring, SessionState::RecordingConfiguring},
    {SessionState::ScrollshotConfiguring, SessionState::Idle},
    {SessionState::ScrollshotConfiguring, SessionState::ScrollshotCapturing},
    // The tools island's Scrollshot button starts capture without a Start step.
    {SessionState::ScreenshotEditing, SessionState::ScrollshotCapturing},
    // Every way out of capturing keeps the stitched image for review, except
    // ending the whole session.
    {SessionState::ScrollshotCapturing, SessionState::ScrollshotReview},
    {SessionState::ScrollshotCapturing, SessionState::Idle},
    {SessionState::ScrollshotReview, SessionState::Idle},
    // Edit hands the stitched image to the screenshot editor. A display change
    // while it is being edited hands it back, annotations included, instead of
    // cancelling as a screenshot of a desktop that no longer exists would.
    {SessionState::ScrollshotReview, SessionState::ScreenshotEditing},
    {SessionState::ScreenshotEditing, SessionState::ScrollshotReview},
}};

}  // namespace

bool IsSessionTransitionLegal(SessionState from, SessionState to) noexcept {
    if (from == to) {
        return true;
    }
    for (const auto& [source, target] : kTransitions) {
        if (source == from && target == to) {
            return true;
        }
    }
    return false;
}

PendingWork PendingWorkFor(SessionState state, bool image_at_risk) noexcept {
    switch (state) {
        case SessionState::RecordingActive:
        case SessionState::RecordingPaused:
        case SessionState::RecordingFinalizing:
            return PendingWork::Recording;
        case SessionState::RecordingTrimming:
            return PendingWork::UnsavedRecording;
        case SessionState::Exporting:
            return PendingWork::Export;
        case SessionState::ScrollshotCapturing:
        case SessionState::ScrollshotReview:
            return PendingWork::Image;
        case SessionState::ScreenshotEditing:
            return image_at_risk ? PendingWork::Image : PendingWork::None;
        case SessionState::Idle:
        case SessionState::RegionSelecting:
        case SessionState::ScreenshotExporting:
        case SessionState::RecordingConfiguring:
        case SessionState::ScrollshotConfiguring:
            return PendingWork::None;
    }
    return PendingWork::None;
}

bool ShouldTellSettingsNotSaved(SessionState state, bool dialog_open, bool already_told) noexcept {
    return state == SessionState::Idle && !dialog_open && !already_told;
}

const wchar_t* ToString(SessionState state) noexcept {
    switch (state) {
        case SessionState::Idle:
            return L"Idle";
        case SessionState::RegionSelecting:
            return L"RegionSelecting";
        case SessionState::ScreenshotEditing:
            return L"ScreenshotEditing";
        case SessionState::ScreenshotExporting:
            return L"ScreenshotExporting";
        case SessionState::RecordingConfiguring:
            return L"RecordingConfiguring";
        case SessionState::RecordingActive:
            return L"RecordingActive";
        case SessionState::RecordingPaused:
            return L"RecordingPaused";
        case SessionState::RecordingFinalizing:
            return L"RecordingFinalizing";
        case SessionState::RecordingTrimming:
            return L"RecordingTrimming";
        case SessionState::Exporting:
            return L"Exporting";
        case SessionState::ScrollshotConfiguring:
            return L"ScrollshotConfiguring";
        case SessionState::ScrollshotCapturing:
            return L"ScrollshotCapturing";
        case SessionState::ScrollshotReview:
            return L"ScrollshotReview";
    }
    return L"Unknown";
}

bool IsScrollshotState(SessionState state) noexcept {
    return state == SessionState::ScrollshotConfiguring || state == SessionState::ScrollshotCapturing ||
           state == SessionState::ScrollshotReview;
}

bool IsRecordingState(SessionState state) noexcept {
    switch (state) {
        case SessionState::RecordingConfiguring:
        case SessionState::RecordingActive:
        case SessionState::RecordingPaused:
        case SessionState::RecordingFinalizing:
        case SessionState::RecordingTrimming:
        case SessionState::Exporting:
            return true;
        default:
            return false;
    }
}

bool SessionStateMachine::RequestTransition(SessionState next) {
    if (!IsSessionTransitionLegal(state_, next)) {
        platform::LogMessage(L"Rejected illegal transition %s -> %s", ToString(state_), ToString(next));
        return false;
    }
    if (state_ != next) {
        platform::LogMessage(L"State %s -> %s", ToString(state_), ToString(next));
    }
    state_ = next;
    return true;
}
}  // namespace easnap::app
