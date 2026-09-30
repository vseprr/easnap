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
};

inline constexpr std::size_t kSessionStateCount = 10;
inline constexpr std::size_t kSessionTransitionCount = 22;

/// Returns true for identity transitions and every edge in the production
/// session graph. This is the sole authority for transition legality.
[[nodiscard]] bool IsSessionTransitionLegal(SessionState from, SessionState to) noexcept;

}  // namespace easnap::app
