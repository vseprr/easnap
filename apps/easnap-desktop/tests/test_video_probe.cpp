// The video probe (ADR-040) on files written at run time: production-writer
// MP4s of several shapes, sink-writer WMV/ASF and audio-only files, bad input
// and access cases. Every file lives in a folder of the test's own under the
// user's temporary directory and is deleted at the end.
//
//   test_video_probe          MP4, bad input and access cases
//   test_video_probe --asf    WMV and ASF cases; exits 77 when Windows has no
//                             WMV or WMA encoder
#include "media/video_probe.h"

#include <mfapi.h>
#include <mferror.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "platform/platform_utils.h"
#include "preview_test_fixture.h"
#include "sink_writer_fixture.h"

namespace {

using easnap::media::ProbeVideoFile;
using easnap::media::VideoContainer;
using easnap::media::VideoNotSupportedYetCause;
using easnap::media::VideoProbeResult;
using easnap::media::VideoProbeStep;
using easnap::media::VideoRefusal;

constexpr int kSkipped = 77;
constexpr int64_t kSecond = 10'000'000;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

class TestDirectory final {
public:
    bool Create() {
        wchar_t temp[MAX_PATH]{};
        const DWORD count = ::GetTempPathW(ARRAYSIZE(temp), temp);
        GUID id{};
        wchar_t text[40]{};
        if (count == 0 || count >= ARRAYSIZE(temp) || FAILED(::CoCreateGuid(&id)) ||
            ::StringFromGUID2(id, text, ARRAYSIZE(text)) == 0) {
            return false;
        }
        path_ = temp;
        path_ += L"EasnapProbeTests-";
        path_ += text;
        return ::CreateDirectoryW(path_.c_str(), nullptr) != FALSE;
    }

    ~TestDirectory() {
        std::error_code ignored;
        if (!path_.empty()) {
            std::filesystem::remove_all(path_, ignored);
        }
    }

    TestDirectory() = default;
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;

    [[nodiscard]] std::wstring File(const std::wstring& name) const { return path_ + L"\\" + name; }
    [[nodiscard]] const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

class MediaFoundationRuntime final {
public:
    MediaFoundationRuntime() {
        const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized_ = SUCCEEDED(com_result);
        result_ = com_initialized_ ? ::MFStartup(MF_VERSION, MFSTARTUP_FULL) : com_result;
        mf_started_ = SUCCEEDED(result_);
    }
    ~MediaFoundationRuntime() {
        if (mf_started_) {
            static_cast<void>(::MFShutdown());
        }
        if (com_initialized_) {
            ::CoUninitialize();
        }
    }
    MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
    MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool com_initialized_{false};
    bool mf_started_{false};
};

std::chrono::steady_clock::time_point Deadline() {
    // ADR-040 gives the probe five seconds. Test machines run builds next to
    // it, so the tests allow more and assert the answer, not the speed.
    return std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

VideoProbeResult Probe(const std::wstring& path) {
    return ProbeVideoFile(path, Deadline());
}

uint64_t FileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
        return 0;
    }
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32u) | data.nFileSizeLow;
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL wrote = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                   nullptr);
    ::CloseHandle(file);
    return wrote != FALSE && written == bytes.size();
}

std::vector<uint8_t> ReadBytes(const std::wstring& path) {
    std::vector<uint8_t> bytes(static_cast<std::size_t>(FileBytes(path)));
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    DWORD read = 0;
    const BOOL ok = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    ::CloseHandle(file);
    return ok != FALSE && read == bytes.size() ? bytes : std::vector<uint8_t>{};
}

std::vector<uint8_t> RandomBytes(std::size_t count) {
    std::mt19937 generator(0x45A5u);
    std::vector<uint8_t> bytes(count);
    for (uint8_t& byte : bytes) {
        byte = static_cast<uint8_t>(generator() & 0xFFu);
    }
    return bytes;
}

bool Near(int64_t actual, int64_t expected, int64_t tolerance) {
    return actual >= expected - tolerance && actual <= expected + tolerance;
}

/// A probe that accepted the file, with the facts every accepted file has.
void CheckAccepted(const VideoProbeResult& result, const std::wstring& path,
                   VideoContainer container, uint32_t width, uint32_t height) {
    CHECK(result.ok);
    CHECK(result.reason == VideoRefusal::None);
    CHECK(result.error == S_OK);
    CHECK(result.facts.step == VideoProbeStep::Complete);
    CHECK(result.facts.container == container);
    CHECK(result.facts.has_video);
    CHECK(result.facts.width == width && result.facts.height == height);
    CHECK(result.facts.rotation_degrees == 0);
    CHECK(!result.facts.first_frame_interlaced);
    CHECK(result.facts.identity.size_bytes == FileBytes(path));
    CHECK(result.facts.identity.last_write_time != 0);
    if (!result.ok) {
        std::fwprintf(stderr, L"  refused %ls: reason %d, step %d, 0x%08X\n", path.c_str(),
                      static_cast<int>(result.reason), static_cast<int>(result.facts.step),
                      static_cast<unsigned>(result.error));
    }
}

void CheckRefused(const VideoProbeResult& result, VideoRefusal reason, const char* label) {
    CHECK(!result.ok);
    CHECK(result.reason == reason);
    if (result.reason != reason) {
        std::fprintf(stderr, "  %s: reason %d, step %d, 0x%08X\n", label,
                     static_cast<int>(result.reason), static_cast<int>(result.facts.step),
                     static_cast<unsigned>(result.error));
    }
}

/// The probe has returned, so nothing may still hold the file: it deletes.
void CheckReleased(const std::wstring& path) {
    const bool deleted = ::DeleteFileW(path.c_str()) != FALSE;
    CHECK(deleted);
    if (!deleted) {
        std::fwprintf(stderr, L"  still held after the probe: %ls (error %lu)\n", path.c_str(),
                      ::GetLastError());
    }
}

void TestProductionMp4Shapes(const TestDirectory& directory) {
    struct Shape final {
        const wchar_t* name;
        easnap::test::PreviewSourceSpec spec;
        int64_t duration;
    };
    std::vector<Shape> shapes;
    {
        easnap::test::PreviewSourceSpec spec;
        spec.frame_count = 60;
        spec.include_audio = true;
        shapes.push_back({L"640x360-30-audio.mp4", spec, 2 * kSecond});
    }
    {
        easnap::test::PreviewSourceSpec spec;
        spec.width = 1920;
        spec.height = 1080;
        spec.frame_rate = 60;
        spec.frame_count = 60;
        shapes.push_back({L"1920x1080-60.mp4", spec, kSecond});
    }
    {
        easnap::test::PreviewSourceSpec spec;
        spec.width = 360;
        spec.height = 640;
        spec.frame_count = 30;
        shapes.push_back({L"360x640-portrait.mp4", spec, kSecond});
    }
    {
        easnap::test::PreviewSourceSpec spec;
        spec.frame_rate = 24;
        spec.frame_count = 48;
        spec.include_audio = true;
        shapes.push_back({L"640x360-24.mp4", spec, 2 * kSecond});
    }
    {
        // 33 ms, 17 ms and 50 ms in turn: a variable frame rate.
        easnap::test::PreviewSourceSpec spec;
        spec.frame_count = 30;
        spec.frame_durations = {1600, 800, 2400};
        spec.include_audio = true;
        shapes.push_back({L"640x360-uneven.mp4", spec, kSecond});
    }
    {
        easnap::test::PreviewSourceSpec spec;
        spec.frame_count = 1;
        shapes.push_back({L"one-frame.mp4", spec, kSecond / 30});
    }

    for (const Shape& shape : shapes) {
        const std::wstring path = directory.File(shape.name);
        CHECK(easnap::test::CreatePreviewSource(path, shape.spec));
        const VideoProbeResult result = Probe(path);
        CheckAccepted(result, path, VideoContainer::Mpeg4, shape.spec.width, shape.spec.height);
        CHECK(result.facts.video_subtype == MFVideoFormat_H264);
        CHECK(result.facts.frame_rate_denominator != 0);
        if (shape.spec.frame_durations.empty()) {
            CHECK(result.facts.frame_rate_numerator ==
                  shape.spec.frame_rate * result.facts.frame_rate_denominator);
        }
        CHECK(result.facts.has_audio == shape.spec.include_audio);
        if (shape.spec.include_audio) {
            CHECK(result.facts.audio_subtype == MFAudioFormat_AAC);
        }
        // AAC framing can stretch the presentation by about one AAC frame.
        CHECK(Near(result.facts.duration.count(), shape.duration, kSecond / 10));
        std::wprintf(L"%ls: %ux%u, %u/%u fps, %lld ms, audio %d\n", shape.name,
                     result.facts.width, result.facts.height, result.facts.frame_rate_numerator,
                     result.facts.frame_rate_denominator,
                     static_cast<long long>(result.facts.duration.count() / 10'000),
                     result.facts.has_audio ? 1 : 0);
        CheckReleased(path);
    }
}

void TestOtherExtensionsAndIdentity(const TestDirectory& directory) {
    const std::wstring source = directory.File(L"identity.mp4");
    easnap::test::PreviewSourceSpec spec;
    spec.width = 320;
    spec.height = 240;
    spec.frame_count = 15;
    CHECK(easnap::test::CreatePreviewSource(source, spec));

    // The MPEG-4 rows of the table, the extension in any case.
    for (const wchar_t* name : {L"copy.mov", L"copy.m4v", L"COPY.MP4"}) {
        const std::wstring path = directory.File(name);
        CHECK(::CopyFileW(source.c_str(), path.c_str(), TRUE) != FALSE);
        CheckAccepted(Probe(path), path, VideoContainer::Mpeg4, 320, 240);
    }

    const VideoProbeResult first = Probe(source);
    const VideoProbeResult second = Probe(source);
    CheckAccepted(first, source, VideoContainer::Mpeg4, 320, 240);
    CHECK(first.facts.identity == second.facts.identity);
    const VideoProbeResult copy = Probe(directory.File(L"copy.mov"));
    CHECK(copy.facts.identity.volume_serial == first.facts.identity.volume_serial);
    CHECK(copy.facts.identity.file_id != first.facts.identity.file_id);

    // A later write changes the identity a later step compares.
    const HANDLE file = ::CreateFileW(source.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) {
        const uint8_t padding[16]{};
        DWORD written = 0;
        CHECK(::WriteFile(file, padding, sizeof(padding), &written, nullptr) != FALSE);
        ::CloseHandle(file);
    }
    const VideoProbeResult changed = Probe(source);
    CHECK(changed.facts.identity.file_id == first.facts.identity.file_id);
    CHECK(changed.facts.identity.size_bytes == first.facts.identity.size_bytes + 16u);
    CHECK(!(changed.facts.identity == first.facts.identity));
}

void TestBadInput(const TestDirectory& directory) {
    const std::wstring random_mp4 = directory.File(L"random.mp4");
    CHECK(WriteBytes(random_mp4, RandomBytes(64 * 1024)));
    const VideoProbeResult random = Probe(random_mp4);
    CheckRefused(random, VideoRefusal::Damaged, "random bytes");
    CHECK(random.facts.step == VideoProbeStep::OpenSource);
    // ADR-040 names this result; the probe test verifies it on a real file.
    CHECK(random.error == MF_E_UNSUPPORTED_BYTESTREAM_TYPE);
    CheckReleased(random_mp4);

    const std::wstring empty_mp4 = directory.File(L"empty.mp4");
    CHECK(WriteBytes(empty_mp4, {}));
    CheckRefused(Probe(empty_mp4), VideoRefusal::Damaged, "empty file");
    CheckReleased(empty_mp4);

    // The first half of a real recording: the index at its end is missing.
    const std::wstring whole = directory.File(L"whole.mp4");
    easnap::test::PreviewSourceSpec spec;
    spec.frame_count = 30;
    spec.include_audio = true;
    CHECK(easnap::test::CreatePreviewSource(whole, spec));
    std::vector<uint8_t> bytes = ReadBytes(whole);
    CHECK(bytes.size() > 1024);
    bytes.resize(bytes.size() / 2);
    const std::wstring truncated = directory.File(L"truncated.mp4");
    CHECK(WriteBytes(truncated, bytes));
    CheckRefused(Probe(truncated), VideoRefusal::Damaged, "truncated");
    CheckReleased(truncated);

    const std::wstring unknown = directory.File(L"video.xyz");
    CHECK(::CopyFileW(whole.c_str(), unknown.c_str(), TRUE) != FALSE);
    const VideoProbeResult unknown_result = Probe(unknown);
    CheckRefused(unknown_result, VideoRefusal::UnsupportedExtension, "unknown extension");
    CHECK(unknown_result.facts.step == VideoProbeStep::Extension);
    const std::wstring no_extension = directory.File(L"video");
    CHECK(::CopyFileW(whole.c_str(), no_extension.c_str(), TRUE) != FALSE);
    CheckRefused(Probe(no_extension), VideoRefusal::UnsupportedExtension, "no extension");

    // Known containers v1 does not open are "format not supported".
    for (const wchar_t* name : {L"video.mkv", L"video.WEBM"}) {
        const std::wstring path = directory.File(name);
        CHECK(::CopyFileW(whole.c_str(), path.c_str(), TRUE) != FALSE);
        CheckRefused(Probe(path), VideoRefusal::FormatNotSupported, "known unsupported");
    }

    const std::wstring folder = directory.File(L"folder.mp4");
    CHECK(::CreateDirectoryW(folder.c_str(), nullptr) != FALSE);
    CheckRefused(Probe(folder), VideoRefusal::UnsupportedExtension, "folder");

    // An MP4 that holds only AAC sound.
    const std::wstring audio_only = directory.File(L"audio-only.mp4");
    easnap::test::SinkWriterSpec audio_spec;
    audio_spec.audio_subtype = MFAudioFormat_AAC;
    const easnap::test::SinkWriterOutcome audio_outcome =
        easnap::test::WriteWithSinkWriter(audio_only, audio_spec);
    CHECK(audio_outcome == easnap::test::SinkWriterOutcome::Written);
    if (audio_outcome == easnap::test::SinkWriterOutcome::Written) {
        const VideoProbeResult result = Probe(audio_only);
        CheckRefused(result, VideoRefusal::NoVideo, "audio only");
        CHECK(result.facts.step == VideoProbeStep::FindVideo);
        CHECK(result.facts.has_audio && result.facts.audio_subtype == MFAudioFormat_AAC);
        CheckReleased(audio_only);
    }
}

void TestLocationsAndAccess(const TestDirectory& directory) {
    const std::wstring source = directory.File(L"access.mp4");
    easnap::test::PreviewSourceSpec spec;
    spec.width = 320;
    spec.height = 240;
    spec.frame_count = 15;
    CHECK(easnap::test::CreatePreviewSource(source, spec));

    const VideoProbeResult missing = Probe(directory.File(L"missing.mp4"));
    CheckRefused(missing, VideoRefusal::NotFound, "missing file");
    CHECK(missing.error == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    CheckRefused(Probe(directory.File(L"missing-folder\\video.mp4")), VideoRefusal::NotFound,
                 "missing folder");
    CheckRefused(Probe(L"video.mp4"), VideoRefusal::NotFound, "relative path");
    CheckRefused(Probe(L""), VideoRefusal::NotFound, "empty path");

    // Network locations are refused by the path rules alone; nothing here
    // reaches the network.
    const VideoProbeResult unc = Probe(L"\\\\easnap-test-server.invalid\\share\\video.mp4");
    CheckRefused(unc, VideoRefusal::NetworkOrOffline, "UNC path");
    CHECK(unc.facts.step == VideoProbeStep::Path);
    CHECK(unc.error == S_OK);
    CheckRefused(Probe(L"\\\\?\\UNC\\easnap-test-server.invalid\\share\\video.mp4"),
                 VideoRefusal::NetworkOrOffline, "long UNC path");
    CheckRefused(Probe(L"https://easnap-test-server.invalid/video.mp4"),
                 VideoRefusal::NetworkOrOffline, "URL");
    CheckRefused(Probe(L"file:///C:/video.mp4"), VideoRefusal::NetworkOrOffline, "file URL");

    // The long-path form of a local file opens.
    CheckAccepted(Probe(L"\\\\?\\" + source), source, VideoContainer::Mpeg4, 320, 240);

    // A file another program holds without sharing is in use.
    const HANDLE exclusive = ::CreateFileW(source.c_str(), GENERIC_READ, 0, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(exclusive != INVALID_HANDLE_VALUE);
    if (exclusive != INVALID_HANDLE_VALUE) {
        const VideoProbeResult in_use = Probe(source);
        CheckRefused(in_use, VideoRefusal::InUse, "exclusive lock");
        CHECK(in_use.facts.step == VideoProbeStep::OpenFile);
        CHECK(in_use.error == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));
        ::CloseHandle(exclusive);
    }
    // So is a file another program is writing.
    const HANDLE writing = ::CreateFileW(source.c_str(), GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(writing != INVALID_HANDLE_VALUE);
    if (writing != INVALID_HANDLE_VALUE) {
        CheckRefused(Probe(source), VideoRefusal::InUse, "open for writing");
        ::CloseHandle(writing);
    }
    // A reader that shares the file does not stop the probe.
    const HANDLE reading = ::CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(reading != INVALID_HANDLE_VALUE);
    if (reading != INVALID_HANDLE_VALUE) {
        CheckAccepted(Probe(source), source, VideoContainer::Mpeg4, 320, 240);
        ::CloseHandle(reading);
    }

    // A read-only file opens, and the probe leaves it as it was.
    const std::wstring read_only = directory.File(L"read-only.mp4");
    CHECK(::CopyFileW(source.c_str(), read_only.c_str(), TRUE) != FALSE);
    CHECK(::SetFileAttributesW(read_only.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    const std::vector<uint8_t> before = ReadBytes(read_only);
    CheckAccepted(Probe(read_only), read_only, VideoContainer::Mpeg4, 320, 240);
    CHECK(ReadBytes(read_only) == before);
    CHECK((::GetFileAttributesW(read_only.c_str()) & FILE_ATTRIBUTE_READONLY) != 0);
    CHECK(::SetFileAttributesW(read_only.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);

    // After the probe nothing holds the file: it can be renamed and deleted.
    const std::wstring renamed = directory.File(L"renamed.mp4");
    CHECK(::MoveFileW(source.c_str(), renamed.c_str()) != FALSE);
    CHECK(::DeleteFileW(renamed.c_str()) != FALSE);

    // A deadline that has passed stops the probe before it touches the file.
    const VideoProbeResult late =
        ProbeVideoFile(read_only, std::chrono::steady_clock::now() - std::chrono::seconds(1));
    CheckRefused(late, VideoRefusal::Timeout, "deadline");
    CHECK(late.facts.deadline_passed);
    CHECK(late.facts.step == VideoProbeStep::Path);
}

int RunMp4Cases() {
    TestDirectory directory;
    CHECK(directory.Create());
    if (g_fail != 0) {
        return 1;
    }
    TestProductionMp4Shapes(directory);
    TestOtherExtensionsAndIdentity(directory);
    TestBadInput(directory);
    TestLocationsAndAccess(directory);
    return g_fail == 0 ? 0 : 1;
}

int RunAsfCases() {
    TestDirectory directory;
    CHECK(directory.Create());
    if (g_fail != 0) {
        return 1;
    }
    struct Case final {
        const wchar_t* name;
        GUID video;
        GUID audio;
    };
    const Case cases[]{
        {L"wmv3-wma.wmv", MFVideoFormat_WMV3, MFAudioFormat_WMAudioV8},
        {L"vc1-wma.wmv", MFVideoFormat_WVC1, MFAudioFormat_WMAudioV8},
        {L"wmv3.asf", MFVideoFormat_WMV3, GUID_NULL},
    };
    int written = 0;
    for (const Case& item : cases) {
        const std::wstring path = directory.File(item.name);
        easnap::test::SinkWriterSpec spec;
        spec.video_subtype = item.video;
        spec.audio_subtype = item.audio;
        spec.width = 320;
        spec.height = 240;
        spec.frame_count = 30;
        const easnap::test::SinkWriterOutcome outcome =
            easnap::test::WriteWithSinkWriter(path, spec);
        if (outcome == easnap::test::SinkWriterOutcome::EncoderUnavailable) {
            std::wprintf(L"SKIP %ls: Windows has no encoder for it\n", item.name);
            continue;
        }
        CHECK(outcome == easnap::test::SinkWriterOutcome::Written);
        if (outcome != easnap::test::SinkWriterOutcome::Written) {
            continue;
        }
        ++written;
        const VideoProbeResult result = Probe(path);
        CheckAccepted(result, path, VideoContainer::Asf, 320, 240);
        CHECK(result.facts.video_subtype == item.video);
        CHECK(result.facts.has_audio == (item.audio != GUID_NULL));
        if (item.audio != GUID_NULL) {
            CHECK(result.facts.audio_subtype == item.audio);
        }
        CHECK(Near(result.facts.duration.count(), kSecond, kSecond / 10));
        std::wprintf(L"%ls: %ux%u, %lld ms, audio %d\n", item.name, result.facts.width,
                     result.facts.height,
                     static_cast<long long>(result.facts.duration.count() / 10'000),
                     result.facts.has_audio ? 1 : 0);
    }

    // H.264 in ASF plays in Windows, but the table lists it only for MPEG-4
    // and AVI.
    const std::wstring h264_asf = directory.File(L"h264-wma.wmv");
    easnap::test::SinkWriterSpec spec;
    spec.video_subtype = MFVideoFormat_H264;
    spec.audio_subtype = MFAudioFormat_WMAudioV8;
    const easnap::test::SinkWriterOutcome outcome = easnap::test::WriteWithSinkWriter(h264_asf, spec);
    if (outcome == easnap::test::SinkWriterOutcome::Written) {
        ++written;
        const VideoProbeResult result = Probe(h264_asf);
        CheckRefused(result, VideoRefusal::FormatNotSupported, "H.264 in ASF");
        CHECK(result.facts.step == VideoProbeStep::ReadFormat);
        CHECK(result.facts.container == VideoContainer::Asf);
    } else {
        CHECK(outcome == easnap::test::SinkWriterOutcome::EncoderUnavailable);
    }
    if (g_fail != 0) {
        return 1;
    }
    return written == 0 ? kSkipped : 0;
}

void PrintLogLine(const wchar_t* line) noexcept { std::fwprintf(stderr, L"log: %ls\n", line); }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    easnap::platform::SetLogSink(PrintLogLine);
    const bool asf = argc > 1 && std::wcscmp(argv[1], L"--asf") == 0;
    int exit_code = 0;
    {
        const MediaFoundationRuntime runtime;
        CHECK(SUCCEEDED(runtime.result()));
        if (SUCCEEDED(runtime.result())) {
            exit_code = asf ? RunAsfCases() : RunMp4Cases();
        }
    }
    if (exit_code == kSkipped) {
        std::printf("SKIPPED  no WMV/ASF file could be written on this machine\n");
        return kSkipped;
    }
    if (g_fail == 0 && exit_code == 0) {
        std::printf("PASSED  %d/%d checks\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
