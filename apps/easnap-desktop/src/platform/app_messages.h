// Easnap - the allocation table of every WM_APP message the application uses.
//
// Each private window message of Easnap is defined here and nowhere else, so a
// new message takes a free value from one list and two messages one window
// receives cannot silently share a value. A constant lives in the namespace of
// the code that owns its window; the header itself sits in platform/ so every
// layer may include it. Messages registered by name (RegisterWindowMessageW)
// stay with their users.
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>

// ---------------------------------------------------------------------------
// Host window (main.cpp): the hidden tray window that owns the message loop.
// ---------------------------------------------------------------------------
namespace easnap::app {

/// Notification-icon callback: posted by the shell, handled by the host window.
inline constexpr UINT kMsgTrayIcon = WM_APP + 1;
/// Posted by the coordinator to the host window so windows are destroyed
/// outside their own window procedure (destroying a window from inside its
/// callback is unsafe).
inline constexpr UINT kMsgSessionCleanup = WM_APP + 11;
/// Posted by the recording finalizer to the host window after it has stopped
/// every recording worker and closed (or safely aborted) the temporary MP4.
/// WPARAM carries its generation.
inline constexpr UINT kMsgRecordingFinalized = WM_APP + 12;
/// Posted by the trim export worker to the host window when its result can be
/// reconciled with the session on the UI thread. WPARAM carries the export's
/// generation.
inline constexpr UINT kMsgTrimExportCompleted = WM_APP + 13;
/// Posted by a Scrollshot capture worker to the host window when its progress
/// changed. WPARAM carries the capture's generation; pixels never travel in
/// the message.
inline constexpr UINT kMsgScrollshotProgress = WM_APP + 14;
/// Posted by the coordinator to the host window when a save of the settings
/// failed and the user may be told now.
inline constexpr UINT kMsgSettingsNotSaved = WM_APP + 16;
/// Posted by the video probe worker to the host window when its result can be
/// taken on the UI thread. WPARAM carries the probe's generation.
inline constexpr UINT kMsgVideoProbeCompleted = WM_APP + 18;
/// Posted by OpenVideoRequests to the host window once it has kept a request.
inline constexpr UINT kMsgOpenVideoRequest = WM_APP + 19;

}  // namespace easnap::app

// ---------------------------------------------------------------------------
// Image export: received by whichever window started the export.
// ---------------------------------------------------------------------------
namespace easnap::platform {

/// Posted by an ImageExportJob worker to the window that started the export
/// once it has finished: the editor, a pin or the host window (Scrollshot).
/// WPARAM carries the export's generation; the result is taken with TryTake.
inline constexpr UINT kMsgImageExportCompleted = WM_APP + 15;

}  // namespace easnap::platform

// ---------------------------------------------------------------------------
// Theme broadcast: sent to every top-level window of the thread and each of
// its children, so no window or control may use this value for anything else.
// ---------------------------------------------------------------------------
namespace easnap::ui::theme {

/// Sent by theme::NotifyWindows after the palette changed; handled by every
/// themed window and control.
inline constexpr UINT kChangedMessage = WM_APP + 0x310;

}  // namespace easnap::ui::theme

// ---------------------------------------------------------------------------
// Private messages of one window each. They share values with the host
// window's because no window receives two of them.
// ---------------------------------------------------------------------------
namespace easnap::ui {

/// Notification message the trim window gives its media preview; posted to the
/// trim window when the preview's state changed.
inline constexpr UINT kMsgTrimPreviewEvent = WM_APP + 1;

}  // namespace easnap::ui

namespace easnap::media {

/// Posted by the preview process client to its own message-only window to poll
/// the video size outside the caller's stack.
inline constexpr UINT kMsgPreviewProcessResizePoll = WM_APP + 1;
/// Notification message the preview host process (EasnapPreviewHost) gives its
/// media preview; posted to that process's own window, which only wakes the loop.
inline constexpr UINT kMsgPreviewHostChanged = WM_APP + 1;
/// Posted by the media preview's presentation wait to its own message-only
/// window so a presented frame is handled on the owner thread.
inline constexpr UINT kMsgMediaPreviewPresentation = WM_APP + 17;

}  // namespace easnap::media

namespace easnap::platform::app_messages_detail {

/// True when no two values in `messages` are equal.
template <std::size_t N>
[[nodiscard]] constexpr bool AllDistinct(const std::array<UINT, N>& messages) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t j = i + 1; j < N; ++j) {
            if (messages[i] == messages[j]) return false;
        }
    }
    return true;
}

static_assert(AllDistinct(std::array<UINT, 10>{
                  app::kMsgTrayIcon, app::kMsgSessionCleanup, app::kMsgRecordingFinalized,
                  app::kMsgTrimExportCompleted, app::kMsgScrollshotProgress,
                  platform::kMsgImageExportCompleted, app::kMsgSettingsNotSaved,
                  app::kMsgVideoProbeCompleted, app::kMsgOpenVideoRequest,
                  ui::theme::kChangedMessage}),
              "the messages the host window receives must be distinct");
static_assert(AllDistinct(std::array<UINT, 2>{ui::kMsgTrimPreviewEvent, ui::theme::kChangedMessage}),
              "the messages the trim window receives must be distinct");
static_assert(AllDistinct(std::array<UINT, 2>{platform::kMsgImageExportCompleted, ui::theme::kChangedMessage}),
              "the messages the editor and a pin receive must be distinct");

}  // namespace easnap::platform::app_messages_detail
