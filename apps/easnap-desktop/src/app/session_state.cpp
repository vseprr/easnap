#include "ui/ui_theme.h"
#include "ui/native_controls.h"
// Easnap - session state machine and capture coordinator implementation.
#include "app/session_state.h"
#include <dwmapi.h>

#include <shlobj.h>

#include <chrono>
#include <new>
#include <utility>

#include "app/recording_workflow.h"
#include "editor/screenshot_editor.h"
#include "media/temp_session.h"
#include "recording/recording_session.h"
#include "scrollshot/scrollshot_session.h"
#include "strings/strings.h"
#include "ui/overlay.h"
#include "ui/recording_controls.h"
#include "ui/scrollshot_controls.h"
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
        case SessionState::ScrollshotConfiguring:
            return L"ScrollshotConfiguring";
        case SessionState::ScrollshotCapturing:
            return L"ScrollshotCapturing";
        case SessionState::ScrollshotReview:
            return L"ScrollshotReview";
    }
    return L"Unknown";
}

bool IsScrollshotState(SessionState state) noexcept {
    return state == SessionState::ScrollshotConfiguring || state == SessionState::ScrollshotCapturing ||
           state == SessionState::ScrollshotReview;
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
    StopScrollshotForShutdown();
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
    if (scrollshot_controls_) {
        scrollshot_controls_->Destroy();
        scrollshot_controls_.reset();
    }
    // The editor, the configuring recording island and the Scrollshot island
    // can be owned by the overlay. Retire their HWNDs before Windows destroys
    // that owner.
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
    snapshot_.Clear();
    trim_source_path_.clear();
    trim_artifact_.Clear();
    // The completion controller refers to the store: it goes first.
    recording_completion_.reset();
    temp_store_.reset();
    state_ = SessionState::Idle;
    initialized_ = false;
    host_window_ = nullptr;
}

// --- Exit, session end and recovery ------------------------------------------

PendingWork CaptureCoordinator::pending_work() const {
    if (!initialized_) {
        return PendingWork::None;
    }
    const bool image_at_risk = editor_ && editor_->visible() &&
                               (editor_->has_annotations() || editor_->document_mode());
    return PendingWorkFor(state_, image_at_risk);
}

HWND CaptureCoordinator::DialogOwner() const {
    if (trim_window_ && trim_window_->visible()) { return trim_window_->hwnd(); }
    if (recording_controls_ && recording_controls_->visible()) { return recording_controls_->hwnd(); }
    if (scrollshot_controls_ && scrollshot_controls_->visible()) { return scrollshot_controls_->hwnd(); }
    if (editor_ && editor_->visible() && editor_->hwnd() != nullptr) { return editor_->hwnd(); }
    return host_window_;
}

bool CaptureCoordinator::ConfirmDiscardImage(HWND owner, bool scrollshot) {
    return ui::ConfirmDestructive(
        owner, strings::Get(scrollshot ? IDS_DISCARD_SCROLLSHOT_TEXT : IDS_DISCARD_IMAGE_TEXT),
        strings::Get(IDS_DISCARD_IMAGE_TITLE), strings::Get(IDS_DISCARD_CONFIRM), strings::Get(IDS_DISCARD_KEEP));
}

bool CaptureCoordinator::ConfirmExit() {
    if (!initialized_) {
        return true;
    }
    // One of the application's dialogs or menus is running its own message
    // loop. Its caller is on the stack and must find its objects when it
    // returns, so the application cannot be taken down from here.
    if (platform::ModalLoopActive() || ui::PopupActive()) {
        ::MessageBeep(MB_ICONWARNING);
        if (HWND active = ::GetActiveWindow()) {
            FLASHWINFO flash{sizeof(flash), active, FLASHW_CAPTION, 3, 0};
            ::FlashWindowEx(&flash);
        }
        return false;
    }
    UINT text = 0;
    switch (pending_work()) {
        case PendingWork::None: return true;
        case PendingWork::Recording: text = IDS_EXIT_RECORDING; break;
        case PendingWork::UnsavedRecording: text = IDS_EXIT_UNSAVED_RECORDING; break;
        case PendingWork::Export: text = IDS_EXIT_EXPORT; break;
        case PendingWork::Image: text = IDS_EXIT_IMAGE; break;
    }
    platform::LogMessage(L"Exit requested with unsaved work (state %d)", static_cast<int>(state_));
    return ui::ConfirmDestructive(DialogOwner(), strings::Get(text), strings::Get(IDS_EXIT_TITLE),
                                  strings::Get(IDS_EXIT_CONFIRM), strings::Get(IDS_EXIT_STAY));
}

const wchar_t* CaptureCoordinator::ShutdownBlockReason() const {
    switch (pending_work()) {
        case PendingWork::Recording: return strings::Get(IDS_SHUTDOWN_REASON_RECORDING);
        case PendingWork::UnsavedRecording:
        case PendingWork::Export: return strings::Get(IDS_SHUTDOWN_REASON_UNSAVED);
        case PendingWork::Image: return strings::Get(IDS_SHUTDOWN_REASON_IMAGE);
        case PendingWork::None: break;
    }
    return nullptr;
}

void CaptureCoordinator::PrepareForSessionEnd() {
    if (!initialized_) {
        return;
    }
    platform::LogMessage(L"The Windows session is ending (state %d)", static_cast<int>(state_));
    PersistSettings();
    // A recording that is still running is closed, so that there is a file to
    // offer again. Windows allows a few seconds for this.
    if (recording_finalizer_.has_worker()) {
        recording_finalizer_.Join();
    } else if (recording_session_ &&
               (state_ == SessionState::RecordingActive || state_ == SessionState::RecordingPaused)) {
        static_cast<void>(recording_session_->Stop());
    }
    if (trim_export_) {
        trim_export_->CancelAndWait();
    }
    if (recording_session_ && temp_store_) {
        const std::optional<media::TemporarySession> session = recording_session_->temporary_session();
        const bool finished =
            recording_session_->state() == recording::RecordingSessionState::Finalized &&
            !ExistingRecordingDataPath(*recording_session_).empty();
        // Marked and no longer owned: neither the end of this process nor the
        // sweep of the next start deletes it.
        if (session.has_value() && finished && temp_store_->MarkRecoverable(*session)) {
            static_cast<void>(temp_store_->ReleaseSession(*session));
            platform::LogMessage(L"The unsaved recording is kept for the next start");
        }
    }
}

void CaptureCoordinator::OfferRecoveredRecordings() {
    if (!initialized_ || !temp_store_ || state_ != SessionState::Idle) {
        return;
    }
    enum : int { kSave = 100, kDelete = 101, kLater = 102 };
    for (const media::TemporarySession& session : temp_store_->AdoptRecoverableSessions()) {
        const std::wstring source = session.recording_path();
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (::GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &data) == FALSE) {
            static_cast<void>(temp_store_->DeleteSession(session));
            continue;
        }
        // When and how large, in the user's own date and time format.
        std::wstring when;
        FILETIME local{};
        SYSTEMTIME time{};
        if (::FileTimeToLocalFileTime(&data.ftLastWriteTime, &local) != FALSE &&
            ::FileTimeToSystemTime(&local, &time) != FALSE) {
            wchar_t date_text[64]{}, time_text[64]{};
            ::GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &time, nullptr, date_text,
                              ARRAYSIZE(date_text), nullptr);
            ::GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr, time_text,
                              ARRAYSIZE(time_text));
            when = std::wstring(date_text) + L" " + time_text;
        }
        const ULONGLONG bytes = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        const ULONGLONG megabytes = std::max<ULONGLONG>(1, (bytes + 512 * 1024) / (1024 * 1024));
        const std::wstring text = strings::Format(IDS_RECOVERY_TEXT, {when, std::to_wstring(megabytes)});
        const int choice = ui::ShowChoice(
            host_window_, text.c_str(), strings::Get(IDS_RECOVERY_TITLE),
            {{kSave, strings::Get(IDS_RECOVERY_SAVE)},
             {kDelete, strings::Get(IDS_RECOVERY_DELETE), true},
             {kLater, strings::Get(IDS_NOT_NOW)}},
            kSave, kLater);
        if (!initialized_ || !temp_store_) {
            return;
        }
        if (choice == kDelete) {
            platform::LogMessage(L"An unsaved recording from an earlier run was deleted on request");
            static_cast<void>(temp_store_->DeleteSession(session));
            continue;
        }
        bool saved = false;
        if (choice == kSave) {
            // No owner: the hidden host window would pin the dialog to the
            // corner of the primary display.
            const auto destination = ShowSaveRecordingDialog(
                nullptr, media::RecordingFormat::Mp4,
                platform::TimestampedFileName(L"Easnap", RecordingFileExtension(media::RecordingFormat::Mp4)),
                settings_.last_save_directory);
            if (!initialized_ || !temp_store_) {
                return;
            }
            if (destination.has_value()) {
                saved = ::CopyFileW(source.c_str(), destination->c_str(), FALSE) != FALSE;
                if (saved) {
                    settings_.last_save_directory = platform::DirectoryOf(*destination);
                    PersistSettings();
                    platform::LogMessage(L"An unsaved recording from an earlier run was saved");
                } else {
                    platform::LogMessage(L"Saving a recovered recording failed (error %lu)", ::GetLastError());
                    ui::ShowErrorMessage(host_window_, strings::Get(IDS_RECOVERY_SAVE_FAILED));
                }
            }
        }
        if (saved) {
            static_cast<void>(temp_store_->DeleteSession(session));
        } else {
            // Not now, cancelled or failed: it stays and is offered again.
            static_cast<void>(temp_store_->ReleaseSession(session));
        }
    }
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
        if (IsScrollshotState(state_) && scrollshot_controls_ && scrollshot_controls_->visible()) {
            // The island takes the focus, so Enter, Ctrl+S and Esc work again
            // after the scrolled application had it.
            scrollshot_controls_->Activate();
        } else if (overlay_ && overlay_->visible()) {
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

    EnsureOverlay();

    if (!overlay_->Show(snapshot_, initiating_pointer)) {
        ui::ShowErrorMessage(host_window_, L"Easnap could not open the capture overlay.");
        EndSession();
        return false;
    }
    return true;
}

void CaptureCoordinator::EnsureOverlay() {
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
        callbacks.on_scrollshot_command = [this](ui::ScrollshotCommand command) { OnScrollshotCommand(command); };
        callbacks.on_editor_wheel = [this](int delta, bool control, bool shift, POINT point) {
            if (editor_) { editor_->Wheel(delta, control, shift, point); }
        };
        overlay_ = std::make_unique<ui::OverlayController>(std::move(callbacks));
    }
    overlay_retired_ = false;
}

void CaptureCoordinator::OnWorkspaceEditingChanged(CaptureMode mode, const RECT& region) {
    if (!overlay_ || !overlay_->visible() ||
        (state_ != SessionState::RegionSelecting && state_ != SessionState::ScreenshotEditing &&
         state_ != SessionState::RecordingConfiguring && state_ != SessionState::ScrollshotConfiguring)) { return; }
    if (platform::IsEmptyRect(region)) {
        return;
    }

    if (mode == CaptureMode::Record) {
        if (editor_) { editor_->Hide(); }
        if (scrollshot_controls_) { scrollshot_controls_->Hide(); }
        BeginRecordingConfiguration(region);
        return;
    }
    if (mode == CaptureMode::Scrollshot) {
        if (editor_) { editor_->Hide(); }
        if (recording_controls_) { recording_controls_->Hide(); }
        BeginScrollshotConfiguration(region);
        return;
    }

    if (!RequestTransition(SessionState::ScreenshotEditing)) {
        return;
    }
    if (recording_controls_) { recording_controls_->Hide(); }
    if (scrollshot_controls_) { scrollshot_controls_->Hide(); }
    OpenEditor(region);
}

void CaptureCoordinator::OnWorkspaceRegionChanged(const RECT& region) {
    if (editor_) { editor_->SetRegion(region, overlay_ && overlay_->selector().interacting()); }
    recording_region_ = MakeRecordingRegionEncoderCompatible(region);
    if (recording_controls_) {
        recording_controls_->SetRegion(region);
        if (state_ == SessionState::RecordingConfiguring) { recording_controls_->SetError(RecordingRegionError()); }
    }
    if (state_ == SessionState::ScrollshotConfiguring && scrollshot_controls_) {
        scrollshot_region_ = region;
        scrollshot_controls_->SetFrame(region);
    }
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
    // A display that could not be read would leave a black part in the image.
    // That is a failed capture: the editor says so and the frame stays for
    // another try.
    const bool read = capture_provider_ && capture_provider_->CaptureDesktop(settings_.capture_cursor, final_snapshot);
    const bool complete = read && final_snapshot.RegionComplete(region);
    if (read && !complete) {
        platform::LogMessage(L"A display under the selection could not be read; no screenshot was taken");
    }
    const bool captured = complete && final_snapshot.CropRegion(region, image) && editor_->SetImage(std::move(image));
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
    // Said at once, while the frame can still be made smaller.
    recording_controls_->SetError(RecordingRegionError());
    ScheduleCleanup();
}

void CaptureCoordinator::OnRecordingStart(
    RecordingConfiguration configuration) {
    if (state_ != SessionState::RecordingConfiguring ||
        !IsValidRecordingConfiguration(configuration) || temp_store_ == nullptr) {
        return;
    }

    if (const std::wstring too_large = RecordingRegionError(); !too_large.empty()) {
        if (recording_controls_) { recording_controls_->SetError(too_large); }
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
        OnRecordingStartFailed(L"The recording pipeline could not be created",
                               recording::RecordingStartFailure::Internal);
        return;
    }

    if (!recording_session_->Start()) {
        OnRecordingStartFailed(recording_session_->last_error(), recording_session_->start_failure());
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

void CaptureCoordinator::OnRecordingStartFailed(const std::wstring& message,
                                                recording::RecordingStartFailure failure) {
    platform::LogMessage(L"Recording start failed: %ls", message.c_str());
    if (recording_session_) {
        recording_session_->Abort();
        // What a failed start wrote cannot be played. Its directory goes now,
        // not when Easnap exits.
        const std::optional<media::TemporarySession> session = recording_session_->temporary_session();
        recording_session_.reset();
        if (session.has_value() && temp_store_ && !temp_store_->DeleteSession(*session)) {
            platform::LogMessage(L"The directory of a failed recording start could not be removed yet");
        }
    }
    if (overlay_) { overlay_->Resume(); }
    UINT next_step = IDS_RECORD_START_INTERNAL;
    switch (failure) {
        case recording::RecordingStartFailure::Audio: next_step = IDS_RECORD_START_AUDIO; break;
        case recording::RecordingStartFailure::Encoder: next_step = IDS_RECORD_START_ENCODER; break;
        case recording::RecordingStartFailure::Storage: next_step = IDS_RECORD_START_STORAGE; break;
        case recording::RecordingStartFailure::Capture: next_step = IDS_RECORD_START_CAPTURE; break;
        case recording::RecordingStartFailure::Internal:
        case recording::RecordingStartFailure::None: break;
    }
    if (recording_controls_) {
        recording_controls_->SetError(std::wstring(strings::Get(IDS_RECORD_START_FAILED)) + L" " +
                                      strings::Get(next_step));
        recording_controls_->Activate();
    }
}

std::wstring CaptureCoordinator::RecordingRegionError() const {
    if (RecordingRegionFitsH264(recording_region_)) { return {}; }
    return strings::Format(IDS_RECORD_REGION_TOO_LARGE,
                           {std::to_wstring(platform::RectWidth(recording_region_)),
                            std::to_wstring(platform::RectHeight(recording_region_))});
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
        // From here on the recording is the user's to save or discard. Should
        // Easnap end before that decision, the next start offers it again.
        if (const std::optional<media::TemporarySession> session = recording_session_->temporary_session();
            session.has_value() && temp_store_) {
            static_cast<void>(temp_store_->MarkRecoverable(*session));
        }
        BeginTrimWorkflow(retained_path);
        return;
    }

    platform::LogMessage(L"Recording could not be completed: %ls", recording_session_->last_error().c_str());
    std::wstring message = strings::Get(IDS_RECORDING_UNFINISHED);
    if (!retained_path.empty()) {
        // Not playable as it is, and deleted with the session when Easnap exits.
        message += L"\n\n" + strings::Format(IDS_RECORDING_UNFINISHED_KEPT, {retained_path});
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
        ui::ShowErrorMessage(host_window_, strings::Get(IDS_RECORDING_KEPT_FOR_RECOVERY));
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
    const bool discard = ui::ConfirmDestructive(
        trim_window_ != nullptr ? trim_window_->hwnd() : host_window_,
        strings::Get(IDS_DISCARD_RECORDING_TEXT), strings::Get(IDS_DISCARD_RECORDING_TITLE),
        strings::Get(IDS_DISCARD_CONFIRM), strings::Get(IDS_DISCARD_RECORDING_KEEP));
    if (state_ != SessionState::RecordingTrimming && state_ != SessionState::Exporting) {
        return;  // The export finished and closed the session while the question was open.
    }
    const TrimCloseAction action =
        PlanTrimClose(discard, state_ == SessionState::Exporting);
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

// --- Scrollshot --------------------------------------------------------------

namespace {
/// How long an action waits for the capture worker to reach its barrier.
constexpr auto kScrollshotStopDeadline = std::chrono::milliseconds(3000);
/// More rows of the reduced image than any preview window can show.
constexpr int kScrollshotPreviewRows = 4096;
/// Height kept free under a document for the tools island, its context
/// island and the brush pop-up, in device-independent pixels.
constexpr float kDocumentToolsReserveDip = 176.0f;
}  // namespace

void CaptureCoordinator::EnsureScrollshotControls() {
    if (!scrollshot_controls_) {
        ui::ScrollshotControls::Callbacks callbacks;
        callbacks.on_action = [this](ui::ScrollshotControls::Action action) {
            using Action = ui::ScrollshotControls::Action;
            switch (action) {
                case Action::Start: OnScrollshotAction(ScrollshotAction::Start); break;
                case Action::Help: OnScrollshotAction(ScrollshotAction::Help); break;
                case Action::Edit: OnScrollshotAction(ScrollshotAction::Edit); break;
                case Action::Save: OnScrollshotAction(ScrollshotAction::Save); break;
                case Action::Discard: OnScrollshotAction(ScrollshotAction::Discard); break;
                case Action::Copy: OnScrollshotAction(ScrollshotAction::Copy); break;
            }
        };
        callbacks.on_wheel_blocked = [this] { OnScrollshotCommand(ui::ScrollshotCommand::WheelBlocked); };
        scrollshot_controls_ = std::make_unique<ui::ScrollshotControls>(std::move(callbacks));
    }
    scrollshot_controls_retired_ = false;
}

void CaptureCoordinator::BeginScrollshotConfiguration(const RECT& region) {
    if (!overlay_ || !RequestTransition(SessionState::ScrollshotConfiguring)) {
        return;
    }
    scrollshot_region_ = region;
    scrollshot_notice_ = ScrollshotNotice::None;
    EnsureScrollshotControls();

    if (!scrollshot_controls_->Show(ui::ScrollshotControls::View::Start, region, overlay_->panel_owner_window())) {
        ui::ShowErrorMessage(host_window_, strings::Get(IDS_SCROLLSHOT_START_FAILED));
        RequestTransition(SessionState::Idle);
        EndSession();
    }
}

void CaptureCoordinator::OnScrollshotAction(ScrollshotAction action) {
    // One action at a time: a Save dialog pumps messages while it is open.
    if (!initialized_ || scrollshot_busy_) {
        return;
    }
    switch (action) {
        case ScrollshotAction::Start: StartScrollshotCapture(); break;
        case ScrollshotAction::Help: ShowScrollshotHelp(); break;
        case ScrollshotAction::Edit: EditScrollshot(); break;
        case ScrollshotAction::Save: SaveScrollshot(); break;
        case ScrollshotAction::Discard: DiscardScrollshot(); break;
        case ScrollshotAction::Copy: CopyScrollshot(); break;
    }
}

void CaptureCoordinator::OnScrollshotCommand(ui::ScrollshotCommand command) {
    const bool holds_image = state_ == SessionState::ScrollshotCapturing || state_ == SessionState::ScrollshotReview;
    switch (command) {
        case ui::ScrollshotCommand::Start:
            if (state_ == SessionState::ScrollshotConfiguring) { OnScrollshotAction(ScrollshotAction::Start); }
            break;
        case ui::ScrollshotCommand::Copy:
            if (holds_image) { OnScrollshotAction(ScrollshotAction::Copy); }
            break;
        case ui::ScrollshotCommand::Save:
            if (holds_image) { OnScrollshotAction(ScrollshotAction::Save); }
            break;
        case ui::ScrollshotCommand::Discard:
            if (holds_image) { OnScrollshotAction(ScrollshotAction::Discard); }
            break;
        case ui::ScrollshotCommand::WheelBlocked:
            // The wheel reached Easnap instead of the content under the frame.
            if (state_ == SessionState::ScrollshotCapturing && scrollshot_notice_ == ScrollshotNotice::None) {
                scrollshot_notice_ = ScrollshotNotice::WheelBlocked;
                UpdateScrollshotStatus();
            }
            break;
    }
}

void CaptureCoordinator::StartScrollshotCapture() {
    const bool from_tools = state_ == SessionState::ScreenshotEditing;
    if ((state_ != SessionState::ScrollshotConfiguring && !from_tools) || !overlay_ || !overlay_->visible() ||
        scrollshot_session_) {
        return;
    }
    // The tools island offers this only while its frame is provisional.
    if (from_tools && (!editor_ || editor_->has_image())) {
        return;
    }
    const RECT region = overlay_->selector().selection();
    HWND message_owner = scrollshot_controls_ && scrollshot_controls_->visible() ? scrollshot_controls_->hwnd()
                                                                                 : host_window_;
    if (platform::IsEmptyRect(region)) {
        return;
    }
    // A continuous capture has no hide-and-show fallback. Every Easnap window
    // must already be invisible to capture, or its pixels would be stitched in.
    if (!ui::CanCaptureVisibleWindows()) {
        ui::ShowMessage(message_owner, strings::Get(IDS_SCROLLSHOT_UNSUPPORTED),
                        strings::Get(IDS_SCROLLSHOT_HELP_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }

    scrollshot::ScrollshotSession::Options options;
    options.region = region;
    options.generation = ++scrollshot_generation_;
    options.notify_window = host_window_;
    options.notify_message = kMsgScrollshotProgress;
    options.preview_width = ui::ScrollshotControls::PreviewPixelWidth(region);
    // Windows lets a process duplicate an output once. The workspace's own
    // provider gives its duplication up, or the capture would fall back to GDI.
    snapshot_.Clear();
    if (capture_provider_) { capture_provider_->Release(); }
    // The first picture is taken with the workspace on screen: its surfaces are
    // excluded from capture, and the shading is rearranged so that nothing
    // translucent contains the frame. Neither is visible, so a failure only
    // has to put the shading back.
    overlay_->ExposeScrollshotFrame(true);
    auto session = std::make_unique<scrollshot::ScrollshotSession>(capture::CreateRegionCaptureProvider(), options);
    if (!session->Start()) {
        overlay_->ExposeScrollshotFrame(false);
        platform::LogMessage(L"Scrollshot could not capture its first picture");
        ui::ShowMessage(message_owner, strings::Get(IDS_SCROLLSHOT_START_FAILED),
                        strings::Get(IDS_SCROLLSHOT_HELP_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }
    if (!RequestTransition(SessionState::ScrollshotCapturing)) {
        overlay_->ExposeScrollshotFrame(false);
        return;  // The session object stops its worker as it goes out of scope.
    }
    scrollshot_session_ = std::move(session);
    scrollshot_region_ = region;
    scrollshot_image_ = {};
    scrollshot_height_ = 0;
    // Until the first click inside the frame, a wheel that follows the focus
    // would go to Easnap. Say so before the user wonders why nothing moves.
    scrollshot_notice_ = platform::WheelFollowsFocus() ? ScrollshotNotice::WheelBlocked : ScrollshotNotice::None;

    if (editor_) { editor_->Hide(); }
    if (recording_controls_) { recording_controls_->Hide(); }
    overlay_->BeginScrollshotCapture();

    // Reached from the tools island, the Start island was never shown.
    EnsureScrollshotControls();
    if (!scrollshot_controls_->Show(ui::ScrollshotControls::View::Capture, region, overlay_->panel_owner_window())) {
        ui::ShowErrorMessage(host_window_, strings::Get(IDS_SCROLLSHOT_START_FAILED));
        RequestTransition(SessionState::Idle);
        EndSession();
        return;
    }
    OnScrollshotProgress(scrollshot_generation_);
    scrollshot_controls_->Activate();
}

void CaptureCoordinator::OnScrollshotProgress(uint64_t generation) {
    if (!initialized_ || generation != scrollshot_generation_ || !scrollshot_session_ ||
        state_ != SessionState::ScrollshotCapturing) {
        return;  // A message from a capture that has already ended.
    }
    const scrollshot::ScrollshotProgress progress = scrollshot_session_->progress();
    // Rows were added, so the wheel reaches the content after all.
    if (scrollshot_notice_ == ScrollshotNotice::WheelBlocked && scrollshot_height_ > 0 &&
        progress.height > scrollshot_height_) {
        scrollshot_notice_ = ScrollshotNotice::None;
    }
    scrollshot_height_ = progress.height;
    if (scrollshot_controls_) {
        scrollshot::ScrollshotPreview preview;
        scrollshot_session_->CopyPreview(kScrollshotPreviewRows, preview);
        scrollshot_controls_->SetPreview(preview);
    }
    UpdateScrollshotStatus();
    if (progress.finished) {
        // The worker ended on its own. Keep what it stitched.
        static_cast<void>(StopScrollshotCapture());
    }
}

void CaptureCoordinator::UpdateScrollshotStatus() {
    if (!scrollshot_controls_ || scrollshot_controls_->view() != ui::ScrollshotControls::View::Capture) {
        return;
    }
    const bool capturing = state_ == SessionState::ScrollshotCapturing && scrollshot_session_ != nullptr;
    int width = scrollshot_image_.width;
    int height = scrollshot_image_.height;
    UINT message = capturing ? IDS_SCROLLSHOT_STATUS_SCROLL : IDS_SCROLLSHOT_STATUS_READY;
    bool warning = false;
    bool following = true;
    if (capturing) {
        const scrollshot::ScrollshotProgress progress = scrollshot_session_->progress();
        width = progress.width;
        height = progress.height;
        following = progress.condition == scrollshot::ScrollshotCondition::Following;
        switch (progress.condition) {
            case scrollshot::ScrollshotCondition::Lost:
                message = IDS_SCROLLSHOT_STATUS_LOST;
                warning = true;
                break;
            case scrollshot::ScrollshotCondition::AtLimit:
                message = IDS_SCROLLSHOT_STATUS_LIMIT;
                break;
            case scrollshot::ScrollshotCondition::CaptureFailed:
                message = IDS_SCROLLSHOT_STATUS_CAPTURE_FAILED;
                warning = true;
                break;
            case scrollshot::ScrollshotCondition::Following:
                break;
        }
    }
    switch (scrollshot_notice_) {
        case ScrollshotNotice::WheelBlocked:
            if (capturing && following) { message = IDS_SCROLLSHOT_STATUS_WHEEL; }
            break;
        case ScrollshotNotice::DisplayChanged: message = IDS_SCROLLSHOT_STATUS_DISPLAY_CHANGED; break;
        case ScrollshotNotice::Stopping: message = IDS_SCROLLSHOT_STATUS_STOPPING; warning = true; break;
        case ScrollshotNotice::Copying: message = IDS_SCROLLSHOT_STATUS_COPYING; break;
        case ScrollshotNotice::Saving: message = IDS_SCROLLSHOT_STATUS_SAVING; break;
        case ScrollshotNotice::CopyFailed: message = IDS_SCROLLSHOT_STATUS_COPY_FAILED; warning = true; break;
        case ScrollshotNotice::SaveFailed: message = IDS_SCROLLSHOT_STATUS_SAVE_FAILED; warning = true; break;
        case ScrollshotNotice::None: break;
    }
    scrollshot_controls_->SetStatus(
        strings::Format(IDS_SCROLLSHOT_SIZE, {std::to_wstring(width), std::to_wstring(height)}),
        strings::Get(message), warning);
}

void CaptureCoordinator::ShowScrollshotNotice(ScrollshotNotice notice) {
    scrollshot_notice_ = notice;
    UpdateScrollshotStatus();
    if (scrollshot_controls_ && scrollshot_controls_->hwnd() != nullptr) {
        ::RedrawWindow(scrollshot_controls_->hwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
}

bool CaptureCoordinator::StopScrollshotCapture() {
    if (state_ == SessionState::ScrollshotReview) {
        return true;
    }
    if (state_ != SessionState::ScrollshotCapturing || !scrollshot_session_) {
        return false;
    }
    if (!scrollshot_session_->Stop(kScrollshotStopDeadline)) {
        // The worker is still inside a capture call. The request stays; the
        // next action tries again.
        scrollshot_notice_ = ScrollshotNotice::Stopping;
        UpdateScrollshotStatus();
        return false;
    }
    if (scrollshot_controls_) {
        scrollshot::ScrollshotPreview preview;
        scrollshot_session_->CopyPreview(kScrollshotPreviewRows, preview);
        scrollshot_controls_->SetPreview(preview);
    }
    const uint32_t fallback_frames = scrollshot_session_->progress().fallback_frames;
    scrollshot_image_ = scrollshot_session_->TakeResult();
    scrollshot_session_.reset();
    if (scrollshot_notice_ == ScrollshotNotice::Stopping || scrollshot_notice_ == ScrollshotNotice::WheelBlocked) {
        scrollshot_notice_ = ScrollshotNotice::None;
    }
    RequestTransition(SessionState::ScrollshotReview);
    if (overlay_) { overlay_->StopScrollshotCapture(); }
    if (scrollshot_image_.empty()) {
        // Only memory exhaustion while composing the result leads here.
        platform::LogMessage(L"Scrollshot stopped without an image");
        ui::ShowErrorMessage(host_window_, strings::Get(IDS_SCROLLSHOT_START_FAILED));
        RequestTransition(SessionState::Idle);
        EndSession();
        return false;
    }
    platform::LogMessage(L"Scrollshot stopped: %d x %d", scrollshot_image_.width, scrollshot_image_.height);
    if (fallback_frames != 0) {
        platform::LogMessage(L"Scrollshot read %u of its pictures through the GDI fallback", fallback_frames);
    }
    UpdateScrollshotStatus();
    return true;
}

void CaptureCoordinator::CopyScrollshot() {
    if (state_ != SessionState::ScrollshotCapturing && state_ != SessionState::ScrollshotReview) {
        return;
    }
    scrollshot_busy_ = true;
    if (scrollshot_controls_) { scrollshot_controls_->SetBusy(true); }
    const HCURSOR previous_cursor = ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
    bool copied = false;
    if (StopScrollshotCapture()) {
        // Encoding a tall image takes a moment on this thread.
        ShowScrollshotNotice(ScrollshotNotice::Copying);
        // A very tall image can exceed what the clipboard accepts. The image
        // stays, and the status points to Save.
        copied = platform::CopyImageToClipboard(host_window_, scrollshot_image_);
        scrollshot_notice_ = copied ? ScrollshotNotice::None : ScrollshotNotice::CopyFailed;
    }
    ::SetCursor(previous_cursor);
    scrollshot_busy_ = false;
    if (copied) {
        platform::LogMessage(L"Scrollshot copied to the clipboard");
        ui::ShowCaptureCopiedNotice(scrollshot_region_);
        RequestTransition(SessionState::Idle);
        EndSession();
        return;
    }
    if (IsScrollshotState(state_)) {
        if (scrollshot_controls_) { scrollshot_controls_->SetBusy(false); }
        UpdateScrollshotStatus();
    }
}

void CaptureCoordinator::SaveScrollshot() {
    if (state_ != SessionState::ScrollshotCapturing && state_ != SessionState::ScrollshotReview) {
        return;
    }
    scrollshot_busy_ = true;
    if (!StopScrollshotCapture()) {
        scrollshot_busy_ = false;
        return;
    }
    // The preview is topmost and must not cover the system dialog.
    if (scrollshot_controls_ && scrollshot_controls_->preview_hwnd() != nullptr) {
        ::ShowWindow(scrollshot_controls_->preview_hwnd(), SW_HIDE);
    }
    HWND owner = scrollshot_controls_ && scrollshot_controls_->hwnd() != nullptr ? scrollshot_controls_->hwnd()
                                                                                 : host_window_;
    const auto destination = platform::ShowSaveImageDialog(
        owner, platform::TimestampedFileName(L"Easnap", L"png"), settings_.last_save_directory);
    scrollshot_busy_ = false;
    if (!initialized_ || state_ != SessionState::ScrollshotReview) {
        return;  // The session ended while the dialog was open.
    }
    if (scrollshot_controls_) { scrollshot_controls_->SetFrame(scrollshot_region_); }
    if (!destination.has_value()) {
        return;  // Cancelled: the island and the image stay.
    }
    const HCURSOR previous_cursor = ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
    ShowScrollshotNotice(ScrollshotNotice::Saving);
    const bool saved = platform::SaveImage(scrollshot_image_, *destination);
    ::SetCursor(previous_cursor);
    if (!saved) {
        scrollshot_notice_ = ScrollshotNotice::SaveFailed;
        UpdateScrollshotStatus();
        return;
    }
    settings_.last_save_directory = platform::DirectoryOf(*destination);
    platform::LogMessage(L"Scrollshot saved");
    RequestTransition(SessionState::Idle);
    EndSession();
}

void CaptureCoordinator::EditScrollshot() {
    if (state_ != SessionState::ScrollshotCapturing && state_ != SessionState::ScrollshotReview) {
        return;
    }
    scrollshot_busy_ = true;
    const bool stopped = StopScrollshotCapture();
    scrollshot_busy_ = false;
    if (!stopped) {
        return;
    }

    // The picture may use the work area of the frame's monitor, less a margin
    // and the room the tool islands need below it.
    const platform::MonitorInfo monitor = platform::MonitorForRect(scrollshot_region_);
    const float scale = monitor.scale();
    const LONG margin = ui::theme::Scaled(12, scale);
    RECT area{monitor.work_area.left + margin, monitor.work_area.top + margin, monitor.work_area.right - margin,
              monitor.work_area.bottom - ui::theme::Scaled(kDocumentToolsReserveDip, scale)};
    if (platform::RectWidth(area) < 2 * margin || platform::RectHeight(area) < 2 * margin) {
        area = monitor.work_area;
    }

    // After a display change the workspace windows describe monitors that no
    // longer exist. Their owned islands go first, then the workspace is rebuilt.
    const bool rebuild = !overlay_ || !overlay_->visible();
    if (rebuild) {
        if (editor_) { editor_->Destroy(); }
        editor_.reset();
        editor_retired_ = false;
        if (scrollshot_controls_) { scrollshot_controls_->Destroy(); }
        scrollshot_controls_.reset();
        scrollshot_controls_retired_ = false;
        EnsureOverlay();
        session_monitors_ = platform::EnumerateMonitors();
        const POINT centre{(monitor.bounds.left + monitor.bounds.right) / 2,
                           (monitor.bounds.top + monitor.bounds.bottom) / 2};
        if (!overlay_->Show(capture::DesktopSnapshot{}, centre)) {
            // Without a workspace the image is still safe in the island.
            ui::ShowErrorMessage(host_window_, strings::Get(IDS_SCROLLSHOT_START_FAILED));
            EnsureScrollshotControls();
            if (scrollshot_controls_->Show(ui::ScrollshotControls::View::Capture, scrollshot_region_, nullptr)) {
                UpdateScrollshotStatus();
            }
            return;
        }
    }

    std::unique_ptr<editor::ScreenshotEditor> editor = CreateEditor();
    if (!editor->OpenDocument(scrollshot_image_, area, scrollshot_region_.left, &settings_,
                              overlay_->panel_owner_window())) {
        // The editor takes the image only when it opened: nothing is lost.
        ui::ShowErrorMessage(scrollshot_controls_ ? scrollshot_controls_->hwnd() : host_window_,
                             L"Easnap could not open the screenshot tools.");
        if (rebuild) {
            EnsureScrollshotControls();
            if (scrollshot_controls_->Show(ui::ScrollshotControls::View::Capture, scrollshot_region_,
                                           overlay_->panel_owner_window())) {
                UpdateScrollshotStatus();
            }
        }
        return;
    }
    if (!RequestTransition(SessionState::ScreenshotEditing)) {
        scrollshot_image_ = editor->TakeImage();
        return;
    }
    if (scrollshot_controls_) { scrollshot_controls_->Hide(); }
    editor_ = std::move(editor);
    editor_retired_ = false;
    scrollshot_notice_ = ScrollshotNotice::None;
    const editor::ImageView view = editor_->view();
    overlay_->BeginDocumentView(view.viewport);
    PublishDocumentView(view);
    // The workspace takes the keyboard and the wheel: scrolling, zooming and
    // the editor's shortcuts all arrive through it.
    ::SetForegroundWindow(overlay_->owner_window());
    ::SetFocus(overlay_->owner_window());
}

void CaptureCoordinator::DiscardScrollshot() {
    if (!IsScrollshotState(state_)) {
        return;
    }
    // More than the first picture means the user has scrolled for it.
    const int height = scrollshot_session_ ? scrollshot_height_ : scrollshot_image_.height;
    if (state_ != SessionState::ScrollshotConfiguring && height > platform::RectHeight(scrollshot_region_)) {
        scrollshot_busy_ = true;
        const bool discard = ConfirmDiscardImage(DialogOwner(), true);
        scrollshot_busy_ = false;
        if (!discard || !IsScrollshotState(state_)) {
            return;
        }
    }
    RequestTransition(SessionState::Idle);
    EndSession();
}

void CaptureCoordinator::ShowScrollshotHelp() {
    std::wstring text = strings::Get(IDS_SCROLLSHOT_HELP_BODY);
    if (platform::WheelFollowsFocus()) {
        text += strings::Get(IDS_SCROLLSHOT_HELP_WHEEL);
    }
    HWND owner = scrollshot_controls_ && scrollshot_controls_->hwnd() != nullptr ? scrollshot_controls_->hwnd()
                                                                                 : host_window_;
    ui::ShowMessage(owner, text.c_str(), strings::Get(IDS_SCROLLSHOT_HELP_TITLE), MB_OK | MB_ICONINFORMATION);
}

void CaptureCoordinator::StopScrollshotForShutdown() {
    // Destroying the session stops and joins its worker.
    scrollshot_session_.reset();
    scrollshot_image_ = {};
}

void CaptureCoordinator::OpenEditor(const RECT& region) {
    if (editor_ && editor_->hwnd()) {
        editor_->SetRegion(region);
        editor_->Show();
        return;
    }
    editor_ = CreateEditor();
    if (!editor_->Open(region, &settings_, overlay_->panel_owner_window())) {
        ui::ShowErrorMessage(host_window_, L"Easnap could not open the screenshot tools.");
        OnOverlayCancelled();
    }
}

std::unique_ptr<editor::ScreenshotEditor> CaptureCoordinator::CreateEditor() {
    editor::ScreenshotEditor::Callbacks callbacks;
    callbacks.on_appearance = [this](AppearanceTheme value) { SetAppearanceTheme(value); };
    callbacks.on_closed = [this](bool exported) { OnEditorClosed(exported); };
    callbacks.on_confirm_discard = [this] {
        return ConfirmDiscardImage(DialogOwner(), editor_ && editor_->document_mode());
    };
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
    callbacks.on_scrollshot = [this] { OnScrollshotAction(ScrollshotAction::Start); };
    callbacks.on_view_changed = [this](const editor::ImageView& view) { PublishDocumentView(view); };
    return std::make_unique<editor::ScreenshotEditor>(std::move(callbacks));
}

void CaptureCoordinator::PublishDocumentView(const editor::ImageView& view) {
    if (!overlay_ || !overlay_->document_view()) {
        return;
    }
    const int percent = static_cast<int>(view.zoom * 100.0f + 0.5f);
    overlay_->SetDocumentView(
        view.viewport, view.top_fraction(), view.visible_fraction(),
        strings::Format(IDS_DOCUMENT_VIEW_LABEL, {std::to_wstring(view.image_width),
                                                  std::to_wstring(view.image_height), std::to_wstring(percent)}));
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
    if (state_ == SessionState::ScrollshotCapturing || state_ == SessionState::ScrollshotReview) {
        if (SameDisplayLayout(session_monitors_, platform::EnumerateMonitors())) {
            return;
        }
        // The frame no longer describes a place on the desktop, but what was
        // stitched is the user's work. Stop capturing and keep the image; the
        // island moves to a monitor that exists and all four actions remain.
        platform::LogMessage(L"Display layout changed during a Scrollshot - capture stopped, image kept");
        if (state_ == SessionState::ScrollshotCapturing && !StopScrollshotCapture()) {
            return;  // Still stopping, or the session ended without an image.
        }
        session_monitors_ = platform::EnumerateMonitors();
        scrollshot_notice_ = ScrollshotNotice::DisplayChanged;
        if (overlay_) { overlay_->Hide(); }
        if (scrollshot_controls_) {
            scrollshot_controls_->SetFrame(scrollshot_region_);
            UpdateScrollshotStatus();
            scrollshot_controls_->Activate();
        }
        return;
    }
    if (state_ == SessionState::ScreenshotEditing && editor_ && editor_->document_mode()) {
        if (SameDisplayLayout(session_monitors_, platform::EnumerateMonitors())) {
            return;
        }
        // A stitched image does not depend on the desktop it was taken from.
        // Its annotations are drawn into it and it goes back to review, where
        // it can be copied, saved or opened for editing again.
        platform::LogMessage(L"Display layout changed while editing a Scrollshot - image kept");
        platform::BgraImage image = editor_->CompositeCommitted();
        if (image.empty()) { image = editor_->TakeImage(); }
        editor_->Hide();
        editor_retired_ = true;
        if (image.empty() || !RequestTransition(SessionState::ScrollshotReview)) {
            OnOverlayCancelled();
            return;
        }
        scrollshot_image_ = std::move(image);
        session_monitors_ = platform::EnumerateMonitors();
        scrollshot_notice_ = ScrollshotNotice::DisplayChanged;
        if (overlay_) { overlay_->Hide(); }
        EnsureScrollshotControls();
        if (scrollshot_controls_->Show(ui::ScrollshotControls::View::Capture, scrollshot_region_,
                                       overlay_ ? overlay_->panel_owner_window() : nullptr)) {
            UpdateScrollshotStatus();
            scrollshot_controls_->Activate();
        }
        ScheduleCleanup();
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
    // A dialog of one of these windows may be running its own message loop,
    // with a frame of that window's code below it on the stack. Destroying the
    // window now would pull it away under that frame; try again shortly.
    if (platform::ModalLoopActive() || ui::PopupActive()) {
        if (host_window_ != nullptr) { ::SetTimer(host_window_, kTimerDeferredCleanup, 100, nullptr); }
        return;
    }
    if (host_window_ != nullptr) { ::KillTimer(host_window_, kTimerDeferredCleanup); }
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
    if (scrollshot_controls_retired_) {
        scrollshot_controls_retired_ = false;
        if (scrollshot_controls_) {
            scrollshot_controls_->Destroy();
            scrollshot_controls_.reset();
        }
    }
    // The editor, the configuring recording island and the Scrollshot island
    // share this owner. Active recording has already detached it in RetireWorkspace.
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
    if (scrollshot_controls_) {
        scrollshot_controls_->Hide();
        scrollshot_controls_retired_ = true;
    }
    // Ending the session ends any capture still running and drops its image.
    scrollshot_session_.reset();
    scrollshot_image_ = {};
    scrollshot_notice_ = ScrollshotNotice::None;
    scrollshot_busy_ = false;
    if (trim_window_) {
        trim_window_->Hide();
        trim_window_retired_ = true;
    }
    ScheduleCleanup();
}

bool CaptureCoordinator::ProcessDialogMessage(MSG& message) const {
    if (editor_ && editor_->ProcessDialogMessage(message)) { return true; }
    if (scrollshot_controls_ && scrollshot_controls_->ProcessDialogMessage(message)) { return true; }
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
