// Easnap - session state machine and capture coordinator.
//
// Ownership rule (see docs/architecture.md, "Processes and ownership"): only the CaptureCoordinator may
// move Easnap between states. UI controls issue commands; they never mutate
// capture state on their own. Every method below must be called on the UI thread
// that owns the message loop.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/session_transitions.h"
#include "app/app_settings.h"
#include "app/recording_completion.h"
#include "app/recording_finalizer.h"
#include "app/trim_flow.h"
#include "capture/capture_provider.h"
#include "media/media_interfaces.h"
#include "platform/platform_utils.h"

namespace easnap::ui {
class OverlayController;
class PinnedImages;
class RecordingControls;
class ScrollshotControls;
enum class ScrollshotCommand;
}  // namespace easnap::ui
namespace easnap::scrollshot {
class ScrollshotSession;
}  // namespace easnap::scrollshot
namespace easnap::editor {
class ScreenshotEditor;
struct ImageView;
}  // namespace easnap::editor
namespace easnap::media {
class ITemporarySessionStore;
enum class AudioSource;
}  // namespace easnap::media
namespace easnap::recording {
class RecordingSession;
enum class RecordingStartFailure;
}  // namespace easnap::recording

namespace easnap::app {

/// Posted to the host window so windows are destroyed outside their own
/// window procedure (destroying a window from inside its callback is unsafe).
inline constexpr UINT kMsgSessionCleanup = WM_APP + 11;
/// Posted after the background finalizer has stopped every recording worker and
/// closed (or safely aborted) the temporary MP4. WPARAM carries its generation.
inline constexpr UINT kMsgRecordingFinalized = WM_APP + 12;
// WM_APP + 13 is kMsgTrimExportCompleted, in app/trim_flow.h.
/// Posted by a Scrollshot capture worker when its progress changed. WPARAM
/// carries the capture's generation; pixels never travel in the message.
inline constexpr UINT kMsgScrollshotProgress = WM_APP + 14;
/// Timer on the host window: retired windows are destroyed once no dialog of
/// theirs is running its own message loop any more.
inline constexpr UINT_PTR kTimerDeferredCleanup = 0xEA50;

/// The session lifecycle; session_transitions.cpp lists the legal transitions.
///
///   Idle
///     -> RegionSelecting
///     -> ScreenshotEditing -> ScreenshotExporting -> Idle
///     -> RecordingConfiguring -> RecordingActive <-> RecordingPaused
///     -> RecordingFinalizing -> RecordingTrimming -> Exporting -> Idle
///     -> ScrollshotConfiguring -> ScrollshotCapturing -> ScrollshotReview
///        -> Idle, or -> ScreenshotEditing to annotate the stitched image
///
/// `SessionStateMachine` (session_transitions.h) holds the current state.

enum class CaptureMode { Screenshot,
                         Record,
                         Scrollshot };


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

    [[nodiscard]] SessionState state() const noexcept { return machine_.state(); }
    [[nodiscard]] static bool IsTransitionLegal(SessionState from, SessionState to) noexcept;
    /// Attempts a transition; illegal transitions are rejected and logged.
    bool RequestTransition(SessionState next);

    /// Ctrl + Shift + A (or the configured shortcut) was pressed.
    void OnGlobalHotkey();
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
    /// Handles kMsgScrollshotProgress on the UI thread.
    void OnScrollshotProgress(uint64_t generation);
    /// Enables native Tab/Enter/Esc navigation in recording/trim surfaces.
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const;

    [[nodiscard]] AppSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const AppSettings& settings() const noexcept { return settings_; }
    bool PersistSettings();
    /// An explicit Light or Dark choice; ends following Windows.
    void SetAppearanceTheme(AppearanceTheme preference);
    /// Light or Dark as Windows is set for applications, from now on.
    void SetAppearanceFollowsSystem();
    /// Applies the appearance the settings describe to every window. Call
    /// after changing the appearance fields of `settings()`.
    void RefreshAppearance();
    void OnVisualSettingsChanged();

    /// What ending the application now would lose.
    [[nodiscard]] PendingWork pending_work() const;
    /// Tray Exit or a close request. Refuses while one of the application's
    /// dialogs is open and asks before work is lost. True when the host window
    /// may be destroyed.
    [[nodiscard]] bool ConfirmExit();
    /// What Windows shows while it holds a sign-out or shutdown back for the
    /// user to decide; null when nothing would be lost.
    [[nodiscard]] const wchar_t* ShutdownBlockReason() const;
    /// Windows is ending the session and will not wait. Closes a running
    /// recording and keeps every finished one for the next start. No window
    /// is destroyed: a dialog may be on the stack.
    void PrepareForSessionEnd();
    /// Offers the recordings an earlier run left unsaved: save, delete or later.
    void OfferRecoveredRecordings();

private:
    CaptureCoordinator() = default;
    ~CaptureCoordinator();

    bool BeginCaptureSession();
    void EnsureOverlay();
    /// A screenshot editor wired to this coordinator, not yet opened.
    [[nodiscard]] std::unique_ptr<editor::ScreenshotEditor> CreateEditor();
    /// Shows the workspace how an edited document's viewport stands.
    void PublishDocumentView(const editor::ImageView& view);
    void OnWorkspaceEditingChanged(CaptureMode mode, const RECT& region);
    void OnWorkspaceRegionChanged(const RECT& region);
    void CompleteWorkspaceScreenshot();
    bool PrepareWorkspaceScreenshot();
    void OnOverlayCancelled();
    void OnEditorClosed(bool exported);
    void OpenEditor(const RECT& region);
    void BeginRecordingConfiguration(const RECT& region);
    void OnRecordingStart(RecordingConfiguration configuration);
    void OnRecordingStartFailed(const std::wstring& message, recording::RecordingStartFailure failure);
    void OnRecordingCancel();
    void OnRecordingPause();
    void OnRecordingResume();
    void OnRecordingStop();
    void OnRecordingStatusTick();
    void BeginRecordingFinalization();
    void FinishRecordingWorkflow(bool finalized);
    void StopRecordingForShutdown();

    enum class ScrollshotAction { Start,
                                  Help,
                                  Edit,
                                  Save,
                                  Discard,
                                  Copy };
    /// What the island's message line reports besides the capture's own condition.
    enum class ScrollshotNotice { None,
                                  WheelBlocked,
                                  DisplayChanged,
                                  Stopping,
                                  Copying,
                                  Saving,
                                  CopyFailed,
                                  SaveFailed };
    /// Shows `notice` on the island at once, before work that blocks this thread.
    void ShowScrollshotNotice(ScrollshotNotice notice);
    /// The visible surface a question about the current session belongs to.
    [[nodiscard]] HWND DialogOwner() const;
    /// "Discard this image?" for the editor and the Scrollshot island.
    [[nodiscard]] bool ConfirmDiscardImage(HWND owner, bool scrollshot);
    /// The message for a region H.264 cannot hold, or empty when it fits.
    [[nodiscard]] std::wstring RecordingRegionError() const;
    void EnsureScrollshotControls();
    void BeginScrollshotConfiguration(const RECT& region);
    void OnScrollshotAction(ScrollshotAction action);
    void OnScrollshotCommand(ui::ScrollshotCommand command);
    void StartScrollshotCapture();
    /// Stops the capture and takes the stitched image. False when the worker
    /// did not reach its barrier in time or nothing was captured.
    bool StopScrollshotCapture();
    void CopyScrollshot();
    void SaveScrollshot();
    void EditScrollshot();
    void DiscardScrollshot();
    void ShowScrollshotHelp();
    void UpdateScrollshotStatus();
    void StopScrollshotForShutdown();

    void EndSession();
    void ScheduleCleanup();

    HWND host_window_{nullptr};
    SessionStateMachine machine_;
    AppSettings settings_{};

    std::unique_ptr<capture::IRegionCaptureProvider> capture_provider_;
    std::unique_ptr<media::ITemporarySessionStore> temp_store_;
    std::unique_ptr<RecordingCompletionController> recording_completion_;
    std::unique_ptr<ui::OverlayController> overlay_;
    std::unique_ptr<ui::RecordingControls> recording_controls_;
    std::unique_ptr<ui::ScrollshotControls> scrollshot_controls_;
    std::unique_ptr<scrollshot::ScrollshotSession> scrollshot_session_;
    std::unique_ptr<editor::ScreenshotEditor> editor_;
    std::unique_ptr<recording::RecordingSession> recording_session_;
    /// Images pinned to the screen. They are not session state: they outlive
    /// every session and close with Easnap (ADR-038).
    std::unique_ptr<ui::PinnedImages> pins_;
    capture::DesktopSnapshot snapshot_;
    RECT recording_region_{};
    /// Monitor bounds and scale the frozen snapshot was taken against.
    std::vector<platform::MonitorInfo> session_monitors_;
    media::RecordingOptions recording_options_{};
    RecordingFinalizer recording_finalizer_;
    /// Trimming and completion of the finished recording; created by Initialize.
    std::unique_ptr<TrimFlow> trim_flow_;

    /// The stitched image once capture has stopped, until an action takes it.
    platform::BgraImage scrollshot_image_;
    RECT scrollshot_region_{};
    uint64_t scrollshot_generation_{0};
    int scrollshot_height_{0};
    ScrollshotNotice scrollshot_notice_{ScrollshotNotice::None};
    /// An action is being carried out; a second one must not start inside it.
    bool scrollshot_busy_{false};

    bool overlay_retired_{false};
    bool recording_controls_retired_{false};
    bool scrollshot_controls_retired_{false};
    bool editor_retired_{false};
    bool initialized_{false};
};

}  // namespace easnap::app
