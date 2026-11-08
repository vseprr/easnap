// Easnap - the trimming and completion of one finished recording (ADR-039).
#include "app/trim_flow.h"

#include <new>
#include <utility>

#include "app/recording_workflow.h"
#include "platform/platform_utils.h"
#include "strings/strings.h"
#include "ui/native_controls.h"
#include "ui/trim_window.h"

namespace easnap::app {
namespace {

const wchar_t* ExportFailureStageName(
    media::TrimExportFailureStage stage) noexcept {
    switch (stage) {
        case media::TrimExportFailureStage::SourceOpen:
            return L"source-open";
        case media::TrimExportFailureStage::SourceValidation:
            return L"source-validation";
        case media::TrimExportFailureStage::MediaTypeNegotiation:
            return L"media-type-negotiation";
        case media::TrimExportFailureStage::VideoDecode:
            return L"video-decode";
        case media::TrimExportFailureStage::DecodedFrameAccess:
            return L"decoded-frame-access";
        case media::TrimExportFailureStage::SourceReset:
            return L"source-reset";
        case media::TrimExportFailureStage::PaletteQuantization:
            return L"palette-quantization";
        case media::TrimExportFailureStage::GifEncoding:
            return L"gif-encoding";
        case media::TrimExportFailureStage::StagingWrite:
            return L"staging-write";
        case media::TrimExportFailureStage::Publication:
            return L"publication";
        case media::TrimExportFailureStage::Unknown:
            return L"unknown";
        default:
            return L"none";
    }
}

// One whole sentence per stage: where the stage stands in the sentence differs
// between languages.
UINT GifExportFailureMessageId(
    media::TrimExportFailureStage stage) noexcept {
    switch (stage) {
        case media::TrimExportFailureStage::SourceOpen:
        case media::TrimExportFailureStage::SourceValidation:
            return IDS_SESSION_GIF_FAILED_READING;
        case media::TrimExportFailureStage::MediaTypeNegotiation:
            return IDS_SESSION_GIF_FAILED_PREPARING;
        case media::TrimExportFailureStage::VideoDecode:
        case media::TrimExportFailureStage::DecodedFrameAccess:
            return IDS_SESSION_GIF_FAILED_DECODING;
        case media::TrimExportFailureStage::SourceReset:
            return IDS_SESSION_GIF_FAILED_FINAL_PASS;
        case media::TrimExportFailureStage::PaletteQuantization:
            return IDS_SESSION_GIF_FAILED_PALETTE;
        case media::TrimExportFailureStage::GifEncoding:
            return IDS_SESSION_GIF_FAILED_ENCODING;
        case media::TrimExportFailureStage::StagingWrite:
            return IDS_SESSION_GIF_FAILED_WRITING;
        case media::TrimExportFailureStage::Publication:
            return IDS_SESSION_GIF_FAILED_PUBLISHING;
        default:
            return IDS_SESSION_GIF_FAILED_CONVERTING;
    }
}

void LogTrimExportFailure(const media::TrimExportFailure& failure) {
    if (failure.has_hresult) {
        platform::LogMessage(
            L"[Trim] Export failed: stage=%s HRESULT=0x%08X detail=%s",
            ExportFailureStageName(failure.stage),
            static_cast<unsigned int>(failure.hresult), failure.detail.c_str());
    } else {
        platform::LogMessage(L"[Trim] Export failed: stage=%s detail=%s",
                             ExportFailureStageName(failure.stage),
                             failure.detail.c_str());
    }
}

std::wstring GifExportFailureMessage(
    media::TrimExportFailureStage stage) {
    return strings::Get(GifExportFailureMessageId(stage));
}

}  // namespace

SaveRecordingDialogText RecordingSaveDialogText() {
    return {strings::Get(IDS_SHELL_SAVE_RECORDING_TITLE), strings::Get(IDS_SHELL_FILE_TYPE_MP4),
            strings::Get(IDS_SHELL_FILE_TYPE_GIF)};
}

TrimFlow::TrimFlow(Services services)
    : services_(std::move(services)),
      machine_(*services_.machine),
      settings_(*services_.settings),
      host_window_(services_.host_window),
      recording_completion_(services_.completion),
      temp_store_(services_.store) {}

TrimFlow::~TrimFlow() { Destroy(); }

void TrimFlow::CancelExportAndWait() {
    if (trim_export_) {
        trim_export_->CancelAndWait();
    }
}

void TrimFlow::RetireWindow() {
    if (trim_window_) {
        trim_window_->Hide();
        trim_window_retired_ = true;
    }
}

void TrimFlow::DestroyRetired() {
    if (trim_window_retired_) {
        trim_window_retired_ = false;
        if (trim_window_) {
            trim_window_->Destroy();
            trim_window_.reset();
        }
    }
}

void TrimFlow::Destroy() {
    CancelExportAndWait();
    if (trim_window_) {
        trim_window_->Destroy();
        trim_window_.reset();
    }
    trim_export_.reset();
    session_.reset();
    trim_source_path_.clear();
    trim_artifact_.Clear();
}

bool TrimFlow::window_visible() const { return trim_window_ && trim_window_->visible(); }

HWND TrimFlow::visible_window() const { return window_visible() ? trim_window_->hwnd() : nullptr; }

void TrimFlow::ActivateWindow() {
    if (trim_window_) {
        trim_window_->Activate();
    }
}

void TrimFlow::RefreshWindowLayout() {
    if (trim_window_) {
        trim_window_->RefreshLayout();
    }
}

bool TrimFlow::ProcessDialogMessage(MSG& message) const {
    return window_visible() && trim_window_->ProcessDialogMessage(message);
}

void TrimFlow::Begin(const std::wstring& source_path, const media::RecordingOptions& options, const RECT& region,
                     std::optional<media::TemporarySession> session) {
    if (state() != SessionState::RecordingTrimming || source_path.empty()) {
        return;
    }
    // Before anything that can throw: a failed start still keeps the recording.
    session_ = std::move(session);

    try {
        if (trim_window_) {
            trim_window_->Destroy();
            trim_window_.reset();
        }
        trim_window_retired_ = false;
        trim_source_path_ = source_path;
        recording_options_ = options;
        trim_artifact_.Clear();
        pending_trim_action_ = PendingTrimAction::None;
        pending_save_destination_.clear();
        trim_close_pending_ = false;

        ui::TrimWindow::Callbacks window_callbacks;
        window_callbacks.on_save =
            [this](media::MediaTime start, media::MediaTime end) {
                OnTrimSaveRequested(start, end);
            };
        window_callbacks.on_copy =
            [this](media::MediaTime start, media::MediaTime end) {
                OnTrimCopyRequested(start, end);
            };
        window_callbacks.on_cancel_export = [this] { OnTrimExportCancel(); };
        window_callbacks.on_discard_requested = [this] { OnTrimWindowClosed(); };
        window_callbacks.on_status_tick = [this] { OnTrimStatusTick(); };
        trim_window_ =
            std::make_unique<ui::TrimWindow>(std::move(window_callbacks));

        auto exporter = recording_options_.format == media::RecordingFormat::Gif
                            ? media::CreateGifExporter()
                            : media::CreateMp4TrimExporter();
        if (!exporter) {
            throw std::bad_alloc{};
        }
        trim_export_ = std::make_unique<TrimExportController>(
            std::move(exporter), [this] {
                if (host_window_ != nullptr &&
                    ::PostMessageW(
                        host_window_, kMsgTrimExportCompleted,
                        static_cast<WPARAM>(trim_export_generation_.load(
                            std::memory_order_acquire)),
                        0) == FALSE) {
                    platform::LogMessage(
                        L"[Trim] Could not post export completion notification");
                }
            });
    } catch (...) {
        platform::LogMessage(L"The trim view could not be created");
        trim_window_.reset();
        trim_export_.reset();
    }

    if (!trim_window_ || !trim_export_ ||
        !trim_window_->Open(trim_source_path_, region)) {
        platform::LogMessage(L"The trim view could not be opened");
        // The next start offers the recording only if it carries the marker;
        // marking is repeated here because its first attempt may have failed.
        const bool offered_later = session_.has_value() && temp_store_ && temp_store_->MarkRecoverable(*session_);
        if (offered_later) {
            ui::ShowErrorMessage(host_window_, strings::Get(IDS_RECORDING_KEPT_FOR_RECOVERY));
        } else {
            ui::ShowErrorMessage(host_window_,
                                 strings::Format(IDS_RECORDING_KEPT_UNTIL_NEXT_START, {trim_source_path_}).c_str());
        }
        FinishTrimWorkflow(false);
        return;
    }
    ScheduleCleanup();
}

void TrimFlow::OnTrimSaveRequested(media::MediaTime start,
                                   media::MediaTime end) {
    const auto destination = ShowSaveRecordingDialog(
        trim_window_ != nullptr ? trim_window_->hwnd() : host_window_,
        recording_options_.format,
        platform::TimestampedFileName(
            L"Easnap", RecordingFileExtension(recording_options_.format)),
        settings_.VideoStartDirectory(), RecordingSaveDialogText());
    if (!destination.has_value()) {
        return;
    }
    const media::MediaTime duration =
        trim_window_ != nullptr ? trim_window_->range().source_duration()
                                : media::MediaTime::zero();
    const bool source_available =
        !ExistingFileDataPath(trim_source_path_).empty();
    const bool artifact_available =
        trim_artifact_.Matches(start, end) &&
        !ExistingFileDataPath(trim_artifact_.path).empty();
    if (PlanTrimMediaPreparation(
            TrimTerminalAction::Save, start, end, duration,
            source_available, artifact_available, recording_options_.format) ==
        TrimMediaPreparation::ReuseFinalizedSource) {
        trim_artifact_.path = trim_source_path_;
        trim_artifact_.start = start;
        trim_artifact_.end = end;
    }
    const DWORD attributes = ::GetFileAttributesW(destination->c_str());
    BeginTrimAction(PendingTrimAction::Save, start, end, *destination,
                    attributes != INVALID_FILE_ATTRIBUTES);
}

void TrimFlow::OnTrimCopyRequested(media::MediaTime start,
                                   media::MediaTime end) {
    const media::MediaTime duration =
        trim_window_ != nullptr ? trim_window_->range().source_duration()
                                : media::MediaTime::zero();
    const bool source_available =
        !ExistingFileDataPath(trim_source_path_).empty();
    const bool artifact_available =
        trim_artifact_.Matches(start, end) &&
        !ExistingFileDataPath(trim_artifact_.path).empty();
    if (PlanTrimMediaPreparation(
            TrimTerminalAction::Copy, start, end, duration,
            source_available, artifact_available, recording_options_.format) ==
        TrimMediaPreparation::ReuseFinalizedSource) {
        trim_artifact_.path = trim_source_path_;
        trim_artifact_.start = start;
        trim_artifact_.end = end;
    }
    BeginTrimAction(PendingTrimAction::Copy, start, end);
}

void TrimFlow::BeginTrimAction(
    PendingTrimAction action, media::MediaTime start, media::MediaTime end,
    std::wstring save_destination, bool replace_existing) {
    if (state() != SessionState::RecordingTrimming || action == PendingTrimAction::None ||
        start < media::MediaTime::zero() || end <= start) {
        return;
    }
    pending_trim_action_ = action;
    pending_trim_start_ = start;
    pending_trim_end_ = end;
    pending_save_destination_ = std::move(save_destination);
    pending_save_replace_ = replace_existing;
    pending_trim_action_started_ms_ = ::GetTickCount64();

    if (trim_artifact_.Matches(start, end) &&
        !ExistingFileDataPath(trim_artifact_.path).empty()) {
        if (action == PendingTrimAction::Copy &&
            recording_completion_ != nullptr &&
            !recording_completion_->IsPreparedClipboardArtifact(
                trim_artifact_.path)) {
            const bool allow_move = trim_artifact_.path != trim_source_path_;
            const auto staged = recording_completion_->StageClipboardArtifact(
                trim_artifact_.path, recording_options_.format, allow_move);
            if (!staged.has_value()) {
                pending_trim_action_ = PendingTrimAction::None;
                if (trim_window_) {
                    trim_window_->SetStatus(
                        strings::Get(IDS_SESSION_TRIM_COPY_PREPARE_FAILED),
                        true);
                }
                return;
            }
            trim_artifact_.path = *staged;
        }
        CompletePendingTrimAction();
        return;
    }
    if (action == PendingTrimAction::Copy && recording_completion_ != nullptr) {
        const auto destination =
            recording_completion_->PrepareClipboardDestination(
                recording_options_.format);
        if (!destination.has_value()) {
            pending_trim_action_ = PendingTrimAction::None;
            if (trim_window_) {
                trim_window_->SetStatus(
                    strings::Get(IDS_SESSION_TRIM_COPY_PREPARE_FAILED),
                    true);
            }
            return;
        }
        StartTrimExport(start, end, *destination, true);
        return;
    }
    if (action == PendingTrimAction::Save &&
        !pending_save_destination_.empty()) {
        if (recording_completion_ == nullptr ||
            !recording_completion_->IsAllowedSaveDestination(
                pending_save_destination_, recording_options_.format,
                pending_save_replace_)) {
            pending_trim_action_ = PendingTrimAction::None;
            if (trim_window_) {
                trim_window_->SetStatus(
                    strings::Get(IDS_SESSION_TRIM_SAVE_DESTINATION_UNAVAILABLE),
                    true);
            }
            return;
        }
        StartTrimExport(start, end, pending_save_destination_, false, true,
                        pending_save_replace_);
        return;
    }
    StartTrimExport(start, end);
}

void TrimFlow::StartTrimExport(media::MediaTime start,
                               media::MediaTime end,
                               std::wstring destination_path,
                               bool destination_is_clipboard,
                               bool destination_is_save,
                               bool replace_existing) {
    if (state() != SessionState::RecordingTrimming || !trim_export_ ||
        trim_source_path_.empty() || start < media::MediaTime::zero() ||
        end <= start) {
        return;
    }

    const std::wstring directory = platform::DirectoryOf(trim_source_path_);
    if (destination_path.empty() && directory.empty()) {
        if (trim_window_) {
            trim_window_->SetStatus(
                strings::Get(IDS_SESSION_TRIM_TEMP_LOCATION_UNAVAILABLE),
                true);
        }
        return;
    }

    TrimExportRequest request;
    request.source_path = trim_source_path_;
    const uint64_t generation =
        trim_export_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    request.destination_path = destination_path.empty()
                                   ? directory + L"\\trimmed-" +
                                         std::to_wstring(generation) + L"." +
                                         RecordingFileExtension(
                                             recording_options_.format)
                                   : std::move(destination_path);
    request.start = start;
    request.end = end;
    request.options = recording_options_;
    request.replace_existing = replace_existing;
    if (!trim_export_->Start(std::move(request))) {
        if (trim_window_) {
            trim_window_->SetStatus(
                strings::Get(IDS_SESSION_TRIM_EXPORT_START_FAILED),
                true);
        }
        pending_trim_action_ = PendingTrimAction::None;
        pending_save_destination_.clear();
        if (destination_is_clipboard && recording_completion_ != nullptr) {
            recording_completion_->AbandonPreparedClipboardArtifact();
        }
        return;
    }
    if (!RequestTransition(SessionState::Exporting)) {
        trim_export_->CancelAndWait();
        if (trim_window_) {
            trim_window_->SetStatus(
                strings::Get(IDS_SESSION_TRIM_EXPORT_STATE_CHANGED),
                true);
        }
        pending_trim_action_ = PendingTrimAction::None;
        pending_save_destination_.clear();
        if (destination_is_clipboard && recording_completion_ != nullptr) {
            recording_completion_->AbandonPreparedClipboardArtifact();
        }
        return;
    }
    trim_export_start_ = start;
    trim_export_end_ = end;
    trim_export_to_clipboard_ = destination_is_clipboard;
    trim_export_to_save_ = destination_is_save;
    trim_close_pending_ = false;
    if (trim_window_) {
        trim_window_->SetExportProgress(0.0f);
        trim_window_->SetExporting(true);
    }
}

void TrimFlow::OnTrimExportCancel() {
    if (state() != SessionState::Exporting || !trim_export_) {
        return;
    }
    trim_export_->Cancel();
    if (trim_window_) {
        trim_window_->SetExporting(true, true);
    }
}

void TrimFlow::OnTrimWindowClosed() {
    if (state() != SessionState::RecordingTrimming &&
        state() != SessionState::Exporting) {
        return;
    }
    // Without a preview nothing can be trimmed and Save is off, but the
    // recording itself is complete. Closing then offers to save it as it is.
    if (state() == SessionState::RecordingTrimming && trim_window_ != nullptr && trim_window_->preview_failed()) {
        SaveUntrimmedRecording();
        return;
    }
    const bool discard = ui::ConfirmDestructive(
        trim_window_ != nullptr ? trim_window_->hwnd() : host_window_,
        strings::Get(IDS_DISCARD_RECORDING_TEXT), strings::Get(IDS_DISCARD_RECORDING_TITLE),
        strings::Get(IDS_DISCARD_CONFIRM), strings::Get(IDS_DISCARD_RECORDING_KEEP));
    if (state() != SessionState::RecordingTrimming && state() != SessionState::Exporting) {
        return;  // The export finished and closed the session while the question was open.
    }
    const TrimCloseAction action =
        PlanTrimClose(discard, state() == SessionState::Exporting);
    if (action == TrimCloseAction::CancelExportThenDiscard) {
        trim_close_pending_ = true;
        OnTrimExportCancel();
    } else if (action == TrimCloseAction::DiscardNow) {
        FinishTrimWorkflow(true);
    }
}

void TrimFlow::SaveUntrimmedRecording() {
    enum : int { kSave = 100,
                 kDelete = 101,
                 kKeep = 102 };
    const HWND owner = trim_window_ != nullptr ? trim_window_->hwnd() : host_window_;
    const int choice = ui::ShowChoice(
        owner, strings::Get(IDS_TRIM_NO_PREVIEW_TEXT), strings::Get(IDS_RECOVERY_TITLE),
        {{kSave, strings::Get(IDS_RECOVERY_SAVE)},
         {kDelete, strings::Get(IDS_RECOVERY_DELETE), true},
         {kKeep, strings::Get(IDS_DISCARD_RECORDING_KEEP)}},
        kSave, kKeep);
    if (state() != SessionState::RecordingTrimming) {
        return;
    }
    if (choice == kDelete) {
        FinishTrimWorkflow(true);
        return;
    }
    if (choice != kSave) {
        return;  // The window and the recording stay.
    }
    // The working recording is an MP4 whatever format was chosen: a GIF is
    // made from it by the export that cannot run here.
    const auto destination = ShowSaveRecordingDialog(
        trim_window_ != nullptr ? trim_window_->hwnd() : host_window_, media::RecordingFormat::Mp4,
        platform::TimestampedFileName(L"Easnap", RecordingFileExtension(media::RecordingFormat::Mp4)),
        settings_.VideoStartDirectory(), RecordingSaveDialogText());
    if (state() != SessionState::RecordingTrimming || !destination.has_value()) {
        return;
    }
    if (::CopyFileW(trim_source_path_.c_str(), destination->c_str(), FALSE) == FALSE) {
        platform::LogMessage(L"Saving a recording without a preview failed (error %lu)", ::GetLastError());
        // The recording stays open here; a normal exit would delete it, so the
        // message must not promise an offer at the next start.
        ui::ShowErrorMessage(trim_window_ != nullptr ? trim_window_->hwnd() : host_window_,
                             strings::Get(IDS_TRIM_SAVE_UNTRIMMED_FAILED));
        return;
    }
    settings_.last_video_directory = platform::DirectoryOf(*destination);
    PersistSettings();
    platform::LogMessage(L"A recording without a preview was saved untrimmed");
    FinishTrimWorkflow(true);
}

void TrimFlow::OnTrimStatusTick() {
    if (state() != SessionState::Exporting || !trim_export_) {
        return;
    }
    const TrimExportSnapshot snapshot = trim_export_->Snapshot();
    if (trim_window_) {
        trim_window_->SetExportProgress(snapshot.progress);
    }
    if (snapshot.status != TrimExportStatus::Running) {
        OnExportCompleted(
            trim_export_generation_.load(std::memory_order_acquire));
    }
}

void TrimFlow::OnExportCompleted(uint64_t export_generation) {
    if (export_generation !=
            trim_export_generation_.load(std::memory_order_acquire) ||
        state() != SessionState::Exporting || !trim_export_) {
        return;
    }
    trim_export_->Wait();
    TrimExportSnapshot snapshot = trim_export_->Snapshot();
    if (snapshot.status == TrimExportStatus::Succeeded) {
        snapshot.result_path = ExistingFileDataPath(snapshot.result_path);
        if (snapshot.result_path.empty()) {
            snapshot.status = TrimExportStatus::Failed;
            snapshot.failure.stage = media::TrimExportFailureStage::Publication;
            snapshot.failure.detail =
                L"The exporter reported success without a readable output file";
        }
    }

    if (trim_close_pending_) {
        FinishTrimWorkflow(true);
        return;
    }

    if (!RequestTransition(SessionState::RecordingTrimming)) {
        return;
    }
    if (!trim_window_) {
        return;
    }
    switch (snapshot.status) {
        case TrimExportStatus::Succeeded:
            if (trim_export_to_save_) {
                trim_export_to_save_ = false;
                pending_trim_action_ = PendingTrimAction::None;
                pending_save_destination_.clear();
                settings_.last_video_directory =
                    platform::DirectoryOf(snapshot.result_path);
                platform::LogMessage(
                    L"[Completion] Save action completed in %llu ms",
                    ::GetTickCount64() - pending_trim_action_started_ms_);
                FinishTrimWorkflow(true);
                return;
            }
            trim_artifact_.path = snapshot.result_path;
            trim_artifact_.start = trim_export_start_;
            trim_artifact_.end = trim_export_end_;
            trim_window_->SetExportProgress(1.0f);
            CompletePendingTrimAction();
            if (trim_window_ && trim_window_->visible() && trim_window_->exporting()) {
                trim_window_->SetExportResult(
                    strings::Get(IDS_SESSION_TRIM_ACTION_FAILED), true);
            }
            break;
        case TrimExportStatus::Cancelled:
            if (trim_export_to_clipboard_ && recording_completion_ != nullptr) {
                recording_completion_->AbandonPreparedClipboardArtifact();
            }
            trim_export_to_clipboard_ = false;
            trim_export_to_save_ = false;
            pending_trim_action_ = PendingTrimAction::None;
            pending_save_destination_.clear();
            trim_window_->SetExportResult(
                strings::Get(IDS_SESSION_TRIM_EXPORT_CANCELLED));
            break;
        case TrimExportStatus::Failed:
            LogTrimExportFailure(snapshot.failure);
            if (trim_export_to_clipboard_ && recording_completion_ != nullptr) {
                recording_completion_->AbandonPreparedClipboardArtifact();
            }
            trim_export_to_clipboard_ = false;
            trim_export_to_save_ = false;
            pending_trim_action_ = PendingTrimAction::None;
            pending_save_destination_.clear();
            if (recording_options_.format == media::RecordingFormat::Gif) {
                trim_window_->SetExportResult(
                    GifExportFailureMessage(snapshot.failure.stage), true);
            } else {
                trim_window_->SetExportResult(
                    strings::Get(IDS_SESSION_TRIM_EXPORT_FAILED),
                    true);
            }
            break;
        default:
            if (trim_export_to_clipboard_ && recording_completion_ != nullptr) {
                recording_completion_->AbandonPreparedClipboardArtifact();
            }
            trim_export_to_clipboard_ = false;
            trim_export_to_save_ = false;
            pending_trim_action_ = PendingTrimAction::None;
            pending_save_destination_.clear();
            trim_window_->SetExportResult(
                strings::Get(IDS_SESSION_TRIM_EXPORT_ENDED),
                true);
            break;
    }
}

void TrimFlow::CompletePendingTrimAction() {
    if (state() != SessionState::RecordingTrimming ||
        pending_trim_action_ == PendingTrimAction::None ||
        !trim_artifact_.Matches(pending_trim_start_, pending_trim_end_) ||
        ExistingFileDataPath(trim_artifact_.path).empty() ||
        recording_completion_ == nullptr) {
        return;
    }
    const PendingTrimAction action = pending_trim_action_;
    pending_trim_action_ = PendingTrimAction::None;
    if (action == PendingTrimAction::Save) {
        const std::wstring destination = pending_save_destination_;
        pending_save_destination_.clear();
        if (!recording_completion_->Save(trim_artifact_.path, destination,
                                         recording_options_.format,
                                         pending_save_replace_)) {
            if (trim_window_) {
                trim_window_->SetExportResult(
                    strings::Get(IDS_SESSION_TRIM_SAVE_FAILED),
                    true);
            }
            return;
        }
        settings_.last_video_directory = platform::DirectoryOf(destination);
        platform::LogMessage(
            L"[Completion] Save action completed in %llu ms",
            ::GetTickCount64() - pending_trim_action_started_ms_);
        FinishTrimWorkflow(true);
        return;
    }

    pending_save_destination_.clear();
    if (!recording_completion_->IsPreparedClipboardArtifact(
            trim_artifact_.path)) {
        const bool allow_move = trim_artifact_.path != trim_source_path_;
        const auto staged = recording_completion_->StageClipboardArtifact(
            trim_artifact_.path, recording_options_.format, allow_move);
        if (staged.has_value()) {
            trim_artifact_.path = *staged;
        }
    }
    if (!recording_completion_->PublishClipboardArtifact(host_window_,
                                                         trim_artifact_.path,
                                                         recording_options_.format)) {
        if (trim_window_) {
            trim_window_->SetExportResult(
                strings::Get(IDS_SESSION_TRIM_COPY_FAILED),
                true);
        }
        return;
    }
    platform::LogMessage(
        L"[Completion] Copy action completed in %llu ms",
        ::GetTickCount64() - pending_trim_action_started_ms_);
    FinishTrimWorkflow(true);
}

void TrimFlow::FinishTrimWorkflow(bool delete_active_session) {
    if (state() == SessionState::Exporting && trim_export_) {
        trim_export_->CancelAndWait();
    }
    if (state() == SessionState::RecordingTrimming ||
        state() == SessionState::Exporting) {
        RequestTransition(SessionState::Idle);
    }
    std::optional<media::TemporarySession> active_session;
    active_session.swap(session_);
    if (trim_window_) {
        trim_window_->Hide();
        trim_window_->ReleaseMedia();
        trim_window_retired_ = true;
    }
    trim_export_.reset();
    services_.release_recording();
    if (recording_completion_ != nullptr) {
        recording_completion_->AbandonPreparedClipboardArtifact();
    }
    if (delete_active_session && active_session.has_value() &&
        recording_completion_ != nullptr &&
        !recording_completion_->Discard(*active_session)) {
        platform::LogMessage(
            L"Temporary recording cleanup was deferred because a file remained in use");
    }
    if (!delete_active_session && active_session.has_value() && temp_store_) {
        // Kept for the next start: no longer owned, so exit does not delete it.
        static_cast<void>(temp_store_->ReleaseSession(*active_session));
    }
    trim_source_path_.clear();
    trim_artifact_.Clear();
    pending_trim_action_ = PendingTrimAction::None;
    pending_save_destination_.clear();
    trim_export_to_clipboard_ = false;
    trim_export_to_save_ = false;
    pending_save_replace_ = false;
    pending_trim_action_started_ms_ = 0;
    trim_close_pending_ = false;
    EndSession();
}

}  // namespace easnap::app
