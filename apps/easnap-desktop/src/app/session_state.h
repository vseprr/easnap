// Easnap - session state machine and capture coordinator.
//
// Modal ownership rule (see docs/user-flows.md): only the CaptureCoordinator may
// move Easnap between states. UI controls issue commands; they never mutate
// capture state on their own. Every method below must be called on the UI thread
// that owns the message loop.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "app/session_transitions.h"
#include "app/app_settings.h"
#include "app/recording_completion.h"
#include "app/recording_finalizer.h"
#include "app/trim_workflow.h"
#include "capture/capture_provider.h"
#include "media/media_interfaces.h"
#include "platform/platform_utils.h"

namespace easnap::ui {
class OverlayController;
class RecordingControls;
class TrimWindow;
}
namespace easnap::editor {
class ScreenshotEditor;
}
namespace easnap::media {
class ITemporarySessionStore;
enum class AudioSource;
}
namespace easnap::recording {
class RecordingSession;
}

namespace easnap::app {

/// Posted to the host window so windows are destroyed outside their own
/// window procedure (destroying a window from inside its callback is unsafe).
inline constexpr UINT kMsgSessionCleanup = WM_APP + 11;
/// Posted after the background finalizer has stopped every recording worker and
/// closed (or safely aborted) the temporary MP4. WPARAM carries its generation.
inline constexpr UINT kMsgRecordingFinalized = WM_APP + 12;
/// Posted when the trim export worker has stopped and its result can be
/// reconciled with the coordinator-owned session on the UI thread.
inline constexpr UINT kMsgTrimExportCompleted = WM_APP + 13;

/// The full v1 session lifecycle. Recording states exist already so Phase 2 only
/// adds behaviour, never new control flow.
///
///   Idle
///     -> RegionSelecting
///     -> ScreenshotEditing -> ScreenshotExporting -> Idle
///     -> RecordingConfiguring -> RecordingActive <-> RecordingPaused
///     -> RecordingFinalizing -> RecordingTrimming -> Exporting -> Idle
[[nodiscard]] const wchar_t* ToString(SessionState state) noexcept;
/// True while a recording session holds data that must not be discarded.
[[nodiscard]] bool IsRecordingState(SessionState state) noexcept;

enum class CaptureMode { Screenshot, Record };


/// Owns session creation, enforces legal state transitions and wires the UI
/// components together. Singleton because exactly one capture session may exist.
class CaptureCoordinator {
public:
    static CaptureCoordinator& Instance();

    CaptureCoordinator(const CaptureCoordinator&) = delete;
    CaptureCoordinator& operator=(const CaptureCoordinator&) = delete;

    /// Prepares the coordinator. Deliberately does not create capture, encoder
    /// or renderer resources: Idle must stay passive.
    bool Initialize(HWND host_window);
    void Shutdown();

    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] static bool IsTransitionLegal(SessionState from, SessionState to) noexcept;
    /// Attempts a transition; illegal transitions are rejected and logged.
    bool RequestTransition(SessionState next);

    /// Ctrl + Shift + A (or the configured shortcut) was pressed.
    void OnGlobalHotkey();
    /// Escape from any overlay/editor surface.
    void OnEscapeRequested();
    /// WM_DISPLAYCHANGE / monitor layout change while a session is open.
    void OnDisplayChanged();
    /// Handles kMsgSessionCleanup: destroys windows retired during callbacks.
    void OnDeferredCleanup();
    /// Handles kMsgRecordingFinalized on the UI thread.
    void OnRecordingFinalized(RecordingFinalizer::Generation generation);
    /// Handles kMsgTrimExportCompleted on the UI thread.
    void OnTrimExportCompleted(uint64_t export_generation);
    /// Handles WM_CLIPBOARDUPDATE for clipboard-backed recording cleanup.
    void OnClipboardChanged();
    /// Enables native Tab/Enter/Esc navigation in recording/trim surfaces.
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const;

    [[nodiscard]] AppSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const AppSettings& settings() const noexcept { return settings_; }
    bool PersistSettings();
    void SetAppearanceTheme(AppearanceTheme preference);
    void OnVisualSettingsChanged();

    [[nodiscard]] HWND host_window() const noexcept { return host_window_; }

private:
    CaptureCoordinator() = default;
    ~CaptureCoordinator();

    bool BeginCaptureSession();
    void OnWorkspaceEditingChanged(CaptureMode mode, const RECT& region);
    void OnWorkspaceRegionChanged(const RECT& region);
    void CompleteWorkspaceScreenshot();
    bool PrepareWorkspaceScreenshot();
    void OnOverlayCancelled();
    void OnEditorClosed(bool exported);
    void OpenEditor(const RECT& region);
    void BeginRecordingConfiguration(const RECT& region);
    void OnRecordingStart(RecordingConfiguration configuration);
    void OnRecordingStartFailed(const std::wstring& message);
    void OnRecordingCancel();
    void OnRecordingPause();
    void OnRecordingResume();
    void OnRecordingStop();
    void OnRecordingStatusTick();
    void BeginRecordingFinalization();
    void FinishRecordingWorkflow(bool finalized);
    void StopRecordingForShutdown();
    void BeginTrimWorkflow(const std::wstring& source_path);
    enum class PendingTrimAction { None, Save, Copy };
    void OnTrimSaveRequested(media::MediaTime start, media::MediaTime end);
    void OnTrimCopyRequested(media::MediaTime start, media::MediaTime end);
    void BeginTrimAction(PendingTrimAction action, media::MediaTime start,
                         media::MediaTime end,
                         std::wstring save_destination = {},
                         bool replace_existing = false);
    void StartTrimExport(media::MediaTime start, media::MediaTime end,
                         std::wstring destination_path = {},
                         bool destination_is_clipboard = false,
                         bool destination_is_save = false,
                         bool replace_existing = false);
    void CompletePendingTrimAction();
    void OnTrimExportCancel();
    void OnTrimWindowClosed();
    void OnTrimStatusTick();
    void FinishTrimWorkflow(bool delete_active_session);
    void StopTrimForShutdown();
    void EndSession();
    void ScheduleCleanup();

    HWND host_window_{nullptr};
    SessionState state_{SessionState::Idle};
    AppSettings settings_{};

    std::unique_ptr<capture::IRegionCaptureProvider> capture_provider_;
    std::unique_ptr<media::ITemporarySessionStore> temp_store_;
    std::unique_ptr<RecordingCompletionController> recording_completion_;
    std::unique_ptr<ui::OverlayController> overlay_;
    std::unique_ptr<ui::RecordingControls> recording_controls_;
    std::unique_ptr<ui::TrimWindow> trim_window_;
    std::unique_ptr<editor::ScreenshotEditor> editor_;
    std::unique_ptr<recording::RecordingSession> recording_session_;
    std::unique_ptr<TrimExportController> trim_export_;
    capture::DesktopSnapshot snapshot_;
    RECT recording_region_{};
    /// Monitor bounds and scale the frozen snapshot was taken against.
    std::vector<platform::MonitorInfo> session_monitors_;
    media::RecordingOptions recording_options_{};
    std::wstring trim_source_path_;
    TrimArtifact trim_artifact_;
    RecordingFinalizer recording_finalizer_;
    std::atomic<uint64_t> trim_export_generation_{0};
    media::MediaTime trim_export_start_{0};
    media::MediaTime trim_export_end_{0};
    bool trim_export_to_clipboard_{false};
    bool trim_export_to_save_{false};
    PendingTrimAction pending_trim_action_{PendingTrimAction::None};
    media::MediaTime pending_trim_start_{0};
    media::MediaTime pending_trim_end_{0};
    std::wstring pending_save_destination_;
    bool pending_save_replace_{false};
    uint64_t pending_trim_action_started_ms_{0};
    bool trim_close_pending_{false};

    bool overlay_retired_{false};
    bool recording_controls_retired_{false};
    bool trim_window_retired_{false};
    bool editor_retired_{false};
    bool initialized_{false};
};

}  // namespace easnap::app
