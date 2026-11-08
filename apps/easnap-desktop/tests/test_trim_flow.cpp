// Easnap integration tests - the trim flow of a finished recording (ADR-039).
//
// The production TrimFlow with its production window, export controller, MP4
// exporter, completion controller and temporary session store, driven with
// window messages on a private desktop. A real H.264 recording is written into
// a real temporary session for every case. Only the last step of a Copy is not
// real: the completion backend records the file it would put on the Shell
// clipboard instead of replacing the clipboard of the person using the PC;
// clipboard publication itself is covered by recording_completion_workflow.
//
// Save is not covered: its file dialog cannot be driven by messages.

#include "preview_test_fixture.h"
#include "native_test_monitor.h"

#include "app/recording_completion.h"
#include "app/session_transitions.h"
#include "app/trim_flow.h"
#include "media/temp_session.h"
#include "platform/platform_utils.h"
#include "strings/strings.h"
#include "ui/native_controls.h"

#include <commctrl.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace easnap;

int passed{}, failed{};
#define CHECK(...)                                                         \
    do {                                                                   \
        if (__VA_ARGS__) {                                                 \
            ++passed;                                                      \
        } else {                                                           \
            ++failed;                                                      \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); \
        }                                                                  \
    } while (false)

// Control identifiers of the trim window (ui/trim_window.cpp).
constexpr int kIdCancelExport = 103;
constexpr int kIdCopy = 104;
constexpr UINT kMsgCleanup = WM_APP + 40;

/// The Windows backend for files; the clipboard is recorded, not replaced.
class RecordedClipboardBackend final : public app::IRecordingCompletionBackend {
public:
    explicit RecordedClipboardBackend(std::vector<std::wstring>& clipboard)
        : files_(app::CreateRecordingCompletionBackend()), clipboard_(clipboard) {}

    bool PersistArtifact(const std::wstring& source_path, const std::wstring& destination_path,
                         media::RecordingFormat format, bool replace_existing) override {
        return files_->PersistArtifact(source_path, destination_path, format, replace_existing);
    }
    bool MoveArtifact(const std::wstring& source_path, const std::wstring& destination_path,
                      media::RecordingFormat format) override {
        return files_->MoveArtifact(source_path, destination_path, format);
    }
    bool ApplyClipboardSecurity(const std::wstring& path) override { return files_->ApplyClipboardSecurity(path); }
    [[nodiscard]] bool IsNonemptyArtifact(const std::wstring& path, media::RecordingFormat format) const override {
        return files_->IsNonemptyArtifact(path, format);
    }
    bool PutFileOnClipboard(HWND, const std::wstring& path) override {
        clipboard_ = {path};
        return true;
    }
    [[nodiscard]] std::optional<std::vector<std::wstring>> ClipboardFilePaths() const override { return clipboard_; }

private:
    std::unique_ptr<app::IRecordingCompletionBackend> files_;
    std::vector<std::wstring>& clipboard_;
};

void Pump(DWORD milliseconds) {
    const ULONGLONG until = ::GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::Sleep(5);
    } while (::GetTickCount64() < until);
}

bool WaitFor(const std::function<bool()>& condition, DWORD timeout_ms) {
    const ULONGLONG until = ::GetTickCount64() + timeout_ms;
    while (!condition()) {
        if (::GetTickCount64() >= until) { return false; }
        Pump(10);
    }
    return true;
}

HWND FindDialog() {
    HWND dialog = nullptr;
    ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        wchar_t name[32]{};
        ::GetClassNameW(window, name, ARRAYSIZE(name));
        if (::lstrcmpW(name, L"#32770") == 0 && ::IsWindowVisible(window)) {
            *reinterpret_cast<HWND*>(value) = window;
            return FALSE;
        }
        return TRUE; }, reinterpret_cast<LPARAM>(&dialog));
    return dialog;
}

/// Answers the next question with the button `answer`, from inside its loop.
int g_answer = 0;
int g_dialogs_answered = 0;
void AnswerNextDialog(int answer) {
    g_answer = answer;
    ::SetTimer(nullptr, 0, 20, [](HWND, UINT, UINT_PTR id, DWORD) {
        HWND dialog = FindDialog();
        if (dialog == nullptr) { return; }  // Not shown yet: the timer fires again.
        ::KillTimer(nullptr, id);
        ++g_dialogs_answered;
        if (HWND button = ::GetDlgItem(dialog, g_answer)) { ::SendMessageW(button, BM_CLICK, 0, 0); }
    });
}

void Click(HWND window, int id) {
    HWND button = ::GetDlgItem(window, id);
    CHECK(button != nullptr && ::IsWindowEnabled(button) != FALSE);
    if (button != nullptr) { ::SendMessageW(button, BM_CLICK, 0, 0); }
}

/// Moves the Out point one second earlier, through the timeline's keyboard.
void ShortenByOneSecond(HWND window) {
    HWND timeline = ::FindWindowExW(window, nullptr, L"EasnapTrimTimeline", nullptr);
    CHECK(timeline != nullptr);
    ::SendMessageW(timeline, WM_KEYDOWN, VK_TAB, 0);   // From the playhead to Out.
    ::SendMessageW(timeline, WM_KEYDOWN, VK_LEFT, 0);  // One second earlier.
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// What the coordinator does around the flow, as small as it can be.
struct Harness {
    app::SessionStateMachine machine;
    app::AppSettings settings{};
    std::unique_ptr<media::ITemporarySessionStore> store;
    std::vector<std::wstring> clipboard;
    std::unique_ptr<app::RecordingCompletionController> completion;
    std::unique_ptr<app::TrimFlow> flow;
    HWND host{nullptr};
    int sessions_ended{0};
    int recordings_released{0};
    int settings_persisted{0};

    static LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Harness*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self != nullptr && self->flow) {
            if (message == app::kMsgTrimExportCompleted) {
                self->flow->OnExportCompleted(static_cast<uint64_t>(wparam));
                return 0;
            }
            if (message == kMsgCleanup) {
                self->flow->DestroyRetired();
                return 0;
            }
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    bool Create() {
        WNDCLASSW definition{};
        definition.lpfnWndProc = HostProc;
        definition.hInstance = ::GetModuleHandleW(nullptr);
        definition.lpszClassName = L"EasnapTrimFlowTestHost";
        ::RegisterClassW(&definition);
        host = ::CreateWindowExW(0, definition.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                 definition.hInstance, nullptr);
        if (host == nullptr) { return false; }
        ::SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        store = media::CreateTemporarySessionStore();
        if (!store || store->root_directory().empty()) { return false; }
        completion = std::make_unique<app::RecordingCompletionController>(
            *store, std::make_unique<RecordedClipboardBackend>(clipboard));

        app::TrimFlow::Services services;
        services.machine = &machine;
        services.settings = &settings;
        services.host_window = host;
        services.completion = completion.get();
        services.store = store.get();
        services.end_session = [this] {
            ++sessions_ended;
            flow->RetireWindow();
            ::PostMessageW(host, kMsgCleanup, 0, 0);
        };
        services.schedule_cleanup = [this] { ::PostMessageW(host, kMsgCleanup, 0, 0); };
        services.persist_settings = [this] {
            ++settings_persisted;
            return true;
        };
        services.release_recording = [this] { ++recordings_released; };
        flow = std::make_unique<app::TrimFlow>(std::move(services));
        return true;
    }

    /// The other application's clipboard content replaces the recording.
    void ClipboardReplaced() {
        clipboard.clear();
        completion->OnClipboardChanged();
    }

    void Destroy() {
        flow.reset();
        if (completion) { completion->Shutdown(); }
        completion.reset();
        store.reset();
        if (host != nullptr) { ::DestroyWindow(host); }
    }
};

/// One finished recording in its temporary session, with the trim view open.
struct Recording {
    media::TemporarySession session;
    HWND window{nullptr};
};

std::optional<Recording> OpenTrim(Harness& harness) {
    const std::optional<media::TemporarySession> session = harness.store->CreateSession();
    CHECK(session.has_value());
    if (!session.has_value()) { return std::nullopt; }
    CHECK(test::CreateVideoPreviewSource(session->recording_path()));

    // Through the recording to RecordingTrimming, as a capture session goes.
    app::SessionStateMachine& machine = harness.machine;
    CHECK(machine.state() == app::SessionState::Idle);
    CHECK(machine.RequestTransition(app::SessionState::RegionSelecting));
    CHECK(machine.RequestTransition(app::SessionState::RecordingConfiguring));
    CHECK(machine.RequestTransition(app::SessionState::RecordingActive));
    CHECK(machine.RequestTransition(app::SessionState::RecordingFinalizing));
    CHECK(machine.RequestTransition(app::SessionState::RecordingTrimming));

    media::RecordingOptions options;
    options.region = RECT{0, 0, 640, 360};
    options.format = media::RecordingFormat::Mp4;
    options.audio = media::AudioSource::None;
    options.video_bitrate_bps = 4'000'000;
    harness.flow->Begin(session->recording_path(), options, test::OnTestMonitor({100, 100, 740, 460}), *session);

    Recording recording{*session, harness.flow->visible_window()};
    CHECK(recording.window != nullptr);
    if (recording.window == nullptr) { return std::nullopt; }
    // Save and Copy are enabled once the preview has read the recording.
    const bool ready = WaitFor([&] { return ::IsWindowEnabled(::GetDlgItem(recording.window, kIdCopy)) != FALSE; }, 15'000);
    CHECK(ready);
    if (!ready) {
        std::fprintf(stderr, "The preview did not become ready on this desktop\n");
        return std::nullopt;
    }
    return recording;
}

/// The window is retired through the session's end and then destroyed.
void ExpectClosed(Harness& harness, HWND window) {
    CHECK(harness.machine.state() == app::SessionState::Idle);
    CHECK(!harness.flow->window_visible());
    CHECK(WaitFor([&] { return ::IsWindow(window) == FALSE; }, 2000));
}

// --- Cases -----------------------------------------------------------------------

void CopyWholeRecording(Harness& harness) {
    std::puts("Copy of the whole recording");
    const int ended = harness.sessions_ended;
    const auto recording = OpenTrim(harness);
    if (!recording) { return; }
    Click(recording->window, kIdCopy);
    // Nothing to export: the copy is made at once from the recording itself.
    ExpectClosed(harness, recording->window);
    CHECK(harness.sessions_ended == ended + 1);
    CHECK(harness.recordings_released == ended + 1);
    CHECK(harness.clipboard.size() == 1);
    if (harness.clipboard.size() == 1) {
        const std::wstring copied = harness.clipboard.front();
        CHECK(!app::ExistingFileDataPath(copied).empty());
        CHECK(copied != recording->session.recording_path());
        CHECK(std::filesystem::path(copied).extension() == L".mp4");
    }
    // The recording's own session is gone; the copy lives in another one.
    CHECK(!DirectoryExists(recording->session.directory));
    harness.ClipboardReplaced();
}

void CopyPartOfRecording(Harness& harness) {
    std::puts("Copy of a part of the recording");
    const int ended = harness.sessions_ended;
    const auto recording = OpenTrim(harness);
    if (!recording) { return; }
    ShortenByOneSecond(recording->window);
    const ULONGLONG started = ::GetTickCount64();
    Click(recording->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    CHECK(WaitFor([&] { return harness.machine.state() == app::SessionState::Idle; }, 60'000));
    std::printf("  export and copy took %llu ms\n", ::GetTickCount64() - started);
    ExpectClosed(harness, recording->window);
    CHECK(harness.sessions_ended == ended + 1);
    CHECK(harness.clipboard.size() == 1);
    if (harness.clipboard.size() == 1) {
        CHECK(!app::ExistingFileDataPath(harness.clipboard.front()).empty());
        // The trimmed file, not the recording: it is a second shorter.
        std::error_code error;
        const auto copied = std::filesystem::file_size(harness.clipboard.front(), error);
        CHECK(!error && copied > 0);
    }
    CHECK(!DirectoryExists(recording->session.directory));
    harness.ClipboardReplaced();
}

void CancelExport(Harness& harness) {
    std::puts("Cancelling an export, then discarding");
    const int ended = harness.sessions_ended;
    const auto recording = OpenTrim(harness);
    if (!recording) { return; }
    ShortenByOneSecond(recording->window);
    Click(recording->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    Click(recording->window, kIdCancelExport);
    // The export stops; the recording and its window stay for another try.
    CHECK(WaitFor([&] { return harness.machine.state() == app::SessionState::RecordingTrimming; }, 30'000));
    Pump(100);
    CHECK(harness.flow->window_visible());
    CHECK(harness.sessions_ended == ended);
    CHECK(harness.clipboard.empty());
    CHECK(::IsWindowEnabled(::GetDlgItem(recording->window, kIdCopy)) != FALSE);
    CHECK(app::ExistingFileDataPath(recording->session.recording_path()) == recording->session.recording_path());

    // Close, and Discard in the question: the recording is deleted.
    AnswerNextDialog(IDYES);
    Click(recording->window, IDCANCEL);
    CHECK(g_dialogs_answered > 0);
    ExpectClosed(harness, recording->window);
    CHECK(harness.sessions_ended == ended + 1);
    CHECK(harness.clipboard.empty());
    CHECK(!DirectoryExists(recording->session.directory));
}

void KeepWhenClosing(Harness& harness) {
    std::puts("Closing and keeping the recording");
    const int ended = harness.sessions_ended;
    const auto recording = OpenTrim(harness);
    if (!recording) { return; }
    const int answered = g_dialogs_answered;
    AnswerNextDialog(IDNO);
    Click(recording->window, IDCANCEL);
    CHECK(g_dialogs_answered == answered + 1);
    Pump(100);
    CHECK(harness.machine.state() == app::SessionState::RecordingTrimming);
    CHECK(harness.flow->window_visible());
    CHECK(harness.sessions_ended == ended);
    CHECK(DirectoryExists(recording->session.directory));

    // Copy of the whole recording ends it, so the next case starts from Idle.
    Click(recording->window, kIdCopy);
    ExpectClosed(harness, recording->window);
    harness.ClipboardReplaced();
}

void DiscardDuringExport(Harness& harness) {
    std::puts("Closing and discarding during an export");
    const int ended = harness.sessions_ended;
    const auto recording = OpenTrim(harness);
    if (!recording) { return; }
    ShortenByOneSecond(recording->window);
    Click(recording->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    AnswerNextDialog(IDYES);
    Click(recording->window, IDCANCEL);
    // The export is cancelled first; its completion then discards.
    CHECK(WaitFor([&] { return harness.machine.state() == app::SessionState::Idle; }, 30'000));
    ExpectClosed(harness, recording->window);
    CHECK(harness.sessions_ended == ended + 1);
    CHECK(harness.clipboard.empty());
    CHECK(!DirectoryExists(recording->session.directory));
}

void DestroyDuringExport(Harness& harness) {
    std::puts("Destroying the flow during an export");
    const int ended = harness.sessions_ended;
    const auto recording = OpenTrim(harness);
    if (!recording) { return; }
    ShortenByOneSecond(recording->window);
    Click(recording->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    // As at the end of the application: the worker is stopped and joined
    // before the window and the controller go.
    harness.flow->Destroy();
    CHECK(::IsWindow(recording->window) == FALSE);
    CHECK(!harness.flow->window_visible());
    CHECK(harness.flow->visible_window() == nullptr);
    // A completion already posted by the worker finds nothing to complete.
    Pump(200);
    CHECK(harness.sessions_ended == ended);
    CHECK(harness.clipboard.empty());
    CHECK(DirectoryExists(recording->session.directory));  // Kept: nobody decided.
    harness.machine.Reset();
}

}  // namespace

int main() {
    easnap::strings::SetPreference(easnap::strings::LanguagePreference::English);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    ::InitCommonControlsEx(&controls);

    Harness harness;
    CHECK(harness.Create());
    if (failed == 0) {
        CopyWholeRecording(harness);
        CopyPartOfRecording(harness);
        CancelExport(harness);
        KeepWhenClosing(harness);
        DiscardDuringExport(harness);
        DestroyDuringExport(harness);
    }
    harness.Destroy();
    ::CoUninitialize();
    std::printf("trim flow: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
