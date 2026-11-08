// Easnap - the trimming and completion of one finished recording (ADR-039).
//
// The coordinator owns one TrimFlow. The flow owns the trim window, the export
// controller, the finalized source path and the latest temporary result; it
// changes the session state only through the coordinator's state machine.
// Every method must be called on the UI thread, except that the export
// controller reports completion from its worker thread by posting
// kMsgTrimExportCompleted.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app/app_settings.h"
#include "app/recording_completion.h"
#include "app/session_transitions.h"
#include "app/trim_workflow.h"
#include "media/media_interfaces.h"
#include "media/temp_session.h"

namespace easnap::ui {
class TrimWindow;
}  // namespace easnap::ui

namespace easnap::app {

/// Posted when the trim export worker has stopped and its result can be
/// reconciled with the session on the UI thread. WPARAM carries the export's
/// generation.
inline constexpr UINT kMsgTrimExportCompleted = WM_APP + 13;

/// The recording Save dialog's text in the active language.
[[nodiscard]] SaveRecordingDialogText RecordingSaveDialogText();

class TrimFlow final {
public:
    /// The coordinator's own operations the flow calls. `machine` and
    /// `settings` must outlive the flow; `completion` and `store` may be null.
    struct Services {
        SessionStateMachine* machine{nullptr};
        AppSettings* settings{nullptr};
        HWND host_window{nullptr};
        RecordingCompletionController* completion{nullptr};
        media::ITemporarySessionStore* store{nullptr};
        /// Ends the capture session; the flow's window is retired through it.
        std::function<void()> end_session;
        /// Posts the deferred cleanup that destroys retired windows.
        std::function<void()> schedule_cleanup;
        std::function<bool()> persist_settings;
        /// Releases the finished recording session the trim was opened for.
        std::function<void()> release_recording;
    };

    explicit TrimFlow(Services services);
    ~TrimFlow();

    TrimFlow(const TrimFlow&) = delete;
    TrimFlow& operator=(const TrimFlow&) = delete;

    /// Opens the trim window for `source_path` in RecordingTrimming. `session`
    /// is the recording's temporary session, deleted or kept when the flow ends.
    void Begin(const std::wstring& source_path, const media::RecordingOptions& options, const RECT& region,
               std::optional<media::TemporarySession> session);
    /// Handles kMsgTrimExportCompleted.
    void OnExportCompleted(uint64_t export_generation);
    /// Stops a running export and waits for its worker.
    void CancelExportAndWait();
    /// Hides the window and marks it for DestroyRetired. Safe to call twice.
    void RetireWindow();
    /// Destroys a window retired earlier; called from the deferred cleanup.
    void DestroyRetired();
    /// Cancels and waits for an export, then destroys the window and the export
    /// controller at once. For the end of the application.
    void Destroy();

    [[nodiscard]] bool window_visible() const;
    /// The trim window while it is visible, otherwise null.
    [[nodiscard]] HWND visible_window() const;
    void ActivateWindow();
    void RefreshWindowLayout();
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const;

private:
    enum class PendingTrimAction { None,
                                   Save,
                                   Copy };

    [[nodiscard]] SessionState state() const noexcept { return machine_.state(); }
    bool RequestTransition(SessionState next) { return machine_.RequestTransition(next); }
    void EndSession() { services_.end_session(); }
    void ScheduleCleanup() { services_.schedule_cleanup(); }
    bool PersistSettings() { return services_.persist_settings(); }

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
    void SaveUntrimmedRecording();
    void OnTrimStatusTick();
    void FinishTrimWorkflow(bool delete_active_session);

    Services services_;
    SessionStateMachine& machine_;
    AppSettings& settings_;
    /// Copied for the export worker, which posts its completion to it.
    const HWND host_window_;
    RecordingCompletionController* const recording_completion_;
    media::ITemporarySessionStore* const temp_store_;

    std::unique_ptr<ui::TrimWindow> trim_window_;
    std::unique_ptr<TrimExportController> trim_export_;
    media::RecordingOptions recording_options_{};
    std::optional<media::TemporarySession> session_;
    std::wstring trim_source_path_;
    TrimArtifact trim_artifact_;
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
    bool trim_window_retired_{false};
};

}  // namespace easnap::app
