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
