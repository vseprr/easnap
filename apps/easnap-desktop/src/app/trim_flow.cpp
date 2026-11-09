// Easnap - the trimming and completion of one video (ADR-039, ADR-040).
#include "app/trim_flow.h"

#include <new>
#include <utility>

#include "app/external_video.h"
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
// between languages. An external video has its own sentences, which do not
// speak of a recording that was kept.
UINT GifExportFailureMessageId(
    media::TrimExportFailureStage stage, bool external) noexcept {
    switch (stage) {
        case media::TrimExportFailureStage::SourceOpen:
        case media::TrimExportFailureStage::SourceValidation:
            return external ? IDS_EXTERNAL_GIF_FAILED_READING : IDS_SESSION_GIF_FAILED_READING;
        case media::TrimExportFailureStage::MediaTypeNegotiation:
            return external ? IDS_EXTERNAL_GIF_FAILED_PREPARING : IDS_SESSION_GIF_FAILED_PREPARING;
        case media::TrimExportFailureStage::VideoDecode:
        case media::TrimExportFailureStage::DecodedFrameAccess:
            return external ? IDS_EXTERNAL_GIF_FAILED_DECODING : IDS_SESSION_GIF_FAILED_DECODING;
        case media::TrimExportFailureStage::SourceReset:
            return external ? IDS_EXTERNAL_GIF_FAILED_FINAL_PASS : IDS_SESSION_GIF_FAILED_FINAL_PASS;
        case media::TrimExportFailureStage::PaletteQuantization:
            return external ? IDS_EXTERNAL_GIF_FAILED_PALETTE : IDS_SESSION_GIF_FAILED_PALETTE;
        case media::TrimExportFailureStage::GifEncoding:
            return external ? IDS_EXTERNAL_GIF_FAILED_ENCODING : IDS_SESSION_GIF_FAILED_ENCODING;
        case media::TrimExportFailureStage::StagingWrite:
            return external ? IDS_EXTERNAL_GIF_FAILED_WRITING : IDS_SESSION_GIF_FAILED_WRITING;
        case media::TrimExportFailureStage::Publication:
            return external ? IDS_EXTERNAL_GIF_FAILED_PUBLISHING : IDS_SESSION_GIF_FAILED_PUBLISHING;
        default:
            return external ? IDS_EXTERNAL_GIF_FAILED_CONVERTING : IDS_SESSION_GIF_FAILED_CONVERTING;
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

/// True when `path` lies inside `directory`, compared as full paths without
/// regard to case.
bool PathInsideDirectory(const std::wstring& path, const std::wstring& directory) {
    const auto full = [](const std::wstring& value) {
        std::wstring result(MAX_PATH, L'\0');
        DWORD length = ::GetFullPathNameW(value.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
        if (length >= result.size()) {
            result.resize(length);
            length = ::GetFullPathNameW(value.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
        }
        result.resize(length < result.size() ? length : 0);
        return result;
    };
    std::wstring inner = full(path);
    std::wstring outer = full(directory);
    while (!outer.empty() && (outer.back() == L'\\' || outer.back() == L'/')) {
        outer.pop_back();
    }
    if (inner.empty() || outer.empty() || inner.size() <= outer.size() + 1 ||
        (inner[outer.size()] != L'\\' && inner[outer.size()] != L'/')) {
        return false;
    }
    return ::CompareStringOrdinal(inner.c_str(), static_cast<int>(outer.size()), outer.c_str(),
                                  static_cast<int>(outer.size()), TRUE) == CSTR_EQUAL;
}

SaveRecordingDialogText ExternalSaveDialogText() {
    return {strings::Get(IDS_EXTERNAL_SAVE_DIALOG_TITLE), strings::Get(IDS_SHELL_FILE_TYPE_MP4),
            strings::Get(IDS_SHELL_FILE_TYPE_GIF)};
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
    // A recording's session stays for whoever decides about it. An external
    // video's work session holds nothing but Easnap's own results.
    if (external() && session_.has_value() && temp_store_ != nullptr &&
        !temp_store_->DeleteSession(*session_)) {
        platform::LogMessage(L"[Trim] The work session of an opened video could not be removed");
    }
    session_.reset();
    source_ = TrimSource{};
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

bool TrimFlow::EnsureExportController(media::RecordingFormat format) {
    if (trim_export_ && export_controller_format_ == format) {
        return true;
    }
    try {
        auto exporter = format == media::RecordingFormat::Gif ? media::CreateGifExporter()
                                                              : media::CreateMp4TrimExporter();
        if (!exporter) {
            return false;
        }
        // Only replaced between actions: no export of the old one is running.
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
        platform::LogMessage(L"[Trim] The export controller could not be created");
        return false;
    }
    export_controller_format_ = format;
    return true;
}

media::RecordingOptions TrimFlow::ExportOptions(media::RecordingFormat format) const {
    media::RecordingOptions options = recording_options_;
    options.format = format;
    return options;
}

std::wstring TrimFlow::TemporaryResultDirectory() const {
    if (external()) {
        return session_.has_value() ? session_->directory : std::wstring{};
    }
    return platform::DirectoryOf(source_.path);
}

bool TrimFlow::SourceReusable() const {
    return !external() || ExternalSourceIsReusable(source_.path, source_.facts);
}

bool TrimFlow::MayMoveArtifact(const std::wstring& path) const {
    if (path.empty() || path == source_.path) {
        return false;
    }
    if (!external()) {
        return true;  // Every file a recording's trim made is its own.
    }
    // The opened video is never handed to code that moves, deletes or secures
    // files: only what the flow made in its own work session may move.
    return session_.has_value() && PathInsideDirectory(path, session_->directory) &&
           !IsSameFile(path, source_.identity);
}

bool TrimFlow::ExternalSourceUnchanged() {
    const std::optional<media::VideoFileIdentity> now = ReadVideoFileIdentity(source_.path);
    if (now.has_value() && *now == source_.identity) {
        return true;
    }
    platform::LogMessage(L"[Trim] The opened video changed or disappeared after it was checked");
    if (trim_window_) {
        trim_window_->SetStatus(strings::Get(IDS_EXTERNAL_SOURCE_CHANGED), true);
    }
    return false;
}

void TrimFlow::Begin(const TrimSource& source, const media::RecordingOptions& options,
                     std::optional<media::TemporarySession> session) {
    if (state() != SessionState::RecordingTrimming || source.path.empty()) {
        return;
    }
    source_.kind = source.kind;
    if (source.kind == TrimSourceKind::Owned) {
        // Before anything that can throw: a failed start still keeps the recording.
        session_ = std::move(session);
    } else if (session.has_value()) {
        platform::LogMessage(L"[Trim] An opened video came with a session; it is not used");
    }

    try {
        if (trim_window_) {
            trim_window_->Destroy();
            trim_window_.reset();
        }
        trim_window_retired_ = false;
        source_ = source;
        recording_options_ = options;
        trim_artifact_.Clear();
        pending_trim_action_ = PendingTrimAction::None;
        pending_save_destination_.clear();
        trim_close_pending_ = false;
        if (external()) {
            // Never beside the user's file, never offered for recovery.
            session_ = temp_store_ != nullptr ? temp_store_->CreateSession() : std::nullopt;
        }

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

        const media::RecordingFormat first_format =
            external() && !source_.mp4_allowed ? media::RecordingFormat::Gif : recording_options_.format;
        if (!EnsureExportController(first_format)) {
            throw std::bad_alloc{};
        }
    } catch (...) {
        platform::LogMessage(L"The trim view could not be created");
        trim_window_.reset();
        trim_export_.reset();
    }

    ui::TrimWindow::Source window_source;
    window_source.kind = source_.kind;
    window_source.mp4_allowed = source_.mp4_allowed;
    if (external()) {
        window_source.file_name = VideoFileDisplayName(source_.path);
    }
    const bool work_session_ready = !external() || session_.has_value();
    if (!trim_window_ || !trim_export_ || !work_session_ready ||
        !trim_window_->Open(source_.path, source_.region, window_source)) {
        platform::LogMessage(L"The trim view could not be opened");
        if (external()) {
            // Nothing of the user's was touched; there is only this to say.
            ui::ShowErrorMessage(host_window_,
                                 strings::Format(IDS_VIDEO_OPEN_FAILED, {window_source.file_name}).c_str());
            FinishTrimWorkflow(true);
            return;
        }
        // The next start offers the recording only if it carries the marker;
        // marking is repeated here because its first attempt may have failed.
        const bool offered_later = session_.has_value() && temp_store_ && temp_store_->MarkRecoverable(*session_);
        if (offered_later) {
            ui::ShowErrorMessage(host_window_, strings::Get(IDS_RECORDING_KEPT_FOR_RECOVERY));
        } else {
            ui::ShowErrorMessage(host_window_,
                                 strings::Format(IDS_RECORDING_KEPT_UNTIL_NEXT_START, {source_.path}).c_str());
        }
        FinishTrimWorkflow(false);
        return;
    }
    ScheduleCleanup();
}

void TrimFlow::OnTrimSaveRequested(media::MediaTime start,
                                   media::MediaTime end) {
    const HWND owner = trim_window_ != nullptr ? trim_window_->hwnd() : host_window_;
    std::optional<std::wstring> destination;
    media::RecordingFormat format = recording_options_.format;
    if (external()) {
        if (!ExternalSourceUnchanged()) {
            return;
        }
        // Beside the original by default, under a name that says what it is.
        auto choice = ShowSaveVideoDialog(
            owner, source_.mp4_allowed,
            source_.mp4_allowed ? recording_options_.format : media::RecordingFormat::Gif,
            TrimmedVideoFileName(source_.path), platform::DirectoryOf(source_.path), ExternalSaveDialogText());
        if (!choice.has_value() || state() != SessionState::RecordingTrimming) {
            return;
        }
        // Compared by what the file is, not by how its path is written: a
        // hard link or another spelling of the same file is refused too.
        if (IsSameFile(choice->path, source_.identity)) {
            platform::LogMessage(L"[Trim] Save over the opened video itself was refused");
            if (trim_window_) {
                trim_window_->SetStatus(strings::Get(IDS_EXTERNAL_SAVE_IS_SOURCE), true);
            }
            return;
        }
        destination = std::move(choice->path);
        format = choice->format;
    } else {
        destination = ShowSaveRecordingDialog(
            owner, recording_options_.format,
            platform::TimestampedFileName(
                L"Easnap", RecordingFileExtension(recording_options_.format)),
            settings_.VideoStartDirectory(), RecordingSaveDialogText());
    }
    if (!destination.has_value()) {
        return;
    }
    const media::MediaTime duration =
        trim_window_ != nullptr ? trim_window_->range().source_duration()
                                : media::MediaTime::zero();
    const bool source_available =
        !ExistingFileDataPath(source_.path).empty() && SourceReusable();
    const bool artifact_available =
        trim_artifact_.Matches(start, end, format) &&
        !ExistingFileDataPath(trim_artifact_.path).empty();
    if (PlanTrimMediaPreparation(
            TrimTerminalAction::Save, start, end, duration,
            source_available, artifact_available, format) ==
        TrimMediaPreparation::ReuseFinalizedSource) {
        trim_artifact_.path = source_.path;
        trim_artifact_.start = start;
        trim_artifact_.end = end;
        trim_artifact_.format = format;
    }
    const DWORD attributes = ::GetFileAttributesW(destination->c_str());
    BeginTrimAction(PendingTrimAction::Save, start, end, format, *destination,
                    attributes != INVALID_FILE_ATTRIBUTES);
}

void TrimFlow::OnTrimCopyRequested(media::MediaTime start,
                                   media::MediaTime end) {
    media::RecordingFormat format = recording_options_.format;
    if (external()) {
        if (!ExternalSourceUnchanged() || trim_window_ == nullptr) {
            return;
        }
        // An opened video is copied as the format the user picks now.
        const std::optional<media::RecordingFormat> chosen = trim_window_->ChooseCopyFormat();
        if (!chosen.has_value() || state() != SessionState::RecordingTrimming) {
            return;
        }
        format = *chosen;
    }
    const media::MediaTime duration =
        trim_window_ != nullptr ? trim_window_->range().source_duration()
                                : media::MediaTime::zero();
    const bool source_available =
        !ExistingFileDataPath(source_.path).empty() && SourceReusable();
    const bool artifact_available =
        trim_artifact_.Matches(start, end, format) &&
        !ExistingFileDataPath(trim_artifact_.path).empty();
    if (PlanTrimMediaPreparation(
            TrimTerminalAction::Copy, start, end, duration,
            source_available, artifact_available, format) ==
        TrimMediaPreparation::ReuseFinalizedSource) {
        trim_artifact_.path = source_.path;
        trim_artifact_.start = start;
        trim_artifact_.end = end;
        trim_artifact_.format = format;
    }
    BeginTrimAction(PendingTrimAction::Copy, start, end, format);
}

void TrimFlow::BeginTrimAction(
    PendingTrimAction action, media::MediaTime start, media::MediaTime end,
    media::RecordingFormat format, std::wstring save_destination, bool replace_existing) {
    if (state() != SessionState::RecordingTrimming || action == PendingTrimAction::None ||
        start < media::MediaTime::zero() || end <= start) {
        return;
    }
    pending_trim_action_ = action;
    pending_trim_start_ = start;
    pending_trim_end_ = end;
    pending_format_ = format;
    pending_save_destination_ = std::move(save_destination);
    pending_save_replace_ = replace_existing;
    pending_trim_action_started_ms_ = ::GetTickCount64();

    if (trim_artifact_.Matches(start, end, format) &&
        !ExistingFileDataPath(trim_artifact_.path).empty()) {
        if (action == PendingTrimAction::Copy &&
            recording_completion_ != nullptr &&
            !recording_completion_->IsPreparedClipboardArtifact(
                trim_artifact_.path)) {
            const auto staged = recording_completion_->StageClipboardArtifact(
                trim_artifact_.path, format, MayMoveArtifact(trim_artifact_.path));
            if (!staged.has_value()) {
                pending_trim_action_ = PendingTrimAction::None;
                if (trim_window_) {
                    trim_window_->SetStatus(
                        strings::Get(TextFor(IDS_SESSION_TRIM_COPY_PREPARE_FAILED,
                                             IDS_EXTERNAL_COPY_PREPARE_FAILED)),
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
            recording_completion_->PrepareClipboardDestination(format);
        if (!destination.has_value()) {
            pending_trim_action_ = PendingTrimAction::None;
            if (trim_window_) {
                trim_window_->SetStatus(
                    strings::Get(TextFor(IDS_SESSION_TRIM_COPY_PREPARE_FAILED,
                                         IDS_EXTERNAL_COPY_PREPARE_FAILED)),
                    true);
            }
            return;
        }
        StartTrimExport(start, end, format, *destination, true);
        return;
    }
    if (action == PendingTrimAction::Save &&
        !pending_save_destination_.empty()) {
        if (recording_completion_ == nullptr ||
            !recording_completion_->IsAllowedSaveDestination(
                pending_save_destination_, format,
                pending_save_replace_)) {
            pending_trim_action_ = PendingTrimAction::None;
            if (trim_window_) {
                trim_window_->SetStatus(
                    strings::Get(TextFor(IDS_SESSION_TRIM_SAVE_DESTINATION_UNAVAILABLE,
                                         IDS_EXTERNAL_SAVE_DESTINATION_UNAVAILABLE)),
                    true);
            }
            return;
        }
        StartTrimExport(start, end, format, pending_save_destination_, false, true,
                        pending_save_replace_);
        return;
    }
    StartTrimExport(start, end, format);
}

void TrimFlow::StartTrimExport(media::MediaTime start,
                               media::MediaTime end,
                               media::RecordingFormat format,
                               std::wstring destination_path,
                               bool destination_is_clipboard,
                               bool destination_is_save,
                               bool replace_existing) {
    if (state() != SessionState::RecordingTrimming || !trim_export_ ||
        source_.path.empty() || start < media::MediaTime::zero() ||
        end <= start) {
        return;
    }

    const std::wstring directory = TemporaryResultDirectory();
    if (destination_path.empty() && directory.empty()) {
        if (trim_window_) {
            trim_window_->SetStatus(
                strings::Get(TextFor(IDS_SESSION_TRIM_TEMP_LOCATION_UNAVAILABLE,
                                     IDS_EXTERNAL_TEMP_LOCATION_UNAVAILABLE)),
                true);
        }
        return;
    }

    const auto abandon = [&](UINT owned_message, UINT external_message) {
        if (trim_window_) {
            trim_window_->SetStatus(strings::Get(TextFor(owned_message, external_message)), true);
        }
        pending_trim_action_ = PendingTrimAction::None;
        pending_save_destination_.clear();
        if (destination_is_clipboard && recording_completion_ != nullptr) {
            recording_completion_->AbandonPreparedClipboardArtifact();
        }
    };
    if (!EnsureExportController(format)) {
        abandon(IDS_SESSION_TRIM_EXPORT_START_FAILED, IDS_EXTERNAL_EXPORT_START_FAILED);
        return;
    }

    TrimExportRequest request;
    request.source_path = source_.path;
    const uint64_t generation =
        trim_export_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    request.destination_path = destination_path.empty()
                                   ? directory + L"\\trimmed-" +
                                         std::to_wstring(generation) + L"." +
                                         RecordingFileExtension(format)
                                   : std::move(destination_path);
    request.start = start;
    request.end = end;
    request.options = ExportOptions(format);
    request.replace_existing = replace_existing;
    if (!trim_export_->Start(std::move(request))) {
        abandon(IDS_SESSION_TRIM_EXPORT_START_FAILED, IDS_EXTERNAL_EXPORT_START_FAILED);
        return;
    }
    if (!RequestTransition(SessionState::Exporting)) {
        trim_export_->CancelAndWait();
        abandon(IDS_SESSION_TRIM_EXPORT_STATE_CHANGED, IDS_SESSION_TRIM_EXPORT_STATE_CHANGED);
        return;
    }
    trim_export_start_ = start;
    trim_export_end_ = end;
    trim_export_format_ = format;
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
    if (external()) {
        CloseExternalVideo();
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

void TrimFlow::CloseExternalVideo() {
    // Nothing of the user's can be lost: the file stays as it is, with a
    // preview or without one.
    if (state() == SessionState::RecordingTrimming) {
        FinishTrimWorkflow(true);
        return;
    }
    // Only an export in progress would be lost, and only its own result.
    const bool stop = ui::ConfirmDestructive(
        trim_window_ != nullptr ? trim_window_->hwnd() : host_window_,
        strings::Get(IDS_EXTERNAL_CLOSE_EXPORT_TEXT), strings::Get(IDS_EXTERNAL_CLOSE_EXPORT_TITLE),
        strings::Get(IDS_EXTERNAL_CLOSE_EXPORT_CONFIRM), strings::Get(IDS_EXTERNAL_CLOSE_EXPORT_KEEP));
    if (state() != SessionState::RecordingTrimming && state() != SessionState::Exporting) {
        return;  // The export finished and closed the session while the question was open.
    }
    const TrimCloseAction action = PlanTrimClose(stop, state() == SessionState::Exporting);
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
    if (::CopyFileW(source_.path.c_str(), destination->c_str(), FALSE) == FALSE) {
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
    const auto forget_action = [this] {
        if (trim_export_to_clipboard_ && recording_completion_ != nullptr) {
            recording_completion_->AbandonPreparedClipboardArtifact();
        }
        trim_export_to_clipboard_ = false;
        trim_export_to_save_ = false;
        pending_trim_action_ = PendingTrimAction::None;
        pending_save_destination_.clear();
    };
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
            trim_artifact_.format = trim_export_format_;
            trim_window_->SetExportProgress(1.0f);
            CompletePendingTrimAction();
            if (trim_window_ && trim_window_->visible() && trim_window_->exporting()) {
                trim_window_->SetExportResult(
                    strings::Get(TextFor(IDS_SESSION_TRIM_ACTION_FAILED, IDS_EXTERNAL_ACTION_FAILED)), true);
            }
            break;
        case TrimExportStatus::Cancelled:
            forget_action();
            trim_window_->SetExportResult(
                strings::Get(TextFor(IDS_SESSION_TRIM_EXPORT_CANCELLED, IDS_EXTERNAL_EXPORT_CANCELLED)));
            break;
        case TrimExportStatus::Failed:
            LogTrimExportFailure(snapshot.failure);
            forget_action();
            if (trim_export_format_ == media::RecordingFormat::Gif) {
                trim_window_->SetExportResult(
                    strings::Get(GifExportFailureMessageId(snapshot.failure.stage, external())), true);
            } else {
                trim_window_->SetExportResult(
                    strings::Get(TextFor(IDS_SESSION_TRIM_EXPORT_FAILED, IDS_EXTERNAL_EXPORT_FAILED)),
                    true);
            }
            break;
        default:
            forget_action();
            trim_window_->SetExportResult(
                strings::Get(TextFor(IDS_SESSION_TRIM_EXPORT_ENDED, IDS_EXTERNAL_EXPORT_ENDED)),
                true);
            break;
    }
}

void TrimFlow::CompletePendingTrimAction() {
    if (state() != SessionState::RecordingTrimming ||
        pending_trim_action_ == PendingTrimAction::None ||
        !trim_artifact_.Matches(pending_trim_start_, pending_trim_end_, pending_format_) ||
        ExistingFileDataPath(trim_artifact_.path).empty() ||
        recording_completion_ == nullptr) {
        return;
    }
    const PendingTrimAction action = pending_trim_action_;
    pending_trim_action_ = PendingTrimAction::None;
    if (action == PendingTrimAction::Save) {
        const std::wstring destination = pending_save_destination_;
        pending_save_destination_.clear();
        // Save copies: the file it reads from, the opened video included,
        // stays where and what it is.
        if (!recording_completion_->Save(trim_artifact_.path, destination,
                                         pending_format_,
                                         pending_save_replace_)) {
            if (trim_window_) {
                trim_window_->SetExportResult(
                    strings::Get(TextFor(IDS_SESSION_TRIM_SAVE_FAILED, IDS_EXTERNAL_SAVE_FAILED)),
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
        const auto staged = recording_completion_->StageClipboardArtifact(
            trim_artifact_.path, pending_format_, MayMoveArtifact(trim_artifact_.path));
        if (staged.has_value()) {
            trim_artifact_.path = *staged;
        }
    }
    if (!recording_completion_->PublishClipboardArtifact(host_window_,
                                                         trim_artifact_.path,
                                                         pending_format_)) {
        if (trim_window_) {
            trim_window_->SetExportResult(
                strings::Get(TextFor(IDS_SESSION_TRIM_COPY_FAILED, IDS_EXTERNAL_COPY_FAILED)),
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
    const bool opened_video = external();
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
    if (!opened_video) {
        services_.release_recording();  // An opened video has no recording to release.
    }
    if (recording_completion_ != nullptr) {
        recording_completion_->AbandonPreparedClipboardArtifact();
    }
    if (opened_video) {
        // The work session holds only what the flow made; it never stays.
        if (active_session.has_value() && temp_store_ != nullptr &&
            !temp_store_->DeleteSession(*active_session)) {
            platform::LogMessage(L"[Trim] The work session of an opened video could not be removed");
        }
    } else {
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
    }
    source_ = TrimSource{};
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
