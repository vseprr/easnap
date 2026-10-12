#include "app/session_transitions.h"

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
    // The bounded UI slice safely retains the temporary MP4 and returns to
    // Idle. The trimming branch remains available for its later slice.
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

}  // namespace easnap::app
