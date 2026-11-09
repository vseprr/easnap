// Easnap integration tests - the trim flow of a finished recording (ADR-039)
// and of a video file the user opened (ADR-040).
//
// The production TrimFlow with its production window, export controller, MP4
// and GIF exporters, completion controller and temporary session store, driven
// with window messages on a private desktop. A real H.264 recording is written
// into a real temporary session for every recording case; an opened video is
// a real H.264/AAC file in a test folder, checked by the production probe.
// Only the last step of a Copy is not real: the completion backend records
// the file it would put on the Shell clipboard instead of replacing the
// clipboard of the person using the PC; clipboard publication itself is
// covered by recording_completion_workflow. The flow's calls to the store and
// the backend's file operations are recorded on their way to the production
// implementations, so that a case can tell what the flow did to which file.
//
// Save is not covered: its file dialog cannot be driven by messages. What
// Save refuses for an opened video, the file itself by identity, is checked
// through the production comparison it uses.

#include "preview_test_fixture.h"
#include "native_test_monitor.h"

#include "temp_folder.h"

#include "app/external_video.h"
#include "app/recording_completion.h"
#include "app/session_transitions.h"
#include "app/trim_flow.h"
#include "media/temp_session.h"
#include "media/video_probe.h"
#include "platform/platform_utils.h"
#include "strings/strings.h"
#include "ui/native_controls.h"

#include <aclapi.h>
#include <commctrl.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cwctype>

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

bool SamePathText(const std::wstring& left, const std::wstring& right) {
    return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

/// The opened video, while a case watches that nothing touches it.
struct WatchedFile {
    std::wstring path;
    media::VideoFileIdentity identity;
    /// Calls that moved, secured or wrote over it; must stay zero.
    int touched{0};

    /// Counts `path` when it names the watched file, by its text or by what it is.
    void Check(const std::wstring& candidate) {
        if (!path.empty() && (SamePathText(candidate, path) || app::IsSameFile(candidate, identity))) {
            ++touched;
        }
    }
};

/// The Windows backend for files; the clipboard is recorded, not replaced.
/// Every path that would be moved, given an access list or written over is
/// compared with the watched video before the production backend acts.
class RecordedClipboardBackend final : public app::IRecordingCompletionBackend {
public:
    RecordedClipboardBackend(std::vector<std::wstring>& clipboard, WatchedFile& watched)
        : files_(app::CreateRecordingCompletionBackend()), clipboard_(clipboard), watched_(watched) {}

    bool PersistArtifact(const std::wstring& source_path, const std::wstring& destination_path,
                         media::RecordingFormat format, bool replace_existing) override {
        watched_.Check(destination_path);  // Reading the video is allowed; writing over it never.
        return files_->PersistArtifact(source_path, destination_path, format, replace_existing);
    }
    bool MoveArtifact(const std::wstring& source_path, const std::wstring& destination_path,
                      media::RecordingFormat format) override {
        watched_.Check(source_path);
        watched_.Check(destination_path);
        return files_->MoveArtifact(source_path, destination_path, format);
    }
    bool ApplyClipboardSecurity(const std::wstring& path) override {
        watched_.Check(path);
        return files_->ApplyClipboardSecurity(path);
    }
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
    WatchedFile& watched_;
};

/// The production store, with a record of what the flow asks of it. Other
/// processes (tests running in parallel, a running Easnap) create sessions in
/// the same folder, so the flow's own work session is known from its calls.
class RecordedSessionStore final : public media::ITemporarySessionStore {
public:
    explicit RecordedSessionStore(media::ITemporarySessionStore& store) : store_(store) {}

    std::optional<media::TemporarySession> CreateSession() override {
        std::optional<media::TemporarySession> session = store_.CreateSession();
        if (session.has_value()) { created.push_back(session->directory); }
        return session;
    }
    [[nodiscard]] std::vector<std::wstring> EnumerateSessions() const override { return store_.EnumerateSessions(); }
    bool DeleteSession(const media::TemporarySession& session) override {
        deleted.push_back(session.directory);
        return store_.DeleteSession(session);
    }
    bool ReleaseSession(const media::TemporarySession& session) override {
        released.push_back(session.directory);
        return store_.ReleaseSession(session);
    }
    bool MarkRecoverable(const media::TemporarySession& session) override {
        marked.push_back(session.directory);
        return store_.MarkRecoverable(session);
    }
    std::vector<media::TemporarySession> AdoptRecoverableSessions() override {
        return store_.AdoptRecoverableSessions();
    }
    int CleanupStaleSessions(const std::vector<std::wstring>& protected_directories) override {
        return store_.CleanupStaleSessions(protected_directories);
    }
    [[nodiscard]] const std::wstring& root_directory() const override { return store_.root_directory(); }

    std::vector<std::wstring> created;
    std::vector<std::wstring> deleted;
    std::vector<std::wstring> released;
    std::vector<std::wstring> marked;

private:
    media::ITemporarySessionStore& store_;
};

bool Contains(const std::vector<std::wstring>& list, const std::wstring& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

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

/// The text a message dialog shows, without the carriage returns it adds.
std::wstring DialogText(HWND dialog) {
    HWND edit = ::FindWindowExW(dialog, nullptr, L"EDIT", nullptr);
    wchar_t text[1024]{};
    if (edit != nullptr) { ::GetWindowTextW(edit, text, ARRAYSIZE(text)); }
    std::wstring result(text);
    std::erase(result, L'\r');
    return result;
}

/// Answers the next question with the button `answer`, from inside its loop.
int g_answer = 0;
int g_dialogs_answered = 0;
std::wstring g_answered_text;
void AnswerNextDialog(int answer) {
    g_answer = answer;
    ::SetTimer(nullptr, 0, 20, [](HWND, UINT, UINT_PTR id, DWORD) {
        HWND dialog = FindDialog();
        if (dialog == nullptr) { return; }  // Not shown yet: the timer fires again.
        ::KillTimer(nullptr, id);
        ++g_dialogs_answered;
        g_answered_text = DialogText(dialog);
        if (HWND button = ::GetDlgItem(dialog, g_answer)) { ::SendMessageW(button, BM_CLICK, 0, 0); }
    });
}

/// Closes and counts any question that appears while it is armed: a step that
/// must ask nothing would otherwise wait in the question's loop for ever.
int g_unexpected_dialogs = 0;
std::wstring g_unexpected_text;
UINT_PTR g_unexpected_timer = 0;
void RefuseDialogs() {
    g_unexpected_timer = ::SetTimer(nullptr, 0, 20, [](HWND, UINT, UINT_PTR, DWORD) {
        if (HWND dialog = FindDialog()) {
            ++g_unexpected_dialogs;
            g_unexpected_text = DialogText(dialog);
            ::SendMessageW(dialog, WM_CLOSE, 0, 0);
        }
    });
}
void AllowDialogs() {
    if (g_unexpected_timer != 0) { ::KillTimer(nullptr, g_unexpected_timer); }
    g_unexpected_timer = 0;
}

/// Chooses item `index` of the next popup menu with the keyboard, from inside
/// the menu's own loop.
int g_menu_index = 0;
int g_menus_chosen = 0;
void ChooseNextMenuItem(int index) {
    g_menu_index = index;
    ::SetTimer(nullptr, 0, 20, [](HWND, UINT, UINT_PTR id, DWORD) {
        HWND menu = nullptr;
        ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
            wchar_t name[32]{};
            ::GetClassNameW(window, name, ARRAYSIZE(name));
            if (::lstrcmpW(name, L"#32768") == 0 && ::IsWindowVisible(window)) {
                *reinterpret_cast<HWND*>(value) = window;
                return FALSE;
            }
            return TRUE; }, reinterpret_cast<LPARAM>(&menu));
        if (menu == nullptr) { return; }  // Not open yet: the timer fires again.
        ::KillTimer(nullptr, id);
        ++g_menus_chosen;
        for (int step = 0; step <= g_menu_index; ++step) { ::PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0); }
        ::PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
    });
}
constexpr int kMenuMp4 = 0;
constexpr int kMenuGif = 1;

void Click(HWND window, int id) {
    HWND button = ::GetDlgItem(window, id);
    CHECK(button != nullptr && ::IsWindowEnabled(button) != FALSE);
    if (button != nullptr) { ::SendMessageW(button, BM_CLICK, 0, 0); }
}

/// Moves the Out point `seconds` earlier, through the timeline's keyboard.
void ShortenBy(HWND window, int seconds) {
    HWND timeline = ::FindWindowExW(window, nullptr, L"EasnapTrimTimeline", nullptr);
    CHECK(timeline != nullptr);
    ::SendMessageW(timeline, WM_KEYDOWN, VK_TAB, 0);  // From the playhead to Out.
    for (int second = 0; second < seconds; ++second) {
        ::SendMessageW(timeline, WM_KEYDOWN, VK_LEFT, 0);  // One second earlier.
    }
}
void ShortenByOneSecond(HWND window) { ShortenBy(window, 1); }

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// What the coordinator does around the flow, as small as it can be.
struct Harness {
    app::SessionStateMachine machine;
    app::AppSettings settings{};
    std::unique_ptr<media::ITemporarySessionStore> store;
    /// What the flow, and only the flow, does with the store.
    std::unique_ptr<RecordedSessionStore> flow_store;
    std::vector<std::wstring> clipboard;
    WatchedFile watched;
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
            *store, std::make_unique<RecordedClipboardBackend>(clipboard, watched));
        flow_store = std::make_unique<RecordedSessionStore>(*store);

        app::TrimFlow::Services services;
        services.machine = &machine;
        services.settings = &settings;
        services.host_window = host;
        services.completion = completion.get();
        services.store = flow_store.get();
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
        flow_store.reset();
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
    app::TrimSource source;
    source.kind = app::TrimSourceKind::Owned;
    source.path = session->recording_path();
    source.region = test::OnTestMonitor({100, 100, 740, 460});
    harness.flow->Begin(source, options, *session);

    Recording recording{*session, harness.flow->visible_window()};
    CHECK(recording.window != nullptr);
    CHECK(harness.flow->source_kind() == app::TrimSourceKind::Owned);
    CHECK(app::PendingWorkFor(machine.state(), false, false, harness.flow->source_kind()) ==
          app::PendingWork::UnsavedRecording);
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


// --- An opened video (ADR-040) --------------------------------------------------

/// What must stay as it was: the bytes, the times other than the last access
/// (reading may set it), the attributes and the owner, group and access list.
struct FileState {
    std::vector<char> bytes;
    LARGE_INTEGER created{};
    LARGE_INTEGER written{};
    LARGE_INTEGER changed{};
    DWORD attributes{0};
    std::wstring security;

    [[nodiscard]] bool operator==(const FileState& other) const {
        return bytes == other.bytes && created.QuadPart == other.created.QuadPart &&
               written.QuadPart == other.written.QuadPart && changed.QuadPart == other.changed.QuadPart &&
               attributes == other.attributes && security == other.security;
    }
};

FileState ReadFileState(const std::wstring& path) {
    FileState state;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return state; }
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) != FALSE) {
        state.created = basic.CreationTime;
        state.written = basic.LastWriteTime;
        state.changed = basic.ChangeTime;
        state.attributes = basic.FileAttributes;
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0) {
        state.bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (::ReadFile(file, state.bytes.data(), static_cast<DWORD>(state.bytes.size()), &read, nullptr) == FALSE ||
            read != state.bytes.size()) {
            state.bytes.clear();
        }
    }
    ::CloseHandle(file);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS) {
        LPWSTR text = nullptr;
        if (::ConvertSecurityDescriptorToStringSecurityDescriptorW(
                descriptor, SDDL_REVISION_1,
                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &text,
                nullptr) != FALSE) {
            state.security = text;
            ::LocalFree(text);
        }
        ::LocalFree(descriptor);
    }
    return state;
}

/// A video the user owns, in a folder of the test's own, checked by the
/// production probe as Open video checks it.
struct OpenedVideoFile {
    std::unique_ptr<test::TempFolder> folder;
    std::wstring path;
    app::TrimSource source;
    FileState state;
    std::vector<std::wstring> folder_files;

    /// H.264, with AAC unless `audio` is false: a full-range action may hand
    /// on the file itself.
    bool Create(const wchar_t* name = L"holiday.mp4", bool audio = true, uint32_t seconds = 12) {
        folder = std::make_unique<test::TempFolder>(L"easnap-opened-video");
        path = folder->File(name);
        if (!folder->ok() || !test::CreatePreviewSource(path, audio, seconds)) { return false; }
        const media::VideoProbeResult probe =
            media::ProbeVideoFile(path, std::chrono::steady_clock::now() + app::kVideoProbeDeadline);
        if (!probe.ok) {
            std::fprintf(stderr, "The probe refused the test video (reason %d)\n", static_cast<int>(probe.reason));
            return false;
        }
        const RECT anchor = test::OnTestMonitor({100, 100, 740, 460});
        source = app::MakeExternalTrimSource(path, probe, platform::MonitorForRect(anchor).work_area);
        state = ReadFileState(path);
        folder_files = folder->Files(L"*");
        return !state.bytes.empty() && !state.security.empty() && app::ExternalSourceIsReusable(path, source.facts);
    }
};

/// The trim view open on the video, with the work session the flow took.
struct OpenedVideo {
    HWND window{nullptr};
    std::wstring work_session;
};

std::optional<OpenedVideo> OpenExternal(Harness& harness, const OpenedVideoFile& video,
                                        bool preview_expected = true) {
    CHECK(harness.machine.state() == app::SessionState::Idle);
    const size_t created = harness.flow_store->created.size();
    // As the coordinator opens a checked video: straight from Idle.
    CHECK(harness.machine.RequestTransition(app::SessionState::RecordingTrimming));
    harness.flow->Begin(video.source, app::ExternalTrimOptions(video.source), std::nullopt);
    CHECK(harness.flow->source_kind() == app::TrimSourceKind::External);

    OpenedVideo opened{harness.flow->visible_window(), {}};
    CHECK(opened.window != nullptr);
    if (opened.window == nullptr) { return std::nullopt; }
    // The flow took exactly one session of its own: the work session.
    CHECK(harness.flow_store->created.size() == created + 1);
    if (harness.flow_store->created.size() > created) { opened.work_session = harness.flow_store->created.back(); }
    CHECK(!opened.work_session.empty());
    // Never offered for recovery: no marker, and not beside the user's file.
    CHECK(::GetFileAttributesW((opened.work_session + L"\\.recoverable").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(!SamePathText(platform::DirectoryOf(opened.work_session), video.folder->path()));
    // The title names the file; the close button only closes.
    wchar_t title[256]{};
    ::GetWindowTextW(opened.window, title, ARRAYSIZE(title));
    CHECK(std::wstring(title).find(app::VideoFileDisplayName(video.path)) != std::wstring::npos);
    wchar_t close[64]{};
    ::GetWindowTextW(::GetDlgItem(opened.window, IDCANCEL), close, ARRAYSIZE(close));
    CHECK(std::wstring(close) == strings::Get(IDS_EXTERNAL_TRIM_CLOSE_BUTTON));
    // Trimming an opened video is not unsaved work.
    CHECK(app::PendingWorkFor(harness.machine.state(), false, false, harness.flow->source_kind()) ==
          app::PendingWork::None);
    harness.watched.path = video.path;
    harness.watched.identity = video.source.identity;
    harness.watched.touched = 0;
    if (!preview_expected) { return opened; }
    const bool ready =
        WaitFor([&] { return ::IsWindowEnabled(::GetDlgItem(opened.window, kIdCopy)) != FALSE; }, 15'000);
    CHECK(ready);
    if (!ready) {
        std::fprintf(stderr, "The preview did not become ready on this desktop\n");
        return std::nullopt;
    }
    harness.watched.path = video.path;
    harness.watched.identity = video.source.identity;
    harness.watched.touched = 0;
    return opened;
}

/// After every case: the flow has ended without a recording to release, the
/// work session is gone, the user's folder holds nothing new, and the video
/// is byte for byte, time for time and access list for access list what it was.
void ExpectUntouched(Harness& harness, const OpenedVideoFile& video, const OpenedVideo& opened, int released) {
    CHECK(harness.recordings_released == released);
    // Deleted, never kept for later and never marked for recovery.
    CHECK(Contains(harness.flow_store->deleted, opened.work_session));
    CHECK(!Contains(harness.flow_store->released, opened.work_session));
    CHECK(!Contains(harness.flow_store->marked, opened.work_session));
    CHECK(WaitFor([&] { return !DirectoryExists(opened.work_session); }, 2000));
    CHECK(video.folder->Files(L"*") == video.folder_files);
    CHECK(ReadFileState(video.path) == video.state);
    CHECK(harness.watched.touched == 0);
    harness.watched = WatchedFile{};
}

void CopyWholeVideo(Harness& harness, const OpenedVideoFile& video) {
    std::puts("Opened video: Copy of the whole video as MP4");
    const int ended = harness.sessions_ended;
    const int released = harness.recordings_released;
    const auto opened = OpenExternal(harness, video);
    if (!opened) { return; }
    const int menus = g_menus_chosen;
    ChooseNextMenuItem(kMenuMp4);
    Click(opened->window, kIdCopy);
    CHECK(g_menus_chosen == menus + 1);
    // An MP4 with H.264 and AAC is handed on as it is: no export.
    ExpectClosed(harness, opened->window);
    CHECK(harness.sessions_ended == ended + 1);
    CHECK(harness.clipboard.size() == 1);
    if (harness.clipboard.size() == 1) {
        const std::wstring copied = harness.clipboard.front();
        CHECK(!SamePathText(copied, video.path));
        CHECK(!app::IsSameFile(copied, video.source.identity));
        CHECK(std::filesystem::path(copied).extension() == L".mp4");
        CHECK(ReadFileState(copied).bytes == video.state.bytes);
    }
    ExpectUntouched(harness, video, *opened, released);
    harness.ClipboardReplaced();
}

void CopyPartOfVideo(Harness& harness, const OpenedVideoFile& video) {
    std::puts("Opened video: Copy of a part as MP4");
    const int released = harness.recordings_released;
    const auto opened = OpenExternal(harness, video);
    if (!opened) { return; }
    ShortenByOneSecond(opened->window);
    ChooseNextMenuItem(kMenuMp4);
    Click(opened->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    // Only the export is pending, and it is.
    CHECK(app::PendingWorkFor(harness.machine.state(), false, false, harness.flow->source_kind()) ==
          app::PendingWork::Export);
    CHECK(WaitFor([&] { return harness.machine.state() == app::SessionState::Idle; }, 60'000));
    ExpectClosed(harness, opened->window);
    CHECK(harness.clipboard.size() == 1);
    if (harness.clipboard.size() == 1) {
        const std::wstring copied = harness.clipboard.front();
        CHECK(std::filesystem::path(copied).extension() == L".mp4");
        const FileState result = ReadFileState(copied);
        CHECK(!result.bytes.empty() && result.bytes != video.state.bytes);
    }
    ExpectUntouched(harness, video, *opened, released);
    harness.ClipboardReplaced();
}

void CopyVideoAsGif(Harness& harness, const OpenedVideoFile& video) {
    std::puts("Opened video: Copy of a part as GIF");
    const int released = harness.recordings_released;
    const auto opened = OpenExternal(harness, video);
    if (!opened) { return; }
    ShortenBy(opened->window, 9);  // Three seconds of GIF.
    ChooseNextMenuItem(kMenuGif);
    const ULONGLONG started = ::GetTickCount64();
    Click(opened->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    CHECK(WaitFor([&] { return harness.machine.state() == app::SessionState::Idle; }, 120'000));
    std::printf("  GIF export and copy took %llu ms\n", ::GetTickCount64() - started);
    ExpectClosed(harness, opened->window);
    CHECK(harness.clipboard.size() == 1);
    if (harness.clipboard.size() == 1) {
        const std::wstring copied = harness.clipboard.front();
        CHECK(std::filesystem::path(copied).extension() == L".gif");
        const FileState result = ReadFileState(copied);
        CHECK(result.bytes.size() > 6 && std::string(result.bytes.data(), 6) == "GIF89a");
    }
    ExpectUntouched(harness, video, *opened, released);
    harness.ClipboardReplaced();
}

void CancelThenCloseVideo(Harness& harness, const OpenedVideoFile& video) {
    std::puts("Opened video: cancelling an export, then closing");
    const int ended = harness.sessions_ended;
    const int released = harness.recordings_released;
    const auto opened = OpenExternal(harness, video);
    if (!opened) { return; }
    ShortenByOneSecond(opened->window);
    ChooseNextMenuItem(kMenuMp4);
    Click(opened->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    Click(opened->window, kIdCancelExport);
    CHECK(WaitFor([&] { return harness.machine.state() == app::SessionState::RecordingTrimming; }, 30'000));
    Pump(100);
    CHECK(harness.flow->window_visible());
    CHECK(harness.clipboard.empty());
    CHECK(harness.sessions_ended == ended);

    // Close asks nothing: nothing of the user's can be lost.
    const int unexpected = g_unexpected_dialogs;
    RefuseDialogs();
    Click(opened->window, IDCANCEL);
    AllowDialogs();
    CHECK(g_unexpected_dialogs == unexpected);
    ExpectClosed(harness, opened->window);
    CHECK(harness.sessions_ended == ended + 1);
    CHECK(harness.clipboard.empty());
    ExpectUntouched(harness, video, *opened, released);
}

void CloseVideoDuringExport(Harness& harness, const OpenedVideoFile& video) {
    std::puts("Opened video: closing during an export");
    const int ended = harness.sessions_ended;
    const int released = harness.recordings_released;
    const auto opened = OpenExternal(harness, video);
    if (!opened) { return; }
    ShortenByOneSecond(opened->window);
    ChooseNextMenuItem(kMenuMp4);
    Click(opened->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    // The one question: the export stops and the original stays unchanged.
    const int answered = g_dialogs_answered;
    AnswerNextDialog(IDYES);
    Click(opened->window, IDCANCEL);
    CHECK(g_dialogs_answered == answered + 1);
    std::wstring expected = strings::Get(IDS_EXTERNAL_CLOSE_EXPORT_TEXT);
    CHECK(g_answered_text == expected);
    CHECK(WaitFor([&] { return harness.machine.state() == app::SessionState::Idle; }, 30'000));
    ExpectClosed(harness, opened->window);
    CHECK(harness.sessions_ended == ended + 1);
    CHECK(harness.clipboard.empty());
    ExpectUntouched(harness, video, *opened, released);
}

void DestroyDuringVideoExport(Harness& harness, const OpenedVideoFile& video) {
    std::puts("Opened video: destroying the flow during an export");
    const int ended = harness.sessions_ended;
    const int released = harness.recordings_released;
    const auto opened = OpenExternal(harness, video);
    if (!opened) { return; }
    ShortenByOneSecond(opened->window);
    ChooseNextMenuItem(kMenuMp4);
    Click(opened->window, kIdCopy);
    CHECK(harness.machine.state() == app::SessionState::Exporting);
    // As at the end of the application: the worker is joined, the window
    // goes, and the work session with it; the user's file stays.
    harness.flow->Destroy();
    CHECK(::IsWindow(opened->window) == FALSE);
    CHECK(harness.flow->visible_window() == nullptr);
    CHECK(harness.flow->source_kind() == app::TrimSourceKind::Owned);
    Pump(200);
    CHECK(harness.sessions_ended == ended);
    CHECK(harness.clipboard.empty());
    harness.machine.Reset();
    ExpectUntouched(harness, video, *opened, released);
    // The clipboard destination the export wrote is abandoned with the controller.
    harness.completion->AbandonPreparedClipboardArtifact();
}

/// Save refuses the opened video as its destination by what the file is: the
/// same path, another spelling of it and a hard link all name it; a copy
/// does not.
void SaveDestinationIdentity(const OpenedVideoFile& video) {
    std::puts("Opened video: Save never writes over the video itself");
    const media::VideoFileIdentity& identity = video.source.identity;
    CHECK(app::IsSameFile(video.path, identity));
    std::wstring shouted = video.path;
    std::transform(shouted.begin(), shouted.end(), shouted.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    CHECK(app::IsSameFile(shouted, identity));
    std::wstring forward = video.path;
    std::replace(forward.begin(), forward.end(), L'\\', L'/');
    CHECK(app::IsSameFile(forward, identity));

    // Elsewhere on the same volume, so that the user's folder stays as it was.
    const test::TempFolder other(L"easnap-opened-video-link");
    CHECK(other.ok());
    const std::wstring link = other.File(L"also holiday.mp4");
    CHECK(::CreateHardLinkW(link.c_str(), video.path.c_str(), nullptr) != FALSE);
    CHECK(app::IsSameFile(link, identity));
    const std::wstring copy = other.File(L"copy.mp4");
    CHECK(::CopyFileW(video.path.c_str(), copy.c_str(), TRUE) != FALSE);
    CHECK(!app::IsSameFile(copy, identity));
    CHECK(!app::IsSameFile(other.File(L"missing.mp4"), identity));
    ::DeleteFileW(link.c_str());
    ::DeleteFileW(copy.c_str());
    CHECK(video.folder->Files(L"*") == video.folder_files);
    // The link the test made and removed changed the file's link count and so
    // its change time; its contents, write time and access list are as before.
    const FileState after = ReadFileState(video.path);
    CHECK(after.bytes == video.state.bytes);
    CHECK(after.written.QuadPart == video.state.written.QuadPart);
    CHECK(after.security == video.state.security);
}

/// The text of the trim window's status line.
std::wstring StatusText(HWND window) {
    HWND status = ::FindWindowExW(window, nullptr, L"EDIT", nullptr);
    wchar_t text[1024]{};
    if (status != nullptr) { ::GetWindowTextW(status, text, ARRAYSIZE(text)); }
    return text;
}

/// Closes and counts any popup menu that opens while it is armed.
int g_unexpected_menus = 0;
UINT_PTR g_menu_guard = 0;
void RefuseMenus() {
    g_menu_guard = ::SetTimer(nullptr, 0, 20, [](HWND, UINT, UINT_PTR, DWORD) {
        HWND menu = nullptr;
        ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
            wchar_t name[32]{};
            ::GetClassNameW(window, name, ARRAYSIZE(name));
            if (::lstrcmpW(name, L"#32768") == 0 && ::IsWindowVisible(window)) {
                *reinterpret_cast<HWND*>(value) = window;
                return FALSE;
            }
            return TRUE; }, reinterpret_cast<LPARAM>(&menu));
        if (menu != nullptr) {
            ++g_unexpected_menus;
            ::EndMenu();
        }
    });
}
void AllowMenus() {
    if (g_menu_guard != 0) { ::KillTimer(nullptr, g_menu_guard); }
    g_menu_guard = 0;
}

/// A video that changed after it was checked is refused before Copy asks for
/// a format; closing still asks nothing and removes the work session.
void ChangedVideoRefused(Harness& harness) {
    std::puts("Opened video: changed after the check, Copy refuses");
    OpenedVideoFile video;
    const bool created = video.Create(L"changed.mp4", false, 2);
    CHECK(created);
    if (!created) { return; }
    const int released = harness.recordings_released;
    const auto opened = OpenExternal(harness, video);
    if (!opened) { return; }
    // Another program saves the file: only its write time moves here, which is
    // allowed while the preview reads it (attributes are not shared access).
    HANDLE file = ::CreateFileW(video.path.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) {
        FILETIME written{};
        ULARGE_INTEGER moved{};
        moved.QuadPart = static_cast<ULONGLONG>(video.state.written.QuadPart) + 20'000'000ULL;  // Two seconds later.
        written.dwLowDateTime = moved.LowPart;
        written.dwHighDateTime = moved.HighPart;
        CHECK(::SetFileTime(file, nullptr, nullptr, &written) != FALSE);
        ::CloseHandle(file);
    }
    harness.watched.path.clear();  // The test itself has just changed it.
    const int menus = g_unexpected_menus;
    RefuseMenus();
    Click(opened->window, kIdCopy);
    AllowMenus();
    CHECK(g_unexpected_menus == menus);
    CHECK(harness.machine.state() == app::SessionState::RecordingTrimming);
    CHECK(StatusText(opened->window) == strings::Get(IDS_EXTERNAL_SOURCE_CHANGED));
    CHECK(harness.clipboard.empty());

    const int unexpected = g_unexpected_dialogs;
    RefuseDialogs();
    Click(opened->window, IDCANCEL);
    AllowDialogs();
    CHECK(g_unexpected_dialogs == unexpected);
    ExpectClosed(harness, opened->window);
    CHECK(harness.recordings_released == released);
    CHECK(Contains(harness.flow_store->deleted, opened->work_session));
    CHECK(WaitFor([&] { return !DirectoryExists(opened->work_session); }, 2000));
    CHECK(video.folder->Files(L"*") == video.folder_files);
    CHECK(ReadFileState(video.path).bytes == video.state.bytes);
    harness.watched = WatchedFile{};
}

/// A video whose preview fails is only closed: no question, nothing offered
/// to save, and the failure says the file is unchanged.
void FailedPreviewCloses(Harness& harness) {
    std::puts("Opened video: the preview fails, closing just closes");
    OpenedVideoFile video;
    const bool created = video.Create(L"gone.mp4", false, 2);
    CHECK(created);
    if (!created) { return; }
    // Checked, then gone before the window opens: the preview cannot open it.
    CHECK(::DeleteFileW(video.path.c_str()) != FALSE);
    const int released = harness.recordings_released;
    const int before_open = g_unexpected_dialogs;
    RefuseDialogs();
    const auto opened = OpenExternal(harness, video, false);
    if (!opened) {
        AllowDialogs();
        return;
    }
    // The window reports the failure in its status line and, when the helper
    // reports it, once in a message with the same text; that message is
    // shown from the same handler, so it has come and gone when the status says so.
    const auto reported_failure = [&] { return StatusText(opened->window).find(L"The video file is unchanged.") != std::wstring::npos; };
    CHECK(WaitFor(reported_failure, 15'000));
    AllowDialogs();
    const std::wstring status = StatusText(opened->window);
    CHECK(status.find(strings::Get(IDS_TRIM_PREVIEW_FAILED_NEXT)) == std::wstring::npos);
    CHECK(g_unexpected_dialogs - before_open <= 1);
    if (g_unexpected_dialogs - before_open == 1) { CHECK(g_unexpected_text == status); }
    CHECK(::IsWindowEnabled(::GetDlgItem(opened->window, kIdCopy)) == FALSE);
    CHECK(::IsWindowEnabled(::GetDlgItem(opened->window, IDOK)) == FALSE);
    // Closing asks nothing at all.
    const int before_close = g_unexpected_dialogs;
    RefuseDialogs();
    Click(opened->window, IDCANCEL);
    AllowDialogs();
    CHECK(g_unexpected_dialogs == before_close);
    ExpectClosed(harness, opened->window);
    CHECK(harness.recordings_released == released);
    CHECK(Contains(harness.flow_store->deleted, opened->work_session));
    CHECK(WaitFor([&] { return !DirectoryExists(opened->work_session); }, 2000));
    CHECK(video.folder->Files(L"*").empty());
    harness.watched = WatchedFile{};
}

void OpenedVideoCases(Harness& harness) {
    OpenedVideoFile video;
    const bool created = video.Create();
    CHECK(created);
    if (!created) { return; }
    CopyWholeVideo(harness, video);
    CopyPartOfVideo(harness, video);
    CopyVideoAsGif(harness, video);
    CancelThenCloseVideo(harness, video);
    CloseVideoDuringExport(harness, video);
    DestroyDuringVideoExport(harness, video);
    SaveDestinationIdentity(video);
    ChangedVideoRefused(harness);
    FailedPreviewCloses(harness);
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
        OpenedVideoCases(harness);
    }
    harness.Destroy();
    ::CoUninitialize();
    std::printf("trim flow: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
