// Easnap - the trimming and completion of one video (ADR-039, ADR-040).
//
// The coordinator owns one TrimFlow. The flow owns the trim window, the export
// controller, the source and the latest temporary result; it changes the
// session state only through the coordinator's state machine. The source is a
// finished recording (Owned) or a video file the user opened (External), which
// the flow never changes, moves or deletes.
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

    /// Opens the trim window for `source` in RecordingTrimming. For a
    /// recording, `options` are the recording's and `session` is its temporary
    /// session, deleted or kept when the flow ends. For an external video,
    /// `options` carry the export settings, `session` must be empty: the flow
    /// takes a work session of its own from the store for its temporary
    /// results, never marks it recoverable and removes it when it ends.
    void Begin(const TrimSource& source, const media::RecordingOptions& options,
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

    /// What the flow holds; Owned when no flow is open.
    [[nodiscard]] TrimSourceKind source_kind() const noexcept { return source_.kind; }
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
    [[nodiscard]] bool external() const noexcept { return source_.kind == TrimSourceKind::External; }
    /// `owned` for a recording, `external` for an opened video: the same
    /// message, without "the recording was kept" for a file nobody could lose.
    [[nodiscard]] UINT TextFor(UINT owned, UINT external_id) const noexcept {
        return external() ? external_id : owned;
    }
    bool RequestTransition(SessionState next) { return machine_.RequestTransition(next); }
    void EndSession() { services_.end_session(); }
    void ScheduleCleanup() { services_.schedule_cleanup(); }
    bool PersistSettings() { return services_.persist_settings(); }

    void OnTrimSaveRequested(media::MediaTime start, media::MediaTime end);
    void OnTrimCopyRequested(media::MediaTime start, media::MediaTime end);
    void BeginTrimAction(PendingTrimAction action, media::MediaTime start,
                         media::MediaTime end, media::RecordingFormat format,
                         std::wstring save_destination = {},
                         bool replace_existing = false);
    void StartTrimExport(media::MediaTime start, media::MediaTime end,
                         media::RecordingFormat format,
                         std::wstring destination_path = {},
                         bool destination_is_clipboard = false,
                         bool destination_is_save = false,
                         bool replace_existing = false);
    /// The export controller for `format`; a recording keeps the one it was
    /// opened with, an external video changes it between actions.
    [[nodiscard]] bool EnsureExportController(media::RecordingFormat format);
    /// The recording options with the format of this export.
    [[nodiscard]] media::RecordingOptions ExportOptions(media::RecordingFormat format) const;
    /// Where a temporary result goes: beside a recording in its own session,
    /// into the work session for an external video, never beside its file.
    [[nodiscard]] std::wstring TemporaryResultDirectory() const;
    /// A full-range action may hand on the source itself.
    [[nodiscard]] bool SourceReusable() const;
    /// Whether `path`, a file the flow made, may be moved on instead of
    /// copied. The external video itself never is; nor is anything outside the
    /// flow's work session.
    [[nodiscard]] bool MayMoveArtifact(const std::wstring& path) const;
    /// For an external video: the file is still the one the probe checked.
    /// Otherwise the window says so and no action starts.
    [[nodiscard]] bool ExternalSourceUnchanged();
    /// Closing an external video: no question unless an export runs.
    void CloseExternalVideo();
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
    /// The format of `trim_export_`'s exporter.
    media::RecordingFormat export_controller_format_{media::RecordingFormat::Mp4};
    media::RecordingOptions recording_options_{};
    /// A recording's temporary session, or an external video's work session.
    std::optional<media::TemporarySession> session_;
    TrimSource source_;
    TrimArtifact trim_artifact_;
    /// The format of the export that runs or ran last.
    media::RecordingFormat trim_export_format_{media::RecordingFormat::Mp4};
    media::RecordingFormat pending_format_{media::RecordingFormat::Mp4};
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
