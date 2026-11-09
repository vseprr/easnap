// Easnap tests - opening a video file the user owns (ADR-040).
//
// Without arguments (Fast): what the user is told for every probe answer, one
// string per answer and each naming the file, the name a trimmed copy is
// offered under, in both languages, and the path rule that keeps Easnap from
// opening files in its own data folder.
//
// With --probe-job (Standard): the production probe job on a worker thread,
// posting to a message-only window. A video written by the production writer
// is accepted and a missing file is refused; each completion carries its own
// generation and is taken once, and a result whose message is lost can still
// be taken. A file in a real temporary session counts as inside Easnap's data
// folder, under any spelling of its path.

#include "app/external_video.h"
#include "preview_test_fixture.h"
#include "temp_folder.h"

#include "media/temp_session.h"
#include "strings/strings.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <cwchar>
#include <optional>
#include <set>
#include <string>

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

media::VideoProbeResult Refused(media::VideoRefusal reason,
                                media::VideoNotSupportedYetCause cause = media::VideoNotSupportedYetCause::None) {
    media::VideoProbeResult result;
    result.ok = false;
    result.reason = reason;
    result.not_supported_yet = cause;
    return result;
}

bool HasPlaceholder(const wchar_t* text) { return text != nullptr && std::wcsstr(text, L"%1") != nullptr; }

// --- Fast ------------------------------------------------------------------------

/// Every answer of the probe has its own sentence; the reason the user reads
/// follows ADR-040's wording table.
void RefusalMessages() {
    using media::VideoNotSupportedYetCause;
    using media::VideoRefusal;
    struct Row {
        media::VideoProbeResult result;
        UINT expected;
    };
    const Row rows[] = {
        {Refused(VideoRefusal::UnsupportedExtension), IDS_VIDEO_REFUSED_EXTENSION},
        {Refused(VideoRefusal::FormatNotSupported), IDS_VIDEO_REFUSED_FORMAT},
        {Refused(VideoRefusal::NeedsCodecExtension), IDS_VIDEO_REFUSED_CODEC_EXTENSION},
        {Refused(VideoRefusal::Damaged), IDS_VIDEO_REFUSED_DAMAGED},
        {Refused(VideoRefusal::Protected), IDS_VIDEO_REFUSED_PROTECTED},
        {Refused(VideoRefusal::NoVideo), IDS_VIDEO_REFUSED_NO_VIDEO},
        {Refused(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::Rotation), IDS_VIDEO_REFUSED_ROTATION},
        {Refused(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::PixelAspect),
         IDS_VIDEO_REFUSED_PIXEL_ASPECT},
        {Refused(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::Interlaced), IDS_VIDEO_REFUSED_INTERLACED},
        {Refused(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::HighBitDepth),
         IDS_VIDEO_REFUSED_HIGH_BIT_DEPTH},
        {Refused(VideoRefusal::NotSupportedYet), IDS_VIDEO_REFUSED_NOT_YET},
        {Refused(VideoRefusal::NetworkOrOffline), IDS_VIDEO_REFUSED_NETWORK},
        {Refused(VideoRefusal::InUse), IDS_VIDEO_REFUSED_IN_USE},
        {Refused(VideoRefusal::NotFound), IDS_VIDEO_REFUSED_NOT_FOUND},
        {Refused(VideoRefusal::NoAccess), IDS_VIDEO_REFUSED_NO_ACCESS},
        // Windows N without the Media Feature Pack: Record's remedy, said of videos.
        {Refused(VideoRefusal::MediaFoundationUnavailable), IDS_VIDEO_REFUSED_MEDIA_FOUNDATION},
        // A deadline on a local disk: damaged or unreadable.
        {Refused(VideoRefusal::Timeout), IDS_VIDEO_REFUSED_DAMAGED},
    };
    std::set<UINT> distinct;
    for (const Row& row : rows) {
        const UINT id = app::VideoRefusalMessageId(row.result);
        CHECK(id == row.expected);
        distinct.insert(id);
        for (const strings::Language language : {strings::Language::English, strings::Language::Turkish}) {
            const wchar_t* text = strings::Get(id, language);
            CHECK(text != nullptr && *text != L'\0');
            // Each names the file.
            CHECK(HasPlaceholder(text));
        }
    }
    // One sentence per answer: no two answers share a string (the timeout
    // borrows "damaged" by design).
    CHECK(distinct.size() == sizeof(rows) / sizeof(rows[0]) - 1);

    // A deadline where the file is slow because of where it lives.
    for (const media::VideoPathProblem remote :
         {media::VideoPathProblem::Remote, media::VideoPathProblem::Offline, media::VideoPathProblem::Url}) {
        media::VideoProbeResult timeout = Refused(VideoRefusal::Timeout);
        timeout.facts.path_problem = remote;
        CHECK(app::VideoRefusalMessageId(timeout) == IDS_VIDEO_REFUSED_NETWORK);
    }
    // Not refusals: the generic failure, never a wrong reason.
    media::VideoProbeResult accepted;
    accepted.ok = true;
    CHECK(app::VideoRefusalMessageId(accepted) == IDS_VIDEO_OPEN_FAILED);
    accepted.reason = VideoRefusal::TooLargeForMp4;
    CHECK(app::VideoRefusalMessageId(accepted) == IDS_VIDEO_OPEN_FAILED);

    strings::SetPreference(strings::LanguagePreference::English);
    const std::wstring shown =
        strings::Format(app::VideoRefusalMessageId(Refused(VideoRefusal::InUse)), {L"holiday.mp4"});
    CHECK(shown.find(L"\"holiday.mp4\"") != std::wstring::npos);
}

/// The text rule behind refusing files in Easnap's own folder.
void EasnapFolderPaths() {
    const std::wstring folder = L"C:\\Users\\Ada\\AppData\\Local\\Easnap";
    CHECK(app::PathIsInsideFolder(folder + L"\\temp\\0A1B\\recording.mp4", folder));
    CHECK(app::PathIsInsideFolder(L"c:/users/ada/appdata/local/EASNAP/temp/x.mp4", folder));
    CHECK(app::PathIsInsideFolder(L"\\\\?\\C:\\Users\\Ada\\AppData\\Local\\Easnap\\x.mp4", folder));
    CHECK(app::PathIsInsideFolder(folder + L"\\x.mp4", folder + L"\\"));
    CHECK(app::PathIsInsideFolder(folder + L"\\x.mp4", L"\\\\?\\" + folder));
    // A folder whose name only begins the same, the folder itself and its parent are not inside.
    CHECK(!app::PathIsInsideFolder(L"C:\\Users\\Ada\\AppData\\Local\\Easnap Videos\\x.mp4", folder));
    CHECK(!app::PathIsInsideFolder(L"C:\\Users\\Ada\\AppData\\Local\\EasnapX\\x.mp4", folder));
    CHECK(!app::PathIsInsideFolder(folder, folder));
    CHECK(!app::PathIsInsideFolder(folder + L"\\", folder));
    CHECK(!app::PathIsInsideFolder(L"C:\\Users\\Ada\\AppData\\Local\\x.mp4", folder));
    CHECK(!app::PathIsInsideFolder(L"D:\\Users\\Ada\\AppData\\Local\\Easnap\\x.mp4", folder));
    CHECK(!app::PathIsInsideFolder(L"", folder));
    CHECK(!app::PathIsInsideFolder(folder + L"\\x.mp4", L""));
    // A drive root contains everything on its drive.
    CHECK(app::PathIsInsideFolder(L"C:\\a.mp4", L"C:\\"));
    CHECK(!app::PathIsInsideFolder(L"D:\\a.mp4", L"C:\\"));
}

/// "<name> (trimmed)", from the file name without its last extension.
void TrimmedNames() {
    strings::SetPreference(strings::LanguagePreference::English);
    CHECK(app::TrimmedVideoFileName(L"C:\\Videos\\holiday.mp4") == L"holiday (trimmed)");
    CHECK(app::TrimmedVideoFileName(L"C:\\Videos\\clip.final.mov") == L"clip.final (trimmed)");
    CHECK(app::TrimmedVideoFileName(L"D:/clips/no extension") == L"no extension (trimmed)");
    CHECK(app::TrimmedVideoFileName(L"C:\\Videos\\.hidden") == L".hidden (trimmed)");
    CHECK(app::VideoFileDisplayName(L"C:\\Videos\\holiday.mp4") == L"holiday.mp4");
    CHECK(app::VideoFileDisplayName(L"D:/clips/a.avi") == L"a.avi");
    strings::SetPreference(strings::LanguagePreference::Turkish);
    CHECK(app::TrimmedVideoFileName(L"C:\\Videos\\tatil.mp4") == L"tatil (kırpılmış)");
    strings::SetPreference(strings::LanguagePreference::English);
}

// --- Standard: the probe job ------------------------------------------------------

/// Waits for the job's message on `window` and returns its WPARAM.
std::optional<WPARAM> WaitForCompletion(HWND window, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        if (::PeekMessageW(&message, window, app::kMsgVideoProbeCompleted, app::kMsgVideoProbeCompleted,
                           PM_REMOVE) != FALSE) {
            return message.wParam;
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_POSTMESSAGE);
    }
    return std::nullopt;
}

int ProbeJob() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!platform::MediaFoundationAvailable()) {
        std::puts("Media Foundation is not available; the probe job cannot be checked here");
        ::CoUninitialize();
        return 77;
    }
    const test::TempFolder folder(L"easnap-probe-job");
    CHECK(folder.ok());
    const std::wstring video = folder.File(L"accepted.mp4");
    CHECK(test::CreateVideoPreviewSource(video, 2));
    HWND window = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    CHECK(window != nullptr);

    app::VideoProbeJob job;
    CHECK(!job.busy());
    CHECK(job.generation() == 0);
    CHECK(!job.TryTake(0).has_value());

    // An accepted video: the message carries this probe's generation.
    CHECK(job.Start(window, app::kMsgVideoProbeCompleted, video, app::kVideoProbeDeadline));
    const app::VideoProbeJob::Generation first = job.generation();
    CHECK(first == 1);
    CHECK(job.busy());
    // One at a time: a second file is refused while the first is checked.
    CHECK(!job.Start(window, app::kMsgVideoProbeCompleted, folder.File(L"other.mp4"), app::kVideoProbeDeadline));
    const std::optional<WPARAM> posted = WaitForCompletion(window, std::chrono::seconds(30));
    CHECK(posted.has_value() && *posted == first);
    const std::optional<app::VideoProbeOutcome> outcome = job.TryTake(first);
    CHECK(outcome.has_value());
    if (outcome.has_value()) {
        CHECK(outcome->path == video);
        CHECK(outcome->result.ok);
        CHECK(!outcome->worker_exception);
        CHECK(outcome->result.facts.width == 640 && outcome->result.facts.height == 360);
        CHECK(outcome->result.facts.identity.size_bytes > 0);
        // The identity the probe recorded is the one the flow compares with.
        CHECK(app::IsSameFile(video, outcome->result.facts.identity));
    }
    CHECK(!job.busy());
    CHECK(!job.TryTake(first).has_value());  // Taken once.

    // A missing file: refused, and told in the user's words.
    CHECK(job.Start(window, app::kMsgVideoProbeCompleted, folder.File(L"missing.mp4"), app::kVideoProbeDeadline));
    const app::VideoProbeJob::Generation second = job.generation();
    CHECK(second == first + 1);
    const std::optional<WPARAM> refused = WaitForCompletion(window, std::chrono::seconds(30));
    CHECK(refused.has_value() && *refused == second);
    CHECK(!job.TryTake(first).has_value());  // A stale message finds nothing.
    const std::optional<app::VideoProbeOutcome> missing = job.TryTake(second);
    CHECK(missing.has_value());
    if (missing.has_value()) {
        CHECK(!missing->result.ok);
        CHECK(missing->result.reason == media::VideoRefusal::NotFound);
        CHECK(app::VideoRefusalMessageId(missing->result) == IDS_VIDEO_REFUSED_NOT_FOUND);
    }

    // A result whose message is lost: the window it was posted to is gone.
    // The job says it has finished, and the result can still be taken.
    HWND gone = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ::DestroyWindow(gone);
    CHECK(job.Start(gone, app::kMsgVideoProbeCompleted, video, app::kVideoProbeDeadline));
    const app::VideoProbeJob::Generation third = job.generation();
    const auto finish_by = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (job.running() && std::chrono::steady_clock::now() < finish_by) {
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    CHECK(!job.running());
    CHECK(job.busy());  // Not taken yet: Open video drops it before a new check.
    const std::optional<app::VideoProbeOutcome> unposted = job.TryTake(third);
    CHECK(unposted.has_value() && unposted->result.ok);
    CHECK(!job.busy() && !job.running());

    // A file in one of Easnap's own temporary sessions is inside its data
    // folder, however its path is written; the test's own file is not.
    {
        const std::unique_ptr<media::ITemporarySessionStore> store = media::CreateTemporarySessionStore();
        const std::optional<media::TemporarySession> session = store->CreateSession();
        CHECK(session.has_value());
        if (session.has_value()) {
            const std::wstring inside = session->recording_path();
            CHECK(::CopyFileW(video.c_str(), inside.c_str(), TRUE) != FALSE);
            const std::wstring data_folder = platform::AppDataDirectory();
            CHECK(app::IsInsideEasnapFolder(inside, data_folder));
            std::wstring spelled = inside;
            std::replace(spelled.begin(), spelled.end(), L'\\', L'/');
            CHECK(app::IsInsideEasnapFolder(spelled, data_folder));
            wchar_t short_path[MAX_PATH]{};
            if (::GetShortPathNameW(inside.c_str(), short_path, MAX_PATH) != 0) {
                CHECK(app::IsInsideEasnapFolder(short_path, data_folder));
            }
            CHECK(!app::IsInsideEasnapFolder(video, data_folder));
            CHECK(!app::IsInsideEasnapFolder(data_folder, data_folder));
            CHECK(store->DeleteSession(*session));
        }
    }

    // A probe whose result nobody took is joined when the job goes.
    {
        app::VideoProbeJob abandoned;
        CHECK(abandoned.Start(window, app::kMsgVideoProbeCompleted, video, app::kVideoProbeDeadline));
    }
    MSG drained{};
    while (::PeekMessageW(&drained, window, 0, 0, PM_REMOVE) != FALSE) {
    }
    if (window != nullptr) { ::DestroyWindow(window); }
    ::CoUninitialize();
    std::printf("probe job: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "--probe-job") {
        return ProbeJob();
    }
    RefusalMessages();
    TrimmedNames();
    EasnapFolderPaths();
    std::printf("external video: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
