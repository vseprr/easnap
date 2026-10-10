#include "ui/ui_theme.h"
#include "ui/native_controls.h"
// Easnap - session state machine and capture coordinator implementation.
#include "app/session_state.h"
#include <dwmapi.h>

#include <shlobj.h>

#include <new>
#include <utility>

#include "app/recording_workflow.h"
#include "editor/screenshot_editor.h"
#include "media/temp_session.h"
#include "recording/recording_session.h"
#include "ui/overlay.h"
#include "ui/recording_controls.h"
#include "ui/trim_window.h"

namespace easnap::app {
namespace {


std::wstring ExistingFileDataPath(const std::wstring& candidate) {
    if (candidate.empty()) {
        return {};
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (::GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard,
                               &attributes) == FALSE ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }
    const ULARGE_INTEGER size{attributes.nFileSizeLow,
                              attributes.nFileSizeHigh};
    return size.QuadPart == 0 ? std::wstring{} : candidate;
}

std::wstring ExistingRecordingDataPath(
    const recording::RecordingSession& session) {
    std::wstring candidate = session.finalized_path();
    if (candidate.empty()) {
        const std::optional<media::TemporarySession> temporary =
            session.temporary_session();
        if (temporary.has_value()) {
            candidate = temporary->recording_path();
        }
    }
    return ExistingFileDataPath(candidate);
}

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

const wchar_t* GifFailureAction(
    media::TrimExportFailureStage stage) noexcept {
    switch (stage) {
        case media::TrimExportFailureStage::SourceOpen:
        case media::TrimExportFailureStage::SourceValidation:
            return L"reading the recorded video";
        case media::TrimExportFailureStage::MediaTypeNegotiation:
            return L"preparing the recorded video for GIF conversion";
        case media::TrimExportFailureStage::VideoDecode:
        case media::TrimExportFailureStage::DecodedFrameAccess:
            return L"decoding the recorded video";
        case media::TrimExportFailureStage::SourceReset:
            return L"starting the final GIF encoding pass";
        case media::TrimExportFailureStage::PaletteQuantization:
            return L"building the GIF color palette";
        case media::TrimExportFailureStage::GifEncoding:
            return L"encoding GIF frames";
        case media::TrimExportFailureStage::StagingWrite:
            return L"writing the GIF output";
        case media::TrimExportFailureStage::Publication:
            return L"publishing the completed GIF";
        default:
            return L"converting the recording to GIF";
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
    std::wstring message = L"GIF export failed while ";
    message += GifFailureAction(stage);
    message += L". The source recording was kept so you can retry.";
    return message;
}

}  // namespace

const wchar_t* ToString(SessionState state) noexcept {
    switch (state) {
        case SessionState::Idle:
            return L"Idle";
        case SessionState::RegionSelecting:
            return L"RegionSelecting";
        case SessionState::ScreenshotEditing:
            return L"ScreenshotEditing";
        case SessionState::ScreenshotExporting:
            return L"ScreenshotExporting";
        case SessionState::RecordingConfiguring:
            return L"RecordingConfiguring";
        case SessionState::RecordingActive:
            return L"RecordingActive";
        case SessionState::RecordingPaused:
            return L"RecordingPaused";
        case SessionState::RecordingFinalizing:
            return L"RecordingFinalizing";
        case SessionState::RecordingTrimming:
            return L"RecordingTrimming";
        case SessionState::Exporting:
            return L"Exporting";
    }
    return L"Unknown";
}

bool IsRecordingState(SessionState state) noexcept {
    switch (state) {
        case SessionState::RecordingConfiguring:
        case SessionState::RecordingActive:
        case SessionState::RecordingPaused:
        case SessionState::RecordingFinalizing:
        case SessionState::RecordingTrimming:
        case SessionState::Exporting:
            return true;
        default:
            return false;
    }
}


// --- CaptureCoordinator -----------------------------------------------------

CaptureCoordinator& CaptureCoordinator::Instance() {
    static CaptureCoordinator instance;
    return instance;
}

CaptureCoordinator::~CaptureCoordinator() { Shutdown(); }

bool CaptureCoordinator::Initialize(HWND host_window) {
    if (initialized_) {
        return true;
    }
    host_window_ = host_window;
    settings_ = AppSettings::Load();
    ui::theme::ApplyPreference(settings_.appearance_theme);

    // Collect anything a previous crash left behind before the user notices it.
    temp_store_ = media::CreateTemporarySessionStore();
    if (temp_store_) {
        recording_completion_ = std::make_unique<RecordingCompletionController>(
            *temp_store_, CreateRecordingCompletionBackend());
        recording_completion_->CleanupStaleSessions();
    }
    if (::AddClipboardFormatListener(host_window_) == FALSE) {
        platform::LogMessage(L"Could not register the clipboard lifetime listener (error %lu)",
                             ::GetLastError());
    }

    state_ = SessionState::Idle;
    initialized_ = true;
    platform::LogMessage(L"Coordinator initialised (idle, no capture resources allocated)");
    return true;
}

void CaptureCoordinator::Shutdown() {
    if (!initialized_) {
        return;
    }
    StopRecordingForShutdown();
    StopTrimForShutdown();
    ::RemoveClipboardFormatListener(host_window_);
    if (recording_completion_) {
        recording_completion_->Shutdown();
    }
    EndSession();
    if (editor_) {
        editor_->Destroy();
        editor_.reset();
    }
    if (recording_controls_) {
        recording_controls_->Destroy();
        recording_controls_.reset();
    }
    // Both the editor and configuring recording island can be owned by the
    // overlay. Retire their HWNDs before Windows destroys that owner.
    if (overlay_) {
        overlay_->Destroy();
        overlay_.reset();
    }
    if (trim_window_) {
        trim_window_->Destroy();
        trim_window_.reset();
    }
    trim_export_.reset();
    recording_session_.reset();
    capture_provider_.reset();
    temp_store_.reset();
    snapshot_.Clear();
    trim_source_path_.clear();
    trim_artifact_.Clear();
    recording_completion_.reset();
    state_ = SessionState::Idle;
    initialized_ = false;
    host_window_ = nullptr;
}

bool CaptureCoordinator::IsTransitionLegal(SessionState from, SessionState to) noexcept {
    return IsSessionTransitionLegal(from, to);
}

bool CaptureCoordinator::RequestTransition(SessionState next) {
    if (!IsTransitionLegal(state_, next)) {
        platform::LogMessage(L"Rejected illegal transition %s -> %s", ToString(state_), ToString(next));
        return false;
    }
    if (state_ != next) {
        platform::LogMessage(L"State %s -> %s", ToString(state_), ToString(next));
    }
    state_ = next;
    return true;
}

void CaptureCoordinator::OnGlobalHotkey() {
    if (!initialized_) {
        return;
    }
    if (state_ != SessionState::Idle) {
        // Exactly one session may exist. Re-pressing the shortcut during a
        // session brings the active surface forward instead of stacking a new
        // overlay on top of it.
        if (overlay_ && overlay_->visible()) {
            if (recording_controls_ && recording_controls_->visible()) {
                recording_controls_->Activate();
            } else {
                ::SetForegroundWindow(overlay_->owner_window());
                ::SetFocus(overlay_->owner_window());
            }
            overlay_->Invalidate();
        } else if (recording_controls_ && recording_controls_->visible()) {
            recording_controls_->Activate();
        } else if (trim_window_ && trim_window_->visible()) {
            trim_window_->Activate();
        } else if (editor_ && editor_->visible()) {
            ::SetForegroundWindow(editor_->hwnd());
        }
        platform::LogMessage(L"Hotkey ignored: a session is already active (%s)", ToString(state_));
        return;
    }
    BeginCaptureSession();
}

bool CaptureCoordinator::BeginCaptureSession() {
    if (!RequestTransition(SessionState::RegionSelecting)) {
        return false;
    }

    session_monitors_ = platform::EnumerateMonitors();
    POINT initiating_pointer{};
    ::GetCursorPos(&initiating_pointer);

    // Capture resources are created lazily so an idle Easnap stays passive.
    if (!capture_provider_) {
        capture_provider_ = capture::CreateRegionCaptureProvider();
    }
    if (!capture_provider_) {
        ui::ShowErrorMessage(host_window_, L"Easnap could not initialise a screen capture provider.");
        RequestTransition(SessionState::Idle);
        return false;
    }

    snapshot_.Clear();
    if (!capture_provider_->CaptureDesktop(settings_.capture_cursor, snapshot_) || snapshot_.empty()) {
        ui::ShowErrorMessage(host_window_, L"Easnap could not read the desktop image.");
        capture_provider_->Release();
        RequestTransition(SessionState::Idle);
        return false;
    }

    if (!overlay_) {
        ui::OverlayController::Callbacks callbacks;
        callbacks.on_editing_changed = [this](CaptureMode mode, const RECT& region) {
            OnWorkspaceEditingChanged(mode, region);
        };
        callbacks.on_cancelled = [this]() { OnOverlayCancelled(); };
        callbacks.on_screenshot_complete = [this] { CompleteWorkspaceScreenshot(); };
        callbacks.on_region_changed = [this](const RECT& region) { OnWorkspaceRegionChanged(region); };
        callbacks.uses_annotation_tool = [this] { return editor_ && editor_->uses_annotation_tool(); };
        callbacks.wants_annotation_pointer = [this](POINT point) { return editor_ && editor_->WantsPointer(point); };
        callbacks.annotation_cursor = [this](POINT point) { return editor_ ? editor_->ImageCursor(point) : nullptr; };
        callbacks.on_frame_interacting = [this](bool value) { if(editor_) { editor_->SetFrameInteracting(value); } };
        callbacks.on_image_pointer = [this](UINT message, POINT point) {
            if (editor_) { editor_->ImagePointer(message, point); }
        };
        callbacks.on_editor_key = [this](UINT message, WPARAM key, LPARAM flags) {
            if (editor_) { editor_->Keyboard(message, key, flags); }
        };
        callbacks.on_paint_image = [this](HDC dc, const RECT& bounds, const RECT& repaint) {
            if (editor_) { editor_->RenderImage(dc, bounds, &repaint); }
        };
        overlay_ = std::make_unique<ui::OverlayController>(std::move(callbacks));
    }

    if (!overlay_->Show(snapshot_, initiating_pointer)) {
        ui::ShowErrorMessage(host_window_, L"Easnap could not open the capture overlay.");
        EndSession();
        return false;
    }
    return true;
}

void CaptureCoordinator::OnWorkspaceEditingChanged(CaptureMode mode, const RECT& region) {
    if (!overlay_ || !overlay_->visible() ||
        (state_ != SessionState::RegionSelecting && state_ != SessionState::ScreenshotEditing &&
         state_ != SessionState::RecordingConfiguring)) { return; }
    if (platform::IsEmptyRect(region)) {
        return;
    }

    if (mode == CaptureMode::Record) {
        if (editor_) { editor_->Hide(); }
        BeginRecordingConfiguration(region);
        return;
    }

    if (!RequestTransition(SessionState::ScreenshotEditing)) {
        return;
    }
    if (recording_controls_) { recording_controls_->Hide(); }
    OpenEditor(region);
}

void CaptureCoordinator::OnWorkspaceRegionChanged(const RECT& region) {
    if (editor_) { editor_->SetRegion(region, overlay_ && overlay_->selector().interacting()); }
    recording_region_ = MakeRecordingRegionEncoderCompatible(region);
    if (recording_controls_) { recording_controls_->SetRegion(region); }
}

void CaptureCoordinator::CompleteWorkspaceScreenshot() {
    if (!overlay_ || !overlay_->visible() || overlay_->mode() != CaptureMode::Screenshot) { return; }
    overlay_->EnterEditing();
    if (state_ == SessionState::ScreenshotEditing && editor_) { editor_->CompleteCopy(); }
}

bool CaptureCoordinator::PrepareWorkspaceScreenshot() {
    if (!editor_ || !overlay_ || state_ != SessionState::ScreenshotEditing) { return false; }
    if (editor_->has_image()) { return true; }
    const RECT region = overlay_->selector().selection();
    // Capture-excluded workspace windows can remain composed during acquisition.
    // Older Windows builds or any unprotected popup retain the safe hide fallback.
    const bool keep_visible=ui::CanCaptureVisibleWindows();
    if(!keep_visible) { editor_->Hide(); overlay_->Hide(); }
    ::DwmFlush();
    capture::DesktopSnapshot final_snapshot;
    platform::BgraImage image;
    const bool captured = capture_provider_ &&
        capture_provider_->CaptureDesktop(settings_.capture_cursor, final_snapshot) &&
        final_snapshot.CropRegion(region, image) && editor_->SetImage(std::move(image));
    if (captured) {
        overlay_->FixScreenshot();
        snapshot_.Clear();
        capture_provider_->Release();
    }
    if(!keep_visible) { overlay_->Resume(); editor_->Show(); }
    ::SetForegroundWindow(editor_->hwnd());
    ::SetFocus(editor_->hwnd());
    return captured;
}

void CaptureCoordinator::BeginRecordingConfiguration(const RECT& region) {
    recording_region_ = MakeRecordingRegionEncoderCompatible(region);
    if (platform::IsEmptyRect(recording_region_)) {
        ui::ShowErrorMessage(
            host_window_, L"The selected region is too small to record.");
        return;
    }
    if (!RequestTransition(SessionState::RecordingConfiguring)) {
        return;
    }

    if (!recording_controls_) {
        ui::RecordingControls::Callbacks callbacks;
        callbacks.on_start = [this](RecordingConfiguration configuration) { OnRecordingStart(std::move(configuration)); };
        callbacks.on_cancel = [this] { OnRecordingCancel(); };
        callbacks.on_pause = [this] { OnRecordingPause(); };
        callbacks.on_resume = [this] { OnRecordingResume(); };
        callbacks.on_stop = [this] { OnRecordingStop(); };
        callbacks.on_status_tick = [this] { OnRecordingStatusTick(); };
        callbacks.on_appearance = [this](AppearanceTheme preference) { SetAppearanceTheme(preference); };
        recording_controls_ = std::make_unique<ui::RecordingControls>(std::move(callbacks));
    }

    RecordingConfiguration configuration;
    configuration.format = settings_.recording_format;
    configuration.mp4_audio = settings_.recording_audio_source;
    configuration.include_cursor = settings_.capture_cursor;
    configuration.gif = settings_.recording_gif;
    if (!recording_controls_ ||
        !recording_controls_->ShowConfiguring(region, configuration, true, overlay_->panel_owner_window())) {
        ui::ShowErrorMessage(
            host_window_, L"Easnap could not open the recording controls.");
        RequestTransition(SessionState::Idle);
        EndSession();
        return;
    }
    ScheduleCleanup();
}

void CaptureCoordinator::OnRecordingStart(
    RecordingConfiguration configuration) {
    if (state_ != SessionState::RecordingConfiguring ||
        !IsValidRecordingConfiguration(configuration) || temp_store_ == nullptr) {
        return;
    }

    if (recording_controls_) { recording_controls_->SetError({}); }
    // Keep the same provisional windows available until Start succeeds.
    if (overlay_) { overlay_->Hide(); }
    if (editor_) { editor_->Hide(); }
    snapshot_.Clear();
    if (capture_provider_) { capture_provider_->Release(); }
    if (recording_controls_) {
        recording_controls_->SetRegion(recording_region_);
    }
    // Complete the desktop compositor's hide before the first live frame.
    ::DwmFlush();

    const media::RecordingOptions options =
        MakeRecordingOptions(recording_region_, configuration);
    recording_options_ = options;
    recording_session_ =
        recording::CreateVideoRecordingSession(*temp_store_, options);
    if (!recording_session_) {
        OnRecordingStartFailed(L"Easnap could not create the recording pipeline.");
        return;
    }

    if (!recording_session_->Start()) {
        OnRecordingStartFailed(recording_session_->last_error());
        return;
    }
    if (!RequestTransition(SessionState::RecordingActive)) {
        const bool finalized = recording_session_->Stop();
        FinishRecordingWorkflow(finalized);
        return;
    }

    settings_.capture_cursor = configuration.include_cursor;
    settings_.recording_format = configuration.format;
    settings_.recording_audio_source = configuration.mp4_audio;
    settings_.recording_gif = configuration.gif;
    PersistSettings();
    overlay_retired_ = overlay_ != nullptr;
    editor_retired_ = editor_ != nullptr;
    if (recording_controls_) {
        recording_controls_->RetireWorkspace();
        recording_controls_->SetElapsed(0);
        recording_controls_->SetSessionState(SessionState::RecordingActive);
    }
    ScheduleCleanup();
}

void CaptureCoordinator::OnRecordingStartFailed(const std::wstring& message) {
    platform::LogMessage(L"Recording start failed: %ls", message.c_str());
    std::wstring retained;
    if (recording_session_) {
        recording_session_->Abort();
        retained = ExistingRecordingDataPath(*recording_session_);
        recording_session_.reset(); // Private store still owns partial media.
    }
    if (overlay_) { overlay_->Resume(); }
    if (recording_controls_) {
        recording_controls_->SetError(L"Could not start recording. Check the audio source and try again.");
        recording_controls_->Activate();
    }
    if (!retained.empty()) {
        const std::wstring detail = message + L"\n\nTemporary media was preserved at:\n" + retained;
        ui::ShowMessage(recording_controls_ ? recording_controls_->hwnd() : host_window_, detail.c_str(),
                      L"Easnap", MB_OK | MB_ICONWARNING);
    }
}

void CaptureCoordinator::OnRecordingCancel() {
    if (state_ != SessionState::RecordingConfiguring) {
        return;
    }
    RequestTransition(SessionState::Idle);
    EndSession();
}

void CaptureCoordinator::OnRecordingPause() {
    if (state_ != SessionState::RecordingActive || !recording_session_) {
        return;
    }
    if (!recording_session_->Pause()) {
        if (recording_session_->state() ==
            recording::RecordingSessionState::Failed) {
            BeginRecordingFinalization();
        }
        return;
    }
    if (!RequestTransition(SessionState::RecordingPaused)) {
        static_cast<void>(recording_session_->Resume());
        return;
    }
    if (recording_controls_) {
        recording_controls_->SetSessionState(SessionState::RecordingPaused);
    }
}

void CaptureCoordinator::OnRecordingResume() {
    if (state_ != SessionState::RecordingPaused || !recording_session_) {
        return;
    }
    if (!recording_session_->Resume()) {
        if (recording_session_->state() ==
            recording::RecordingSessionState::Failed) {
            BeginRecordingFinalization();
        }
        return;
    }
    if (!RequestTransition(SessionState::RecordingActive)) {
        static_cast<void>(recording_session_->Pause());
        return;
    }
    if (recording_controls_) {
        recording_controls_->SetSessionState(SessionState::RecordingActive);
    }
}

void CaptureCoordinator::OnRecordingStop() {
    if (state_ == SessionState::RecordingActive ||
        state_ == SessionState::RecordingPaused) {
        BeginRecordingFinalization();
    }
}

void CaptureCoordinator::OnRecordingStatusTick() {
    if (state_ == SessionState::RecordingFinalizing) {
        OnRecordingFinalized(recording_finalizer_.generation());
        return;
    }
    if ((state_ != SessionState::RecordingActive &&
         state_ != SessionState::RecordingPaused) ||
        !recording_session_) {
        return;
    }

    const recording::RecordingSessionDiagnostics diagnostics =
        recording_session_->diagnostics();
    if (recording_controls_) {
        recording_controls_->SetElapsed(diagnostics.active_duration_100ns);
    }
    if (recording_session_->has_runtime_failure()) {
        BeginRecordingFinalization();
    }
}

void CaptureCoordinator::BeginRecordingFinalization() {
    if ((state_ != SessionState::RecordingActive &&
         state_ != SessionState::RecordingPaused) ||
        !recording_session_ || recording_finalizer_.has_worker()) {
        return;
    }
    if (!RequestTransition(SessionState::RecordingFinalizing)) {
        return;
    }
    if (recording_controls_) {
        recording_controls_->SetSessionState(SessionState::RecordingFinalizing);
    }

    try {
        if (recording_finalizer_.Start(
                host_window_, kMsgRecordingFinalized,
                [session = recording_session_.get()] { return session->Stop(); })) {
            return;
        }
    } catch (...) {
        // Preparing the callable can allocate before Start is entered.
    }
    // Preserve the existing safe drain fallback if a worker cannot be created.
    FinishRecordingWorkflow(recording_session_->Stop());
}

void CaptureCoordinator::OnRecordingFinalized(RecordingFinalizer::Generation generation) {
    if (!initialized_ || state_ != SessionState::RecordingFinalizing ||
        !recording_session_) {
        return;
    }
    const auto result = recording_finalizer_.TryTake(generation);
    if (!result.has_value()) {
        return;
    }
    if (result->notification_error != ERROR_SUCCESS) {
        platform::LogMessage(L"Recovered recording completion after PostMessage failed: %lu",
                             result->notification_error);
    }
    if (result->worker_exception) {
        platform::LogMessage(L"Recording finalization failed with an exception");
    }
    FinishRecordingWorkflow(result->finalized);
}

void CaptureCoordinator::FinishRecordingWorkflow(bool finalized) {
    if (!recording_session_) {
        return;
    }
    if (recording_controls_) {
        recording_controls_->Hide();
        recording_controls_retired_ = true;
    }
    const std::wstring retained_path =
        ExistingRecordingDataPath(*recording_session_);
    const SessionState next = StateAfterRecordingFinalization(
        finalized, !retained_path.empty());
    if (next == SessionState::RecordingTrimming && RequestTransition(next)) {
        BeginTrimWorkflow(retained_path);
        return;
    }

    std::wstring message = recording_session_->last_error();
    if (message.empty()) {
        message = finalized
                      ? L"Easnap finalized the recording, but the temporary working media could not be found."
                      : L"Easnap could not complete the recording.";
    }
    if (!retained_path.empty()) {
        message += L"\n\nTemporary media data was preserved at:\n" +
                   retained_path;
    }
    ui::ShowMessage(host_window_, message.c_str(), L"Easnap",
                  MB_OK | MB_ICONWARNING);
    RequestTransition(SessionState::Idle);
    recording_session_.reset();
    EndSession();
}

void CaptureCoordinator::BeginTrimWorkflow(const std::wstring& source_path) {
    if (state_ != SessionState::RecordingTrimming || source_path.empty()) {
        return;
    }

    try {
        if (trim_window_) {
            trim_window_->Destroy();
            trim_window_.reset();
        }
        trim_window_retired_ = false;
        trim_source_path_ = source_path;
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
        trim_window_.reset();
        trim_export_.reset();
    }

    if (!trim_window_ || !trim_export_ ||
        !trim_window_->Open(trim_source_path_, recording_region_)) {
        ui::ShowErrorMessage(
            host_window_,
            L"Easnap could not open the trim view. The finalized recording remains in temporary storage until Easnap exits.");
        FinishTrimWorkflow(false);
        return;
    }
    ScheduleCleanup();
}

void CaptureCoordinator::OnTrimSaveRequested(media::MediaTime start,
                                             media::MediaTime end) {
    const auto destination = ShowSaveRecordingDialog(
        trim_window_ != nullptr ? trim_window_->hwnd() : host_window_,
        recording_options_.format,
        platform::TimestampedFileName(
            L"Easnap", RecordingFileExtension(recording_options_.format)),
        settings_.last_save_directory);
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

void CaptureCoordinator::OnTrimCopyRequested(media::MediaTime start,
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

void CaptureCoordinator::BeginTrimAction(
    PendingTrimAction action, media::MediaTime start, media::MediaTime end,
    std::wstring save_destination, bool replace_existing) {
    if (state_ != SessionState::RecordingTrimming || action == PendingTrimAction::None ||
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
                        L"Copy preparation failed. The source recording was kept so you can retry.",
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
                    L"Copy preparation failed. The source recording was kept so you can retry.",
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
                    L"The selected Save destination is unavailable or not durable. The source was kept.",
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

void CaptureCoordinator::StartTrimExport(media::MediaTime start,
                                         media::MediaTime end,
                                         std::wstring destination_path,
                                         bool destination_is_clipboard,
                                         bool destination_is_save,
                                         bool replace_existing) {
    if (state_ != SessionState::RecordingTrimming || !trim_export_ ||
        trim_source_path_.empty() || start < media::MediaTime::zero() ||
        end <= start) {
        return;
    }

    const std::wstring directory = platform::DirectoryOf(trim_source_path_);
    if (destination_path.empty() && directory.empty()) {
        if (trim_window_) {
            trim_window_->SetStatus(
                L"The temporary recording location is unavailable. The source was kept.",
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
                L"The trim export could not start. The source recording was kept.",
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
                L"The trim export was stopped because the session state changed.",
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

void CaptureCoordinator::OnTrimExportCancel() {
    if (state_ != SessionState::Exporting || !trim_export_) {
        return;
    }
    trim_export_->Cancel();
    if (trim_window_) {
        trim_window_->SetExporting(true, true);
    }
}

void CaptureCoordinator::OnTrimWindowClosed() {
    if (state_ != SessionState::RecordingTrimming &&
        state_ != SessionState::Exporting) {
        return;
    }
    const int answer = ui::ShowMessage(
        trim_window_ != nullptr ? trim_window_->hwnd() : host_window_,
        L"Discard this recording?\n\nThe unsaved recording and its temporary trim data will be permanently deleted.",
        L"Discard recording", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    const TrimCloseAction action =
        PlanTrimClose(answer == IDYES, state_ == SessionState::Exporting);
    if (action == TrimCloseAction::CancelExportThenDiscard) {
        trim_close_pending_ = true;
        OnTrimExportCancel();
    } else if (action == TrimCloseAction::DiscardNow) {
        FinishTrimWorkflow(true);
    }
}

void CaptureCoordinator::OnTrimStatusTick() {
    if (state_ != SessionState::Exporting || !trim_export_) {
        return;
    }
    const TrimExportSnapshot snapshot = trim_export_->Snapshot();
    if (trim_window_) {
        trim_window_->SetExportProgress(snapshot.progress);
    }
    if (snapshot.status != TrimExportStatus::Running) {
        OnTrimExportCompleted(
            trim_export_generation_.load(std::memory_order_acquire));
    }
}

void CaptureCoordinator::OnTrimExportCompleted(uint64_t export_generation) {
    if (export_generation !=
            trim_export_generation_.load(std::memory_order_acquire) ||
        state_ != SessionState::Exporting || !trim_export_) {
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
                settings_.last_save_directory =
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
                    L"The requested action could not be completed. The temporary recording was kept so you can retry.", true);
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
                L"Trim export cancelled. The source recording was kept.");
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
                    L"Trim export failed. The source recording was kept so you can retry.",
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
                L"Trim export ended unexpectedly. The source recording was kept.",
                true);
            break;
    }
}

void CaptureCoordinator::CompletePendingTrimAction() {
    if (state_ != SessionState::RecordingTrimming ||
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
                    L"Save failed. The temporary recording was kept so you can retry.",
                    true);
            }
            return;
        }
        settings_.last_save_directory = platform::DirectoryOf(destination);
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
                L"Copy failed. The temporary recording was kept so you can retry.",
                true);
        }
        return;
    }
    platform::LogMessage(
        L"[Completion] Copy action completed in %llu ms",
        ::GetTickCount64() - pending_trim_action_started_ms_);
    FinishTrimWorkflow(true);
}

void CaptureCoordinator::FinishTrimWorkflow(bool delete_active_session) {
    if (state_ == SessionState::Exporting && trim_export_) {
        trim_export_->CancelAndWait();
    }
    if (state_ == SessionState::RecordingTrimming ||
        state_ == SessionState::Exporting) {
        RequestTransition(SessionState::Idle);
    }
    std::optional<media::TemporarySession> active_session;
    if (recording_session_) {
        active_session = recording_session_->temporary_session();
    }
    if (trim_window_) {
        trim_window_->Hide();
        trim_window_->ReleaseMedia();
        trim_window_retired_ = true;
    }
    trim_export_.reset();
    recording_session_.reset();
    if (recording_completion_ != nullptr) {
        recording_completion_->AbandonPreparedClipboardArtifact();
    }
    if (delete_active_session && active_session.has_value() &&
        recording_completion_ != nullptr &&
        !recording_completion_->Discard(*active_session)) {
        platform::LogMessage(
            L"Temporary recording cleanup was deferred because a file remained in use");
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

void CaptureCoordinator::OnClipboardChanged() {
    if (recording_completion_ != nullptr) {
        recording_completion_->OnClipboardChanged();
    }
}

void CaptureCoordinator::StopTrimForShutdown() {
    if (trim_export_) {
        trim_export_->CancelAndWait();
    }
}

void CaptureCoordinator::StopRecordingForShutdown() {
    if (recording_finalizer_.has_worker()) {
        recording_finalizer_.Join();
        return;
    }
    if (!recording_session_) {
        return;
    }
    const recording::RecordingSessionState session_state =
        recording_session_->state();
    if (session_state == recording::RecordingSessionState::Recording ||
        session_state == recording::RecordingSessionState::Paused ||
        session_state == recording::RecordingSessionState::Failed) {
        static_cast<void>(recording_session_->Stop());
    } else if (session_state != recording::RecordingSessionState::Finalized) {
        recording_session_->Abort();
    }
}

void CaptureCoordinator::OpenEditor(const RECT& region) {
    if (editor_ && editor_->hwnd()) {
        editor_->SetRegion(region);
        editor_->Show();
        return;
    }
    editor::ScreenshotEditor::Callbacks callbacks;
    callbacks.on_appearance = [this](AppearanceTheme value) { SetAppearanceTheme(value); };
    callbacks.on_closed = [this](bool exported) { OnEditorClosed(exported); };
    callbacks.on_feature_used = [this] { if (overlay_) { overlay_->workspace().EditorFeatureUsed(); } };
    callbacks.on_prepare_image = [this] { return PrepareWorkspaceScreenshot(); };
    callbacks.on_document_committed = [this] {
        if (overlay_ && overlay_->workspace().DocumentCommitted() && editor_ && !editor_->completing()) {
            CompleteWorkspaceScreenshot();
        }
    };
    callbacks.on_image_invalidated = [this](const RECT& damage) {
        if (overlay_) { overlay_->InvalidateImage(damage); }
    };
    editor_ = std::make_unique<editor::ScreenshotEditor>(std::move(callbacks));
    if (!editor_->Open(region, &settings_, overlay_->panel_owner_window())) {
        ui::ShowErrorMessage(host_window_, L"Easnap could not open the screenshot tools.");
        OnOverlayCancelled();
    }
}

void CaptureCoordinator::OnOverlayCancelled() {
    RequestTransition(SessionState::Idle);
    EndSession();
}

void CaptureCoordinator::OnEditorClosed(bool exported) {
    if (state_ == SessionState::ScreenshotEditing) {
        if (exported) {
            RequestTransition(SessionState::ScreenshotExporting);
        }
        RequestTransition(SessionState::Idle);
    }
    editor_retired_ = true;
    EndSession();
}

void CaptureCoordinator::OnEscapeRequested() {
    if (state_ == SessionState::Idle) {
        return;
    }
    if (state_ == SessionState::RecordingConfiguring) {
        OnRecordingCancel();
        return;
    }
    if (state_ == SessionState::RecordingActive ||
        state_ == SessionState::RecordingPaused) {
        OnRecordingStop();
        return;
    }
    if (state_ == SessionState::RecordingFinalizing) {
        if (recording_controls_) {
            recording_controls_->Activate();
        }
        return;
    }
    if (state_ == SessionState::RecordingTrimming ||
        state_ == SessionState::Exporting) {
        OnTrimWindowClosed();
        return;
    }
    if (editor_ && editor_->visible()) {
        editor_->Close(false);
        return;
    }
    if (overlay_ && overlay_->visible()) {
        overlay_->HandleCancelled();
        return;
    }
    RequestTransition(SessionState::Idle);
    EndSession();
}

namespace {
bool SameDisplayLayout(const std::vector<platform::MonitorInfo>& before,
                       const std::vector<platform::MonitorInfo>& after) {
    return before.size() == after.size() &&
           std::equal(before.begin(), before.end(), after.begin(),
                      [](const platform::MonitorInfo& a, const platform::MonitorInfo& b) {
                          return ::EqualRect(&a.bounds, &b.bounds) != FALSE && a.dpi == b.dpi;
                      });
}
}  // namespace

void CaptureCoordinator::OnDisplayChanged() {
    if (state_ == SessionState::Idle) {
        return;
    }
    if (overlay_ && overlay_->visible()) {
        // Work-area notifications (taskbar auto-hide, a docked app bar) arrive
        // here too and leave the frozen snapshot valid. Only a changed monitor
        // layout or scale makes it stale; cancelling then is the one outcome
        // that cannot show the user outdated pixels.
        if (SameDisplayLayout(session_monitors_, platform::EnumerateMonitors())) {
            return;
        }
        platform::LogMessage(L"Display layout changed during selection - cancelling the session");
        OnOverlayCancelled();
        return;
    }
    if (IsRecordingState(state_)) {
        // The capture provider rejects a region that left the virtual screen
        // and the worker would repeat the last frame until Stop. End the
        // recording instead so the user keeps what was really captured.
        const RECT visible = platform::IntersectRects(recording_region_, platform::VirtualScreenBounds());
        if (::EqualRect(&visible, &recording_region_) == FALSE) {
            platform::LogMessage(L"The recording region left the desktop - ending the recording");
            if (state_ == SessionState::RecordingActive || state_ == SessionState::RecordingPaused) {
                OnRecordingStop();
                return;
            }
            if (state_ == SessionState::RecordingConfiguring) {
                OnRecordingCancel();
                return;
            }
        }
        // Otherwise a recording survives display changes. Keep the selected
        // desktop coordinates and reposition only its user-facing chrome.
        if (trim_window_ && trim_window_->visible()) {
            trim_window_->RefreshLayout();
        } else if (recording_controls_) {
            recording_controls_->RefreshLayout();
        }
        return;
    }
}

void CaptureCoordinator::ScheduleCleanup() {
    if (host_window_ != nullptr) {
        ::PostMessageW(host_window_, kMsgSessionCleanup, 0, 0);
    }
}

void CaptureCoordinator::OnDeferredCleanup() {
    if (editor_retired_) {
        editor_retired_ = false;
        if (editor_) {
            editor_->Destroy();
        }
    }
    if (recording_controls_retired_) {
        recording_controls_retired_ = false;
        if (recording_controls_) {
            recording_controls_->Destroy();
            recording_controls_.reset();
        }
    }
    // The editor and configuring recording island share this owner. Active
    // recording has already detached it in RetireWorkspace.
    if (overlay_retired_) {
        overlay_retired_ = false;
        if (overlay_) {
            overlay_->Destroy();
        }
    }
    if (trim_window_retired_) {
        trim_window_retired_ = false;
        if (trim_window_) {
            trim_window_->Destroy();
            trim_window_.reset();
        }
    }

    if (state_ == SessionState::Idle) {
        // Session finished: release GPU/duplication resources and the frozen
        // desktop copy so an idle Easnap keeps a small, boring footprint.
        snapshot_.Clear();
        if (capture_provider_) {
            capture_provider_->Release();
        }
        PersistSettings();
    }
}

void CaptureCoordinator::EndSession() {
    if (overlay_) {
        overlay_->Hide();
        overlay_retired_ = true;
    }
    if (editor_ && editor_->visible()) {
        editor_->Close(false);
    }
    editor_retired_ = editor_retired_ || editor_ != nullptr;
    if (recording_controls_) {
        recording_controls_->Hide();
        recording_controls_retired_ = true;
    }
    if (trim_window_) {
        trim_window_->Hide();
        trim_window_retired_ = true;
    }
    ScheduleCleanup();
}

bool CaptureCoordinator::ProcessDialogMessage(MSG& message) const {
    if (editor_ && editor_->ProcessDialogMessage(message)) { return true; }
    if (overlay_ && overlay_->ProcessDialogMessage(message)) { return true; }
    if (recording_controls_ && recording_controls_->visible() &&
        recording_controls_->ProcessDialogMessage(message)) {
        return true;
    }
    return trim_window_ && trim_window_->visible() &&
           trim_window_->ProcessDialogMessage(message);
}

bool CaptureCoordinator::PersistSettings() {
    if (!settings_.Save()) {
        platform::LogMessage(L"Settings could not be written");
        return false;
    }
    return true;
}

void CaptureCoordinator::SetAppearanceTheme(AppearanceTheme preference) {
    if (!IsValidAppearanceTheme(preference)) { return; }
    // This settings object contains accepted recording preferences only. Pending
    // panel values stay in RecordingConfiguration until a successful Start.
    settings_.appearance_theme = preference;
    ui::theme::ApplyPreference(preference);
    ui::theme::NotifyWindows();
    PersistSettings(); // One failure log, no modal interruption or redraw retry.
}

void CaptureCoordinator::OnVisualSettingsChanged() {
    ui::theme::RefreshSystemColors();
    ui::theme::NotifyWindows();
}

}  // namespace easnap::app
