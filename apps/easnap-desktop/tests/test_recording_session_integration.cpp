#include <windows.h>
#include "native_test_monitor.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "app/recording_completion.h"
#include "media/audio_queue.h"
#include "media/media_preview.h"
#include "media/temp_session.h"
#include "platform/platform_utils.h"
#include "recording/recording_session.h"

namespace {

using Microsoft::WRL::ComPtr;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

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

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool com_initialized_{false};
    bool mf_started_{false};
};

struct Mp4Inspection {
    uint64_t duration_100ns{0};
    uint64_t sample_count{0};
    uint64_t last_sample_end_100ns{0};
    uint64_t audio_sample_count{0};
    uint64_t last_audio_sample_end_100ns{0};
    bool sample_durations_valid{false};
    bool audio_valid{false};
    bool valid{false};
};

class PreviewWindow final {
public:
    PreviewWindow() {
        const RECT placement=easnap::test::OnTestMonitor({0,0,320,240});
        window_ = ::CreateWindowExW(0, L"STATIC", L"Easnap preview test",
                                    WS_POPUP, placement.left, placement.top, 320, 240, nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    }

    ~PreviewWindow() {
        if (window_ != nullptr) {
            static_cast<void>(::DestroyWindow(window_));
        }
    }

    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    [[nodiscard]] HWND get() const noexcept { return window_; }

private:
    HWND window_{nullptr};
};

template <typename Predicate>
bool WaitForPreview(easnap::media::IMediaPreview& preview,
                    Predicate&& predicate,
                    std::chrono::milliseconds timeout =
                        std::chrono::milliseconds{8'000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        // Match the real UI owner: preview snapshots and its watchdog advance
        // through this thread's message loop, including hidden owner timers.
        MSG message{};
        for (unsigned count = 0; count < 128 &&
             ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        const easnap::media::MediaPreviewSnapshot snapshot = preview.snapshot();
        if (snapshot.state == easnap::media::MediaPreviewState::Failed) {
            return false;
        }
        if (predicate(snapshot)) {
            return true;
        }
        static_cast<void>(::MsgWaitForMultipleObjectsEx(
            0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool PositionNear(easnap::media::IMediaPreview& preview,
                  easnap::media::MediaTime target,
                  easnap::media::MediaTime tolerance =
                      easnap::media::MediaTime{3'000'000}) {
    const easnap::media::MediaTime actual = preview.position();
    const easnap::media::MediaTime delta =
        actual > target ? actual - target : target - actual;
    return delta <= tolerance;
}

void ReportPreviewFailure(const wchar_t* scenario,
                          easnap::media::IMediaPreview& preview,
                          const wchar_t* checkpoint) {
    const easnap::media::MediaPreviewSnapshot snapshot = preview.snapshot();
    std::fwprintf(
        stderr,
        L"%ls preview checkpoint %ls failed: state=%u HRESULT=0x%08X operation=%u desired-play=%u pending=%u position=%lld duration=%lld\n",
        scenario, checkpoint, static_cast<unsigned int>(snapshot.state),
        static_cast<unsigned int>(snapshot.error),
        static_cast<unsigned int>(snapshot.failed_operation),
        snapshot.play_requested ? 1u : 0u,
        snapshot.command_pending ? 1u : 0u,
        static_cast<long long>(preview.position().count()),
        static_cast<long long>(snapshot.duration.count()));
    std::fwprintf(stderr, L"%ls\n", preview.diagnostics().c_str());
}

bool ExerciseMfPlayPreview(const wchar_t* scenario,
                           const std::wstring& source_path) {
    using easnap::media::MediaPreviewState;
    using easnap::media::MediaTime;

    PreviewWindow window;
    auto preview = easnap::media::CreateMfPlayPreview();
    CHECK(window.get() != nullptr);
    CHECK(preview != nullptr);
    if (window.get() == nullptr || preview == nullptr) {
        return false;
    }

    const bool opened =
        preview->Open(source_path, window.get(), window.get(), WM_APP + 71u);
    CHECK(opened);
    const bool ready = opened && WaitForPreview(
                                     *preview,
                                     [](const auto& snapshot) {
                                         return snapshot.state ==
                                                MediaPreviewState::Ready;
                                     });
    CHECK(ready);
    if (!ready) {
        ReportPreviewFailure(scenario, *preview, L"open");
        preview->Close();
        return false;
    }
    const MediaTime duration = preview->snapshot().duration;
    CHECK(duration > MediaTime::zero());

    CHECK(preview->Play());
    const bool advanced = WaitForPreview(
        *preview, [&](const auto& snapshot) {
            return snapshot.state == MediaPreviewState::Playing &&
                   preview->position() > MediaTime::zero();
        });
    CHECK(advanced);
    CHECK(preview->Pause());
    const bool paused = WaitForPreview(*preview, [](const auto& snapshot) {
        return snapshot.state == MediaPreviewState::Paused &&
               !snapshot.command_pending;
    });
    CHECK(paused);

    const MediaTime first_scrub{duration.count() / 4};
    const MediaTime final_scrub{(duration.count() * 3) / 4};
    CHECK(preview->Seek(first_scrub));
    CHECK(preview->Seek(final_scrub));
    const bool scrubbed = WaitForPreview(
        *preview, [&](const auto& snapshot) {
            return !snapshot.command_pending &&
                   snapshot.state == MediaPreviewState::Paused &&
                   PositionNear(*preview, final_scrub);
        });
    CHECK(scrubbed);

    const MediaTime play_from{duration.count() / 3};
    const MediaTime out_point{(duration.count() * 2) / 3};
    CHECK(preview->PlayFrom(play_from));
    const bool playing_from = WaitForPreview(
        *preview, [](const auto& snapshot) {
            return snapshot.state == MediaPreviewState::Playing;
        });
    CHECK(playing_from);
    CHECK(preview->PauseAt(out_point));
    const bool held_at_out = WaitForPreview(
        *preview, [&](const auto& snapshot) {
            return !snapshot.command_pending &&
                   snapshot.state == MediaPreviewState::Paused &&
                   PositionNear(*preview, out_point);
        });
    CHECK(held_at_out);

    const MediaTime natural_end_start{
        (std::max)(int64_t{0}, duration.count() - 3'000'000)};
    CHECK(preview->PlayFrom(natural_end_start));
    const bool naturally_ended = WaitForPreview(
        *preview, [](const auto& snapshot) {
            return snapshot.state == MediaPreviewState::Ended;
        });
    CHECK(naturally_ended);

    CHECK(preview->PlayFrom(MediaTime::zero()));
    const bool replayed = WaitForPreview(
        *preview, [&](const auto& snapshot) {
            return snapshot.state == MediaPreviewState::Playing &&
                   preview->position() > MediaTime::zero();
        });
    CHECK(replayed);
    CHECK(preview->Pause());
    const bool replay_paused = WaitForPreview(
        *preview, [](const auto& snapshot) {
            return snapshot.state == MediaPreviewState::Paused &&
                   !snapshot.command_pending;
        });
    CHECK(replay_paused);

    const bool remained_healthy =
        preview->snapshot().state != MediaPreviewState::Failed;
    CHECK(remained_healthy);
    if (!remained_healthy) {
        ReportPreviewFailure(scenario, *preview, L"final");
    }
    preview->Close();
    return advanced && paused && scrubbed && playing_from && held_at_out &&
           naturally_ended && replayed && replay_paused && remained_healthy;
}

struct GifInspection final {
    uint32_t width{0};
    uint32_t height{0};
    uint64_t frame_count{0};
    uint64_t duration_centiseconds{0};
    uint32_t failed_frame{0};
    HRESULT decode_result{S_OK};
    bool finalized{false};
    bool valid{false};
};

uint32_t MetadataUnsigned(IWICMetadataQueryReader* reader,
                          const wchar_t* name) {
    if (reader == nullptr) {
        return 0;
    }
    PROPVARIANT value;
    ::PropVariantInit(&value);
    const HRESULT result = reader->GetMetadataByName(name, &value);
    uint32_t output = 0;
    if (SUCCEEDED(result)) {
        if (value.vt == VT_UI1) {
            output = value.bVal;
        } else if (value.vt == VT_UI2) {
            output = value.uiVal;
        } else if (value.vt == VT_UI4) {
            output = value.ulVal;
        }
    }
    ::PropVariantClear(&value);
    return output;
}

GifInspection InspectGif(const std::wstring& path) {
    GifInspection inspection;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
            &decoder);
    }
    UINT frame_count = 0;
    if (SUCCEEDED(result)) {
        result = decoder->GetFrameCount(&frame_count);
    }
    for (UINT index = 0; SUCCEEDED(result) && index < frame_count; ++index) {
        inspection.failed_frame = index;
        ComPtr<IWICBitmapFrameDecode> frame;
        result = decoder->GetFrame(index, &frame);
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(result)) {
            result = frame->GetSize(&width, &height);
        }
        if (index == 0 && SUCCEEDED(result)) {
            inspection.width = width;
            inspection.height = height;
        }
        ComPtr<IWICMetadataQueryReader> metadata;
        if (SUCCEEDED(result)) {
            result = frame->GetMetadataQueryReader(&metadata);
        }
        if (SUCCEEDED(result)) {
            const uint32_t delay =
                MetadataUnsigned(metadata.Get(), L"/grctlext/Delay");
            if (delay == 0) {
                result = E_UNEXPECTED;
            } else {
                inspection.duration_centiseconds += delay;
            }
        }
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(result)) {
            result = factory->CreateFormatConverter(&converter);
        }
        if (SUCCEEDED(result)) {
            result = converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom);
        }
        std::vector<BYTE> pixels;
        if (SUCCEEDED(result)) {
            const uint64_t byte_count =
                static_cast<uint64_t>(width) * height * 4u;
            if (byte_count > (std::numeric_limits<UINT>::max)()) {
                result = E_OUTOFMEMORY;
            } else {
                pixels.resize(static_cast<std::size_t>(byte_count));
                result = converter->CopyPixels(nullptr, width * 4u,
                                               static_cast<UINT>(pixels.size()),
                                               pixels.data());
            }
        }
    }
    inspection.frame_count = frame_count;
    inspection.decode_result = result;

    easnap::platform::UniqueHandle file{
        ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    LARGE_INTEGER offset{};
    offset.QuadPart = -1;
    BYTE trailer = 0;
    DWORD read = 0;
    inspection.finalized =
        file.get() != nullptr && file.get() != INVALID_HANDLE_VALUE &&
        ::SetFilePointerEx(file.get(), offset, nullptr, FILE_END) != FALSE &&
        ::ReadFile(file.get(), &trailer, 1, &read, nullptr) != FALSE && read == 1 &&
        trailer == 0x3B;
    inspection.valid = SUCCEEDED(result) && frame_count > 0 &&
                       inspection.width > 0 && inspection.height > 0 &&
                       inspection.duration_centiseconds > 0 &&
                       inspection.finalized;
    return inspection;
}

bool HasGifPartialFiles(const std::wstring& directory) {
    WIN32_FIND_DATAW entry{};
    const HANDLE find =
        ::FindFirstFileW((directory + L"\\*.partial.gif").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    ::FindClose(find);
    return true;
}

class ClipboardPublicationProbe final
    : public easnap::app::IRecordingCompletionBackend {
public:
    bool PersistArtifact(const std::wstring&, const std::wstring&,
                         easnap::media::RecordingFormat, bool) override {
        ++persist_calls;
        return false;
    }

    bool MoveArtifact(const std::wstring&, const std::wstring&,
                      easnap::media::RecordingFormat) override {
        ++move_calls;
        return false;
    }

    bool ApplyClipboardSecurity(const std::wstring& path) override {
        ++security_calls;
        return easnap::app::ApplyClipboardPublicationSecurity(path);
    }

    [[nodiscard]] bool IsNonemptyArtifact(
        const std::wstring& path,
        easnap::media::RecordingFormat format) const override {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!easnap::app::IsValidRecordingDestination(path, format) ||
            ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
                                   &attributes) == FALSE ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return false;
        }
        const ULARGE_INTEGER size{attributes.nFileSizeLow,
                                  attributes.nFileSizeHigh};
        return size.QuadPart > 0;
    }

    bool PutFileOnClipboard(HWND, const std::wstring& path) override {
        ++clipboard_calls;
        clipboard_paths = std::vector<std::wstring>{path};
        return true;
    }

    [[nodiscard]] std::optional<std::vector<std::wstring>>
    ClipboardFilePaths() const override {
        return clipboard_paths;
    }

    int persist_calls{0};
    int move_calls{0};
    int security_calls{0};
    int clipboard_calls{0};
    std::optional<std::vector<std::wstring>> clipboard_paths{
        std::vector<std::wstring>{}};
};

Mp4Inspection InspectMp4(const std::wstring& path, UINT32 expected_width,
                         UINT32 expected_height, bool expect_audio) {
    Mp4Inspection inspection;
    ComPtr<IMFSourceReader> reader;
    HRESULT result = ::MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    CHECK(SUCCEEDED(result));
    if (FAILED(result)) {
        return inspection;
    }

    ComPtr<IMFMediaType> native_type;
    result = reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native_type);
    CHECK(SUCCEEDED(result));
    if (FAILED(result)) {
        return inspection;
    }

    GUID major_type{};
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    result = native_type->GetGUID(MF_MT_MAJOR_TYPE, &major_type);
    result = SUCCEEDED(result) ? native_type->GetGUID(MF_MT_SUBTYPE, &subtype) : result;
    result = SUCCEEDED(result)
                 ? ::MFGetAttributeSize(native_type.Get(), MF_MT_FRAME_SIZE, &width, &height)
                 : result;
    CHECK(SUCCEEDED(result));
    CHECK(major_type == MFMediaType_Video);
    CHECK(subtype == MFVideoFormat_H264);
    CHECK(width == expected_width);
    CHECK(height == expected_height);
    if (FAILED(result) || major_type != MFMediaType_Video || subtype != MFVideoFormat_H264 ||
        width != expected_width || height != expected_height) {
        return inspection;
    }

    PROPVARIANT duration;
    ::PropVariantInit(&duration);
    result = reader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration);
    CHECK(SUCCEEDED(result));
    if (SUCCEEDED(result) && duration.vt == VT_UI8) {
        inspection.duration_100ns = duration.uhVal.QuadPart;
    } else if (SUCCEEDED(result) && duration.vt == VT_I8 && duration.hVal.QuadPart >= 0) {
        inspection.duration_100ns = static_cast<uint64_t>(duration.hVal.QuadPart);
    } else {
        CHECK(false);
    }
    ::PropVariantClear(&duration);

    LONGLONG previous_timestamp = -1;
    bool timestamps_monotonic = true;
    bool sample_durations_valid = true;
    for (;;) {
        DWORD stream_flags = 0;
        LONGLONG sample_time = 0;
        ComPtr<IMFSample> sample;
        result = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                    0, nullptr, &stream_flags, &sample_time, &sample);
        if (FAILED(result) || (stream_flags & MF_SOURCE_READERF_ERROR) != 0) {
            timestamps_monotonic = false;
            break;
        }
        if ((stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample == nullptr) {
            continue;
        }
        if (sample_time < 0 || (previous_timestamp >= 0 && sample_time <= previous_timestamp)) {
            timestamps_monotonic = false;
        }
        LONGLONG sample_duration = 0;
        if (FAILED(sample->GetSampleDuration(&sample_duration)) || sample_duration <= 0 ||
            sample_time > (std::numeric_limits<LONGLONG>::max)() - sample_duration) {
            sample_durations_valid = false;
        } else {
            inspection.last_sample_end_100ns =
                static_cast<uint64_t>(sample_time + sample_duration);
        }
        previous_timestamp = sample_time;
        ++inspection.sample_count;
    }
    CHECK(timestamps_monotonic);
    CHECK(sample_durations_valid);
    CHECK(inspection.sample_count > 0);

    if (expect_audio) {
        ComPtr<IMFMediaType> audio_type;
        result = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
            &audio_type);
        CHECK(SUCCEEDED(result));
        GUID audio_major_type{};
        GUID audio_subtype{};
        UINT32 audio_sample_rate = 0;
        UINT32 audio_channels = 0;
        if (SUCCEEDED(result)) {
            result = audio_type->GetGUID(MF_MT_MAJOR_TYPE, &audio_major_type);
        }
        if (SUCCEEDED(result)) {
            result = audio_type->GetGUID(MF_MT_SUBTYPE, &audio_subtype);
        }
        if (SUCCEEDED(result)) {
            result = audio_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                           &audio_sample_rate);
        }
        if (SUCCEEDED(result)) {
            result = audio_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                           &audio_channels);
        }
        CHECK(SUCCEEDED(result));
        CHECK(audio_major_type == MFMediaType_Audio);
        CHECK(audio_subtype == MFAudioFormat_AAC);
        CHECK(audio_sample_rate == 44'100 || audio_sample_rate == 48'000);
        CHECK(audio_channels == 1 || audio_channels == 2);

        LONGLONG previous_audio_timestamp = -1;
        bool audio_timestamps_monotonic = true;
        bool audio_durations_valid = true;
        for (;;) {
            DWORD stream_flags = 0;
            LONGLONG sample_time = 0;
            ComPtr<IMFSample> sample;
            result = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                nullptr, &stream_flags, &sample_time, &sample);
            if (FAILED(result) || (stream_flags & MF_SOURCE_READERF_ERROR) != 0) {
                audio_timestamps_monotonic = false;
                break;
            }
            if ((stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                break;
            }
            if (sample == nullptr) {
                continue;
            }
            if (sample_time < 0 ||
                (previous_audio_timestamp >= 0 &&
                 sample_time <= previous_audio_timestamp)) {
                audio_timestamps_monotonic = false;
            }
            LONGLONG sample_duration = 0;
            if (FAILED(sample->GetSampleDuration(&sample_duration)) ||
                sample_duration <= 0 ||
                sample_time > (std::numeric_limits<LONGLONG>::max)() -
                                  sample_duration) {
                audio_durations_valid = false;
            } else {
                inspection.last_audio_sample_end_100ns =
                    static_cast<uint64_t>(sample_time + sample_duration);
            }
            previous_audio_timestamp = sample_time;
            ++inspection.audio_sample_count;
        }
        CHECK(audio_timestamps_monotonic);
        CHECK(audio_durations_valid);
        CHECK(inspection.audio_sample_count > 0);
        inspection.audio_valid = SUCCEEDED(result) &&
                                 audio_major_type == MFMediaType_Audio &&
                                 audio_subtype == MFAudioFormat_AAC &&
                                 audio_timestamps_monotonic &&
                                 audio_durations_valid &&
                                 inspection.audio_sample_count > 0;
    } else {
        inspection.audio_valid = true;
    }
    inspection.sample_durations_valid = sample_durations_valid;
    inspection.valid = timestamps_monotonic && sample_durations_valid &&
                       inspection.sample_count > 0 && inspection.duration_100ns > 0 &&
                       inspection.audio_valid;
    return inspection;
}

RECT ModestPrimaryMonitorRegion() {
    POINT origin{};
    const auto* selected=easnap::test::SelectedTestMonitor();
    const HMONITOR monitor = selected ? selected->handle : ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == FALSE) {
        return {};
    }

    const LONG monitor_width = info.rcMonitor.right - info.rcMonitor.left;
    const LONG monitor_height = info.rcMonitor.bottom - info.rcMonitor.top;
    // Match the non-macroblock-aligned shape from the Phase 3.2 acceptance
    // defect when the desktop is large enough. Both dimensions force the H.264
    // decoder's coded surface/aperture boundary to be exercised.
    LONG width = (std::min)(492L, monitor_width);
    LONG height = (std::min)(748L, monitor_height);
    width -= width % 2;
    height -= height % 2;
    if (width < 2 || height < 2) {
        return {};
    }
    const LONG left = info.rcMonitor.left + (monitor_width - width) / 2;
    const LONG top = info.rcMonitor.top + (monitor_height - height) / 2;
    return RECT{left, top, left + width, top + height};
}

class VisibleTestSurface final {
public:
    ~VisibleTestSurface() { Stop(); }

    bool Start(const RECT& region, bool animate) {
        region_ = region;
        animate_ = animate;
        try {
            thread_ = std::thread(&VisibleTestSurface::ThreadMain, this);
        } catch (...) {
            return false;
        }
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return initialized_; });
        return window_ != nullptr;
    }

    void Stop() noexcept {
        stop_.store(true, std::memory_order_release);
        changed_.notify_all();
        HWND window = nullptr;
        {
            const std::scoped_lock lock(mutex_);
            window = window_;
        }
        if (window != nullptr) {
            ::PostMessageW(window, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
        if (message == WM_ERASEBKGND) {
            return 1;
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    static void DrawFrame(HWND window, uint32_t frame_index) {
        RECT client{};
        if (::GetClientRect(window, &client) == FALSE) {
            return;
        }
        const BYTE red = static_cast<BYTE>((frame_index * 17u) & 0xffu);
        const BYTE green = static_cast<BYTE>((frame_index * 29u + 64u) & 0xffu);
        const BYTE blue = static_cast<BYTE>((frame_index * 43u + 128u) & 0xffu);
        HDC dc = ::GetDC(window);
        if (dc == nullptr) {
            return;
        }
        HBRUSH background = ::CreateSolidBrush(RGB(red, green, blue));
        if (background != nullptr) {
            ::FillRect(dc, &client, background);
            ::DeleteObject(background);
        }
        const LONG width = client.right - client.left;
        RECT marker{static_cast<LONG>(frame_index * 19u % static_cast<uint32_t>((std::max)(1L, width))),
                    40, 0, 140};
        marker.right = (std::min)(client.right, marker.left + 120);
        HBRUSH white = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
        ::FillRect(dc, &marker, white);
        ::GdiFlush();
        ::ReleaseDC(window, dc);
    }

    void ThreadMain() noexcept {
        constexpr wchar_t kClassName[] = L"EasnapRecordingIntegrationSurface";
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = &VisibleTestSurface::WindowProc;
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.lpszClassName = kClassName;
        window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        const ATOM atom = ::RegisterClassW(&window_class);
        if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            const std::scoped_lock lock(mutex_);
            initialized_ = true;
            ready_.notify_all();
            return;
        }

        HWND window = ::CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
            L"Easnap recording integration surface", WS_POPUP, region_.left, region_.top,
            region_.right - region_.left, region_.bottom - region_.top, nullptr, nullptr,
            ::GetModuleHandleW(nullptr), nullptr);
        {
            const std::scoped_lock lock(mutex_);
            window_ = window;
            initialized_ = true;
            ready_.notify_all();
        }
        if (window == nullptr) {
            return;
        }

        ::ShowWindow(window, SW_SHOWNOACTIVATE);
        ::UpdateWindow(window);
        DrawFrame(window, animate_ ? 1u : 9u);

        uint32_t frame_index = 2;
        while (!stop_.load(std::memory_order_acquire)) {
            MSG message{};
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            if (animate_) {
                DrawFrame(window, frame_index++);
            }
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, std::chrono::milliseconds(animate_ ? 40 : 100),
                              [this] { return stop_.load(std::memory_order_acquire); });
        }
        ::DestroyWindow(window);
        const std::scoped_lock lock(mutex_);
        window_ = nullptr;
    }

    RECT region_{};
    bool animate_{false};
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable changed_;
    std::thread thread_;
    HWND window_{nullptr};
    bool initialized_{false};
};

void TestRealDesktopRecordingSession() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    const RECT region = ModestPrimaryMonitorRegion();
    const UINT32 width = static_cast<UINT32>(region.right - region.left);
    const UINT32 height = static_cast<UINT32>(region.bottom - region.top);
    CHECK(width >= 2 && height >= 2);
    if (width < 2 || height < 2) {
        return;
    }

    MediaFoundationRuntime inspection_runtime;
    CHECK(SUCCEEDED(inspection_runtime.result()));
    if (FAILED(inspection_runtime.result())) {
        return;
    }

    wchar_t temporary_path[MAX_PATH]{};
    const DWORD temporary_length =
        ::GetTempPathW(ARRAYSIZE(temporary_path), temporary_path);
    CHECK(temporary_length > 0 && temporary_length < ARRAYSIZE(temporary_path));
    const std::wstring save_output_directory =
        std::wstring(temporary_path) + L"EasnapGifTerminal-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" +
        std::to_wstring(::GetTickCount64());
    CHECK(easnap::platform::EnsureDirectory(save_output_directory));

    auto store = easnap::media::CreateTemporarySessionStore();
    CHECK(store != nullptr);
    if (store == nullptr) {
        return;
    }

    const auto run_scenario =
        [&](const wchar_t* name, bool show_surface, bool animate,
             uint32_t frame_rate, easnap::media::AudioSource audio_source,
             std::chrono::milliseconds first_active,
             std::chrono::milliseconds pause_duration,
             std::chrono::milliseconds second_active,
             easnap::media::RecordingFormat format =
                 easnap::media::RecordingFormat::Mp4,
             bool exercise_preview = false) {
            VisibleTestSurface surface;
            const bool surface_started = !show_surface || surface.Start(region, animate);
            CHECK(surface_started);
            if (!surface_started) {
                return;
            }
            if (show_surface) {
                std::this_thread::sleep_for(250ms);
            }

            easnap::media::RecordingOptions options;
            options.region = region;
            options.format = format;
            options.audio = audio_source;
            options.include_cursor = false;
            options.target_frame_rate = frame_rate;
            options.video_bitrate_bps = 2'000'000;

            auto session = easnap::recording::CreateVideoRecordingSession(*store, options);
            CHECK(session != nullptr);
            if (session == nullptr) {
                return;
            }

            const auto start_requested = Clock::now();
            const bool started = session->Start();
            if (!started) {
                std::fwprintf(stderr, L"%ls Start error: %ls\n", name,
                              session->last_error().c_str());
            }
            CHECK(started);
            if (!started) {
                return;
            }

            // Start has already published the first capture. Anchor once at
            // that active offset, externally bracket it by the Start call, and
            // measure subsequent elapsed time independently of session timing.
            const auto anchor_before = Clock::now();
            const auto initial_active = easnap::media::MediaTime{
                static_cast<int64_t>(session->diagnostics().active_duration_100ns)};
            const auto anchor_after = Clock::now();
            CHECK(initial_active <= std::chrono::duration_cast<easnap::media::MediaTime>(
                                        anchor_after - start_requested));
            const auto wall_start = anchor_before + (anchor_after - anchor_before) / 2;
            const auto first_active_start = wall_start;
            std::this_thread::sleep_for(first_active);
            const auto first_active_end = Clock::now();

            std::chrono::steady_clock::duration measured_second_active{};
            if (pause_duration > 0ms) {
                const bool paused = session->Pause();
                CHECK(paused);
                if (!paused) {
                    session->Abort();
                    return;
                }
                std::this_thread::sleep_for(pause_duration);
                const bool resumed = session->Resume();
                const auto second_active_start = Clock::now();
                CHECK(resumed);
                if (!resumed) {
                    session->Abort();
                    return;
                }
                std::this_thread::sleep_for(second_active);
                measured_second_active = Clock::now() - second_active_start;
            }
            const auto stop_request = Clock::now();

            const bool finalized = session->Stop();
            if (!finalized) {
                std::fwprintf(stderr, L"%ls Stop error: %ls\n", name,
                              session->last_error().c_str());
            }
            CHECK(finalized);
            CHECK(session->state() == easnap::recording::RecordingSessionState::Finalized);
            surface.Stop();

            const easnap::recording::RecordingSessionDiagnostics diagnostics =
                session->diagnostics();
            CHECK(diagnostics.all_worker_threads_stopped);
            CHECK(diagnostics.queue_capacity ==
                  easnap::capture::kDefaultVideoFrameQueueCapacity);
            CHECK(diagnostics.peak_queue_depth <= diagnostics.queue_capacity);
            CHECK(diagnostics.capture_attempts ==
                  diagnostics.acquired_frames + diagnostics.no_new_frames +
                      diagnostics.capture_failures);
            CHECK(diagnostics.reused_frames >=
                  diagnostics.failure_reused_frames);
            CHECK(diagnostics.reused_frames -
                      diagnostics.failure_reused_frames <=
                  diagnostics.no_new_frames);
            CHECK(diagnostics.accepted_frames > 0);
            CHECK(diagnostics.written_frames == diagnostics.accepted_frames);
            CHECK(diagnostics.queue_full_rejections == 0);
            CHECK(diagnostics.capture_failures == 0);
            CHECK(diagnostics.failure_reused_frames == 0);
            CHECK(diagnostics.capture_failure_diagnostics.count <=
                  easnap::capture::kCaptureFailureDiagnosticCapacity);
            CHECK(diagnostics.capture_failure_diagnostics.count +
                      diagnostics.capture_failure_diagnostics.omitted ==
                  diagnostics.capture_failures);
            CHECK(diagnostics.capture_attempt_timing.count ==
                  diagnostics.capture_attempts);
            CHECK(diagnostics.acquisition_timing.count ==
                  diagnostics.capture_attempts);
            CHECK(diagnostics.region_copy_timing.count ==
                  diagnostics.capture_attempts);
            CHECK(diagnostics.queue_submission_timing.count ==
                  diagnostics.accepted_frames + diagnostics.queue_full_rejections);
            CHECK(diagnostics.writer_submission_timing.count ==
                  diagnostics.written_frames);
            const bool audio_enabled = audio_source != easnap::media::AudioSource::None;
            const bool is_microphone =
                audio_source == easnap::media::AudioSource::Microphone;
            if (audio_enabled) {
                // Internal accounting consistency: silence write timing
                // matches the silence write counter.
                CHECK(diagnostics.silence_write_submission_timing.count ==
                      diagnostics.silence_writes);
                // Real captured audio accounting invariant:
                // audio_writer_submission_timing counts only successful real
                // audio packet writes, matching written_audio_packets 1:1.
                CHECK(diagnostics.audio_writer_submission_timing.count ==
                      diagnostics.written_audio_packets);
                // If real audio was written, frames must also be non-zero.
                if (diagnostics.written_audio_packets > 0) {
                    CHECK(diagnostics.written_audio_frames > 0);
                }
                // Microphone always delivers real audio — require it.
                // System Audio may be silent if the desktop is quiet.
                // Synthetic silence may be zero for Microphone since real
                // audio arrives continuously from recording start.
                if (is_microphone) {
                    CHECK(diagnostics.written_audio_packets > 0);
                    CHECK(diagnostics.written_audio_frames > 0);
                }
                // System Audio may be silent if the desktop is quiet.
                // If no real audio arrived, synthetic silence must have
                // maintained the continuous AAC timeline.
                if (!is_microphone && diagnostics.written_audio_packets == 0) {
                    CHECK(diagnostics.silence_writes > 0);
                }
                CHECK(diagnostics.audio_capture.capture_failures == 0);
                CHECK(diagnostics.audio_capture.device_invalidations == 0);
                CHECK(diagnostics.audio_capture.buffer_overruns == 0);
                CHECK(diagnostics.audio_capture.queue_capacity >=
                      easnap::media::kMinimumAudioQueueCapacity);
                CHECK(diagnostics.audio_capture.peak_queue_occupancy <=
                      diagnostics.audio_capture.queue_capacity);
                CHECK(diagnostics.audio_capture.timestamp_resolution_count <=
                      easnap::media::kAudioTimestampDiagnosticCapacity);
                // First audio writer input comes from either real audio or
                // finalization silence (which sets it if no real audio arrived).
                CHECK(diagnostics.first_audio_writer_input_pts_100ns >= 0);
                CHECK(diagnostics.maximum_audio_gate_wait_microseconds ==
                      diagnostics.audio_backpressure_wait_timing
                          .maximum_microseconds);
                for (std::size_t index = 0;
                     index < diagnostics.audio_capture.timestamp_resolution_count;
                     ++index) {
                    CHECK(diagnostics.audio_capture
                              .timestamp_resolutions[index]
                              .resolved_timestamp >=
                          easnap::media::MediaTime::zero());
                }
                for (std::size_t index = 0;
                     index < diagnostics.audio_packet_timeline_count; ++index) {
                    const auto& timeline =
                        diagnostics.audio_packet_timeline[index];
                    CHECK(timeline.mapped_duration_100ns > 0);
                    CHECK(timeline.mapped_pts_100ns >=
                          timeline.committed_tick_100ns);
                    CHECK(timeline.writer_accepted);
                }
            } else {
                CHECK(diagnostics.written_audio_packets == 0);
                CHECK(diagnostics.written_audio_frames == 0);
                CHECK(diagnostics.silence_writes == 0);
                CHECK(diagnostics.audio_capture.queue_capacity == 0);
                CHECK(diagnostics.audio_capture.packets_acquired == 0);
            }

            const uint64_t requested_active_milliseconds =
                static_cast<uint64_t>((first_active + second_active).count());
            const uint64_t expected_attempts =
                requested_active_milliseconds * frame_rate / 1000;
            CHECK(diagnostics.capture_attempts + 2 >= expected_attempts);
            if (diagnostics.no_new_frames > 1) {
                CHECK(diagnostics.reused_frames > 0);
            }

            const auto temporary_session = session->temporary_session();
            CHECK(temporary_session.has_value());
            const std::wstring output_path = session->finalized_path();
            CHECK(temporary_session.has_value() &&
                  output_path == temporary_session->recording_path());

            WIN32_FILE_ATTRIBUTE_DATA file_data{};
            const bool file_exists =
                ::GetFileAttributesExW(output_path.c_str(), GetFileExInfoStandard,
                                       &file_data) != FALSE;
            CHECK(file_exists);
            const uint64_t file_size =
                (static_cast<uint64_t>(file_data.nFileSizeHigh) << 32) |
                file_data.nFileSizeLow;
            CHECK(file_size > 0);

            const Mp4Inspection inspection =
                InspectMp4(output_path, width, height, audio_enabled);
            CHECK(inspection.valid);
            CHECK(inspection.sample_durations_valid);
            CHECK(inspection.sample_count == diagnostics.written_frames);
            if (exercise_preview && inspection.valid) {
                CHECK(ExerciseMfPlayPreview(name, output_path));
            }

            const auto measured_active =
                initial_active + (first_active_end - first_active_start) + measured_second_active;
            const auto measured_wall = initial_active + (stop_request - wall_start);
            const uint64_t external_active_100ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    measured_active)
                    .count() /
                100);
            const uint64_t wall_100ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(measured_wall)
                    .count() /
                100);
            const uint64_t frame_tolerance_100ns = 10'000'000 / frame_rate;
            const uint64_t output_delta =
                inspection.duration_100ns > diagnostics.active_duration_100ns
                    ? inspection.duration_100ns -
                          diagnostics.active_duration_100ns
                    : diagnostics.active_duration_100ns -
                          inspection.duration_100ns;
            const uint64_t external_delta =
                inspection.duration_100ns > external_active_100ns
                    ? inspection.duration_100ns - external_active_100ns
                    : external_active_100ns - inspection.duration_100ns;
            CHECK(output_delta <= frame_tolerance_100ns);
            CHECK(external_delta <= frame_tolerance_100ns);
            const uint64_t sample_end_delta =
                inspection.duration_100ns > inspection.last_sample_end_100ns
                    ? inspection.duration_100ns -
                          inspection.last_sample_end_100ns
                    : inspection.last_sample_end_100ns -
                          inspection.duration_100ns;
            CHECK(sample_end_delta <= frame_tolerance_100ns);
            if (audio_enabled) {
                constexpr uint64_t kAudioEndTolerance100ns = 1'000'000;
                const uint64_t audio_end_delta =
                    inspection.duration_100ns >
                            inspection.last_audio_sample_end_100ns
                        ? inspection.duration_100ns -
                              inspection.last_audio_sample_end_100ns
                        : inspection.last_audio_sample_end_100ns -
                              inspection.duration_100ns;
                CHECK(audio_end_delta <= kAudioEndTolerance100ns);
            }
            if (pause_duration > 0ms) {
                const uint64_t pause_100ns =
                    static_cast<uint64_t>(pause_duration.count()) * 10'000;
                CHECK(inspection.duration_100ns + pause_100ns / 2 < wall_100ns);
            }

            const auto& capture = diagnostics.capture_attempt_timing;
            const auto& acquisition = diagnostics.acquisition_timing;
            const auto& copy = diagnostics.region_copy_timing;
            const auto& queue = diagnostics.queue_submission_timing;
            const auto& writer = diagnostics.writer_submission_timing;
            const auto& audio_writer =
                diagnostics.audio_writer_submission_timing;
            const auto& audio_tick =
                diagnostics.silence_write_submission_timing;
            const auto& audio_wait =
                diagnostics.audio_backpressure_wait_timing;
            std::wprintf(
                L"REAL_RECORDING_SCENARIO name=%ls bytes=%llu codec=%ls "
                L"region=%ux%u fps=%u external_active_100ns=%llu "
                L"session_active_100ns=%llu wall_100ns=%llu output_100ns=%llu "
                L"last_sample_end_100ns=%llu samples=%llu audio_samples=%llu "
                L"last_audio_end_100ns=%llu audio_packets=%llu audio_queue=%zu/%zu "
                L"audio_capture_failures=%llu audio_overruns=%llu "
                L"attempts=%llu "
                L"acquired=%llu no_new=%llu reused=%llu failure_reused=%llu "
                L"video_capture_failures=%llu "
                L"pacing_missed=%llu queue_full=%llu accepted=%llu written=%llu "
                L"peak_queue=%zu queue_capacity=%zu bounded_frame_bytes=%llu "
                L"capture_us=%llu/%llu/%llu acquisition_us=%llu/%llu/%llu "
                L"copy_us=%llu/%llu/%llu queue_us=%llu/%llu/%llu "
                L"video_writer_us=%llu/%llu/%llu "
                L"audio_writer_us=%llu/%llu/%llu silence_writes=%llu "
                L"silence_write_us=%llu/%llu/%llu "
                L"audio_wait_us=%llu/%llu/%llu max_gate_wait_us=%llu "
                L"first_audio_input_pts_100ns=%lld threads_stopped=%d\n",
                name, static_cast<unsigned long long>(file_size),
                audio_enabled ? L"H264+AAC" : L"H264", width, height,
                frame_rate, static_cast<unsigned long long>(external_active_100ns),
                static_cast<unsigned long long>(diagnostics.active_duration_100ns),
                static_cast<unsigned long long>(wall_100ns),
                static_cast<unsigned long long>(inspection.duration_100ns),
                static_cast<unsigned long long>(
                    inspection.last_sample_end_100ns),
                static_cast<unsigned long long>(inspection.sample_count),
                static_cast<unsigned long long>(inspection.audio_sample_count),
                static_cast<unsigned long long>(
                    inspection.last_audio_sample_end_100ns),
                static_cast<unsigned long long>(
                    diagnostics.written_audio_packets),
                diagnostics.audio_capture.peak_queue_occupancy,
                diagnostics.audio_capture.queue_capacity,
                static_cast<unsigned long long>(
                    diagnostics.audio_capture.capture_failures),
                static_cast<unsigned long long>(
                    diagnostics.audio_capture.buffer_overruns),
                static_cast<unsigned long long>(diagnostics.capture_attempts),
                static_cast<unsigned long long>(diagnostics.acquired_frames),
                static_cast<unsigned long long>(diagnostics.no_new_frames),
                static_cast<unsigned long long>(diagnostics.reused_frames),
                static_cast<unsigned long long>(
                    diagnostics.failure_reused_frames),
                static_cast<unsigned long long>(diagnostics.capture_failures),
                static_cast<unsigned long long>(diagnostics.pacing_slots_missed),
                static_cast<unsigned long long>(
                    diagnostics.queue_full_rejections),
                static_cast<unsigned long long>(diagnostics.accepted_frames),
                static_cast<unsigned long long>(diagnostics.written_frames),
                diagnostics.peak_queue_depth, diagnostics.queue_capacity,
                static_cast<unsigned long long>(
                    diagnostics.bounded_frame_payload_bytes),
                static_cast<unsigned long long>(capture.minimum_microseconds),
                static_cast<unsigned long long>(capture.average_microseconds()),
                static_cast<unsigned long long>(capture.maximum_microseconds),
                static_cast<unsigned long long>(acquisition.minimum_microseconds),
                static_cast<unsigned long long>(
                    acquisition.average_microseconds()),
                static_cast<unsigned long long>(acquisition.maximum_microseconds),
                static_cast<unsigned long long>(copy.minimum_microseconds),
                static_cast<unsigned long long>(copy.average_microseconds()),
                static_cast<unsigned long long>(copy.maximum_microseconds),
                static_cast<unsigned long long>(queue.minimum_microseconds),
                static_cast<unsigned long long>(queue.average_microseconds()),
                static_cast<unsigned long long>(queue.maximum_microseconds),
                static_cast<unsigned long long>(writer.minimum_microseconds),
                static_cast<unsigned long long>(writer.average_microseconds()),
                static_cast<unsigned long long>(writer.maximum_microseconds),
                static_cast<unsigned long long>(audio_writer.minimum_microseconds),
                static_cast<unsigned long long>(audio_writer.average_microseconds()),
                static_cast<unsigned long long>(audio_writer.maximum_microseconds),
                static_cast<unsigned long long>(diagnostics.silence_writes),
                static_cast<unsigned long long>(audio_tick.minimum_microseconds),
                static_cast<unsigned long long>(audio_tick.average_microseconds()),
                static_cast<unsigned long long>(audio_tick.maximum_microseconds),
                static_cast<unsigned long long>(audio_wait.minimum_microseconds),
                static_cast<unsigned long long>(audio_wait.average_microseconds()),
                static_cast<unsigned long long>(audio_wait.maximum_microseconds),
                static_cast<unsigned long long>(
                    diagnostics.maximum_audio_gate_wait_microseconds),
                static_cast<long long>(
                    diagnostics.first_audio_writer_input_pts_100ns),
                diagnostics.all_worker_threads_stopped ? 1 : 0);

            for (std::size_t index = 0;
                 index < diagnostics.capture_failure_diagnostics.count;
                 ++index) {
                const auto& entry =
                    diagnostics.capture_failure_diagnostics.entries[index];
                std::wprintf(
                    L"CAPTURE_FAILURE scenario=%ls attempt=%llu "
                    L"timestamp_100ns=%lld primary_stage=%ls "
                    L"primary_kind=%u primary_code=0x%08X "
                    L"fallback_stage=%ls fallback_kind=%u "
                    L"fallback_code=0x%08X\n",
                    name,
                    static_cast<unsigned long long>(entry.capture_attempt),
                    static_cast<long long>(entry.timestamp.count()),
                    easnap::capture::RegionCaptureFailureStageName(
                        entry.primary_failure.stage),
                    static_cast<unsigned int>(
                        entry.primary_failure.code_kind),
                    static_cast<unsigned int>(
                        entry.primary_failure.native_code),
                    easnap::capture::RegionCaptureFailureStageName(
                        entry.fallback_failure.stage),
                    static_cast<unsigned int>(
                        entry.fallback_failure.code_kind),
                    static_cast<unsigned int>(
                        entry.fallback_failure.native_code));
            }
            if (diagnostics.capture_failure_diagnostics.omitted != 0) {
                std::wprintf(
                    L"CAPTURE_FAILURE_OMITTED scenario=%ls count=%llu\n",
                    name,
                    static_cast<unsigned long long>(
                        diagnostics.capture_failure_diagnostics.omitted));
            }

            for (std::size_t index = 0;
                 index < diagnostics.audio_capture.timestamp_resolution_count;
                 ++index) {
                const auto& entry =
                    diagnostics.audio_capture.timestamp_resolutions[index];
                std::wprintf(
                    L"AUDIO_CLOCK_RESOLUTION scenario=%ls packet=%llu "
                    L"epoch_100ns=%lld raw_qpc_100ns=%llu "
                    L"audio_clock_pos=%llu audio_clock_qpc_100ns=%llu "
                    L"capture_qpc_100ns=%llu device_pos=%llu frames=%u "
                    L"raw_relative_100ns=%lld clock_relative_100ns=%lld "
                    L"fallback_relative_100ns=%lld resolved_100ns=%lld "
                    L"source=%u flags=0x%X raw_plausible=%u "
                    L"clock_valid=%u\n",
                    name,
                    static_cast<unsigned long long>(entry.packet_index),
                    static_cast<long long>(
                        diagnostics.audio_capture.qpc_epoch_100ns),
                    static_cast<unsigned long long>(
                        entry.raw_wasapi_qpc_100ns),
                    static_cast<unsigned long long>(
                        entry.audio_clock_position),
                    static_cast<unsigned long long>(
                        entry.audio_clock_qpc_100ns),
                    static_cast<unsigned long long>(entry.capture_qpc_100ns),
                    static_cast<unsigned long long>(entry.device_position),
                    entry.frame_count,
                    static_cast<long long>(entry.raw_wasapi_timestamp.count()),
                    static_cast<long long>(entry.audio_clock_timestamp.count()),
                    static_cast<long long>(
                        entry.capture_fallback_timestamp.count()),
                    static_cast<long long>(entry.resolved_timestamp.count()),
                    static_cast<unsigned int>(entry.source),
                    static_cast<unsigned int>(entry.packet_flags),
                    entry.raw_wasapi_plausible ? 1u : 0u,
                    entry.audio_clock_correlation_valid ? 1u : 0u);
            }
            for (std::size_t index = 0;
                 index < diagnostics.audio_packet_timeline_count; ++index) {
                const auto& entry = diagnostics.audio_packet_timeline[index];
                std::wprintf(
                    L"AUDIO_PACKET_TIMELINE scenario=%ls packet=%llu "
                    L"source_pts_100ns=%lld device_pos=%llu frames=%u "
                    L"flags=0x%X source=%u mapped_pts_100ns=%lld "
                    L"duration_100ns=%lld decision=%u qpc_error_100ns=%lld "
                    L"tick_100ns=%lld video_end_100ns=%lld lead_100ns=%lld "
                    L"gate_wait_us=%llu accepted=%u\n",
                    name,
                    static_cast<unsigned long long>(entry.packet_sequence),
                    static_cast<long long>(entry.source_timestamp_100ns),
                    static_cast<unsigned long long>(entry.device_position),
                    entry.source_frame_count, entry.source_flags,
                    static_cast<unsigned int>(entry.timestamp_source),
                    static_cast<long long>(entry.mapped_pts_100ns),
                    static_cast<long long>(entry.mapped_duration_100ns),
                    static_cast<unsigned int>(entry.decision),
                    static_cast<long long>(entry.qpc_error_100ns),
                    static_cast<long long>(entry.committed_tick_100ns),
                    static_cast<long long>(entry.video_end_when_written_100ns),
                    static_cast<long long>(entry.audio_lead_when_written_100ns),
                    static_cast<unsigned long long>(
                        entry.gate_wait_microseconds),
                    entry.writer_accepted ? 1u : 0u);
            }

            std::vector<std::wstring> durable_gif_outputs;
            if (format == easnap::media::RecordingFormat::Gif &&
                inspection.valid && temporary_session.has_value()) {
                auto publication_backend =
                    std::make_unique<ClipboardPublicationProbe>();
                ClipboardPublicationProbe* publication_probe =
                    publication_backend.get();
                easnap::app::RecordingCompletionController completion(
                    *store, std::move(publication_backend));
                const auto export_and_inspect =
                    [&](const std::wstring& destination,
                        easnap::media::MediaTime start,
                        easnap::media::MediaTime end) -> bool {
                        auto exporter = easnap::media::CreateGifExporter();
                        CHECK(exporter != nullptr);
                        if (exporter == nullptr) {
                            return false;
                        }
                        const bool exported = exporter->Export(
                            output_path, destination, start, end, options,
                            [](float) { return true; });
                        if (!exported) {
                            const easnap::media::TrimExportFailure failure =
                                exporter->LastFailure();
                            std::fwprintf(
                                stderr,
                                L"%ls GIF export failed: stage=%u HRESULT=%ls0x%08X detail=%ls\n",
                                name, static_cast<unsigned int>(failure.stage),
                                failure.has_hresult ? L"" : L"n/a ",
                                static_cast<unsigned int>(failure.hresult),
                                failure.detail.c_str());
                        }
                        CHECK(exported);
                        if (!exported) {
                            return false;
                        }
                        const GifInspection gif = InspectGif(destination);
                        if (!gif.valid) {
                            std::fwprintf(
                                stderr,
                                L"%ls WIC GIF inspection failed: path=%ls HRESULT=0x%08X frame=%u frames=%llu size=%ux%u duration_cs=%llu trailer=%u\n",
                                name, destination.c_str(),
                                static_cast<unsigned int>(gif.decode_result),
                                gif.failed_frame,
                                static_cast<unsigned long long>(gif.frame_count),
                                gif.width, gif.height,
                                static_cast<unsigned long long>(
                                    gif.duration_centiseconds),
                                gif.finalized ? 1u : 0u);
                        }
                        CHECK(gif.valid);
                        CHECK(gif.finalized);
                        CHECK(gif.width == width);
                        CHECK(gif.height == height);
                        CHECK(gif.frame_count > 0);
                        const uint64_t selected_ticks = static_cast<uint64_t>(
                            (end - start).count());
                        const uint64_t expected_centiseconds =
                            (selected_ticks + 50'000u) / 100'000u;
                        const uint64_t duration_delta =
                            gif.duration_centiseconds > expected_centiseconds
                                ? gif.duration_centiseconds - expected_centiseconds
                                : expected_centiseconds -
                                      gif.duration_centiseconds;
                        CHECK(duration_delta <= 1);
                        return gif.valid && duration_delta <= 1;
                    };

                const easnap::media::MediaTime full_end{
                    static_cast<int64_t>(inspection.duration_100ns)};
                const easnap::media::MediaTime interior_start{
                    static_cast<int64_t>(inspection.duration_100ns / 4u)};
                const easnap::media::MediaTime interior_end{
                    static_cast<int64_t>((inspection.duration_100ns * 3u) / 4u)};
                CHECK(interior_start > easnap::media::MediaTime::zero());
                CHECK(interior_end < full_end);

                const std::wstring full_save =
                    save_output_directory + L"\\recording-full.gif";
                const std::wstring interior_save =
                    save_output_directory + L"\\recording-interior.gif";
                CHECK(completion.IsAllowedSaveDestination(
                    full_save, easnap::media::RecordingFormat::Gif, false));
                CHECK(completion.IsAllowedSaveDestination(
                    interior_save, easnap::media::RecordingFormat::Gif, false));

                const std::wstring failed_save =
                    save_output_directory + L"\\recording-invalid.gif";
                auto failed_exporter = easnap::media::CreateGifExporter();
                CHECK(failed_exporter != nullptr);
                CHECK(failed_exporter != nullptr &&
                      !failed_exporter->Export(
                          output_path, failed_save,
                          easnap::media::MediaTime::zero(),
                          full_end + easnap::media::MediaTime{1}, options,
                          [](float) { return true; }));
                if (failed_exporter != nullptr) {
                    CHECK(failed_exporter->LastFailure().stage ==
                          easnap::media::TrimExportFailureStage::SourceValidation);
                }
                CHECK(::GetFileAttributesW(output_path.c_str()) !=
                      INVALID_FILE_ATTRIBUTES);
                CHECK(::GetFileAttributesW(failed_save.c_str()) ==
                      INVALID_FILE_ATTRIBUTES);
                CHECK(!HasGifPartialFiles(save_output_directory));

                CHECK(export_and_inspect(full_save,
                                         easnap::media::MediaTime::zero(),
                                         full_end));
                CHECK(export_and_inspect(interior_save, interior_start,
                                         interior_end));
                durable_gif_outputs.push_back(full_save);
                durable_gif_outputs.push_back(interior_save);

                const auto export_and_publish_copy =
                    [&](easnap::media::MediaTime start,
                        easnap::media::MediaTime end) -> std::wstring {
                        const auto destination =
                            completion.PrepareClipboardDestination(
                                easnap::media::RecordingFormat::Gif);
                        CHECK(destination.has_value());
                        if (!destination.has_value()) {
                            return {};
                        }
                        CHECK(completion.IsPreparedClipboardArtifact(
                            *destination));
                        if (!export_and_inspect(*destination, start, end)) {
                            completion.AbandonPreparedClipboardArtifact();
                            return {};
                        }
                        CHECK(completion.PublishClipboardArtifact(
                            nullptr, *destination,
                            easnap::media::RecordingFormat::Gif));
                        CHECK(completion.clipboard_session().has_value());
                        easnap::platform::UniqueHandle readable{
                            ::CreateFileW(destination->c_str(), GENERIC_READ,
                                          FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                          nullptr)};
                        CHECK(readable.get() != nullptr &&
                              readable.get() != INVALID_HANDLE_VALUE);
                        return *destination;
                    };

                const std::wstring full_copy = export_and_publish_copy(
                    easnap::media::MediaTime::zero(), full_end);
                CHECK(!full_copy.empty());
                const std::wstring interior_copy =
                    export_and_publish_copy(interior_start, interior_end);
                CHECK(!interior_copy.empty());
                CHECK(full_copy.empty() ||
                      ::GetFileAttributesW(full_copy.c_str()) ==
                          INVALID_FILE_ATTRIBUTES);
                CHECK(publication_probe->persist_calls == 0);
                CHECK(publication_probe->move_calls == 0);
                CHECK(publication_probe->security_calls == 4);
                CHECK(publication_probe->clipboard_calls == 2);
                publication_probe->clipboard_paths =
                    std::vector<std::wstring>{};
                completion.OnClipboardChanged();
                CHECK(interior_copy.empty() ||
                      ::GetFileAttributesW(interior_copy.c_str()) ==
                          INVALID_FILE_ATTRIBUTES);

                options.gif_fps = 12;
                options.gif_palette_colors = 256;
                options.gif_dither = true;
                options.gif_low_size = true;
                options.gif_sparse_transparency = true;
                options.gif_distributed_cadence = true;
                const std::wstring low_full_save =
                    save_output_directory + L"\\recording-low-full.gif";
                const std::wstring low_interior_save =
                    save_output_directory + L"\\recording-low-interior.gif";
                CHECK(export_and_inspect(low_full_save,
                                         easnap::media::MediaTime::zero(),
                                         full_end));
                CHECK(export_and_inspect(low_interior_save, interior_start,
                                         interior_end));
                durable_gif_outputs.push_back(low_full_save);
                durable_gif_outputs.push_back(low_interior_save);

                const std::wstring low_full_copy = export_and_publish_copy(
                    easnap::media::MediaTime::zero(), full_end);
                CHECK(!low_full_copy.empty());
                const std::wstring low_interior_copy =
                    export_and_publish_copy(interior_start, interior_end);
                CHECK(!low_interior_copy.empty());
                CHECK(low_full_copy.empty() ||
                      ::GetFileAttributesW(low_full_copy.c_str()) ==
                          INVALID_FILE_ATTRIBUTES);
                CHECK(publication_probe->persist_calls == 0);
                CHECK(publication_probe->move_calls == 0);
                CHECK(publication_probe->security_calls == 8);
                CHECK(publication_probe->clipboard_calls == 4);
                publication_probe->clipboard_paths =
                    std::vector<std::wstring>{};
                completion.OnClipboardChanged();
                CHECK(low_interior_copy.empty() ||
                      ::GetFileAttributesW(low_interior_copy.c_str()) ==
                          INVALID_FILE_ATTRIBUTES);
            }

            CHECK(temporary_session.has_value() &&
                  store->DeleteSession(*temporary_session));
            CHECK(::GetFileAttributesW(output_path.c_str()) ==
                  INVALID_FILE_ATTRIBUTES);
            for (const std::wstring& destination : durable_gif_outputs) {
                CHECK(InspectGif(destination).valid);
                CHECK(::DeleteFileW(destination.c_str()) != FALSE);
            }
        };

    run_scenario(L"static-15fps", false, false, 15,
                 easnap::media::AudioSource::None, 3000ms, 0ms, 0ms);
    run_scenario(L"changing-15fps", true, true, 15,
                 easnap::media::AudioSource::None, 3000ms, 0ms, 0ms);
    run_scenario(L"pause-resume-15fps", true, true, 15,
                 easnap::media::AudioSource::None, 2000ms, 1000ms, 2000ms);
    run_scenario(L"changing-30fps", true, true, 30,
                 easnap::media::AudioSource::None, 3000ms, 0ms, 0ms);
    run_scenario(L"gif-working-pause-resume-15fps", true, true, 15,
                 easnap::media::AudioSource::None, 1500ms, 750ms, 1500ms,
                 easnap::media::RecordingFormat::Gif, true);

    auto audio_probe = easnap::media::CreateWasapiAudioCaptureProvider();
    CHECK(audio_probe != nullptr);
    if (audio_probe != nullptr &&
        audio_probe->IsSourceAvailable(easnap::media::AudioSource::SystemAudio)) {
        run_scenario(L"av-system-pause-resume-15fps", true, true, 15,
                     easnap::media::AudioSource::SystemAudio, 2000ms, 1000ms,
                     2000ms, easnap::media::RecordingFormat::Mp4, true);
    } else {
        std::printf("SKIPPED  system-audio A/V session: endpoint unavailable\n");
    }
    if (audio_probe != nullptr &&
        audio_probe->IsSourceAvailable(easnap::media::AudioSource::Microphone)) {
        run_scenario(L"av-microphone-15fps", true, true, 15,
                     easnap::media::AudioSource::Microphone, 2000ms, 0ms, 0ms);
        // Continuous microphone packets make the Pause boundary exercise the
        // production in-flight audio drain instead of relying on sparse
        // loopback ticks alone.
        run_scenario(L"av-microphone-pause-resume-15fps", true, true, 15,
                     easnap::media::AudioSource::Microphone, 2000ms, 1000ms,
                     2000ms, easnap::media::RecordingFormat::Mp4, true);
    } else {
        std::printf("SKIPPED  microphone A/V session: endpoint unavailable\n");
    }
    if (audio_probe != nullptr &&
        audio_probe->IsSourceAvailable(easnap::media::AudioSource::Both)) {
        run_scenario(L"av-both-pause-resume-15fps", true, true, 15,
                     easnap::media::AudioSource::Both, 2000ms, 1000ms,
                     2000ms, easnap::media::RecordingFormat::Mp4, true);
    } else {
        std::printf("SKIPPED  Both A/V session: one or both endpoints unavailable\n");
    }
    easnap::media::RecordingOptions invalid_options;
    invalid_options.region = region;
    invalid_options.region.right = invalid_options.region.left;
    invalid_options.format = easnap::media::RecordingFormat::Mp4;
    invalid_options.audio = easnap::media::AudioSource::None;
    invalid_options.target_frame_rate = 15;
    auto failed_start =
        easnap::recording::CreateVideoRecordingSession(*store, invalid_options);
    CHECK(failed_start != nullptr);
    if (failed_start != nullptr) {
        CHECK(!failed_start->Start());
        CHECK(failed_start->state() ==
              easnap::recording::RecordingSessionState::Failed);
        CHECK(failed_start->diagnostics().all_worker_threads_stopped);
        const auto failed_temporary_session = failed_start->temporary_session();
        CHECK(failed_temporary_session.has_value());
        CHECK(failed_temporary_session.has_value() &&
              store->DeleteSession(*failed_temporary_session));
    }

    easnap::media::RecordingOptions invalid_gif_audio;
    invalid_gif_audio.region = region;
    invalid_gif_audio.format = easnap::media::RecordingFormat::Gif;
    invalid_gif_audio.audio = easnap::media::AudioSource::SystemAudio;
    CHECK(easnap::recording::CreateVideoRecordingSession(
              *store, invalid_gif_audio) == nullptr);

    failed_start.reset();
    store.reset();
    CHECK(::RemoveDirectoryW(save_output_directory.c_str()) != FALSE);
    easnap::platform::GraphicsEnvironment::Get().Shutdown();
}

void TestPauseStopStress() {
    using namespace std::chrono_literals;
    using namespace easnap;
    MediaFoundationRuntime runtime;
    CHECK(SUCCEEDED(runtime.result()));
    if (FAILED(runtime.result())) return;
    platform::EnableDpiAwareness();
    CHECK(platform::GraphicsEnvironment::Get().EnsureInitialized());
    auto store = media::CreateTemporarySessionStore();
    auto collector = media::CreateTemporarySessionStore();
    const auto screen = platform::VirtualScreenBounds();
    auto probe = media::CreateWasapiAudioCaptureProvider();
    constexpr media::AudioSource sources[]{media::AudioSource::None,
        media::AudioSource::SystemAudio, media::AudioSource::Microphone,
        media::AudioSource::Both};
    for (const auto source : sources) {
        if (source != media::AudioSource::None && !probe->IsSourceAvailable(source)) {
            std::printf("SKIPPED stress source=%d: endpoint unavailable\n",
                        static_cast<int>(source));
            continue;
        }
        for (int cycle = 0; cycle < 8; ++cycle) {
            media::RecordingOptions options;
            options.region = {screen.left, screen.top, screen.left + 160,
                              screen.top + 120};
            options.audio = source;
            options.include_cursor = false;
            options.target_frame_rate = cycle % 2 == 0 ? 15 : 30;
            auto session = recording::CreateVideoRecordingSession(*store, options);
            CHECK(session != nullptr);
            if (!session) continue;
            const bool started = session->Start();
            CHECK(started);
            if (!started) {
                std::fwprintf(stderr, L"STRESS Start: %ls\n", session->last_error().c_str());
                continue;
            }
            const auto temporary = session->temporary_session();
            CHECK(temporary.has_value());
            if (!temporary) {
                session->Abort();
                continue;
            }
            const bool immediate = cycle >= 6;
            if (!immediate) std::this_thread::sleep_for(180ms);
            for (int boundary = 0; boundary < (immediate ? 1 : 4); ++boundary) {
                CHECK(session->Pause());
                const auto paused_duration = session->diagnostics().active_duration_100ns;
                std::this_thread::sleep_for(20ms);
                CHECK(session->diagnostics().active_duration_100ns == paused_duration);
                // A separate store models application startup while the first
                // process is paused. Protect unrelated user/test recordings.
                auto protected_directories = collector->EnumerateSessions();
                std::erase(protected_directories, temporary->directory);
                CHECK(collector->CleanupStaleSessions(protected_directories) == 0);
                if (!immediate && (boundary != 3 || cycle % 2 != 0)) {
                    CHECK(session->Resume());
                    std::this_thread::sleep_for(70ms);
                }
            }
            const bool abort = cycle == 4 || cycle == 5;
            bool finished = true;
            if (abort) session->Abort();
            else finished = session->Stop();
            CHECK(finished);
            if (!finished) std::fwprintf(stderr, L"STRESS Stop: %ls\n",
                                         session->last_error().c_str());
            const auto diagnostics = session->diagnostics();
            CHECK(diagnostics.all_worker_threads_stopped);
            CHECK(diagnostics.peak_queue_depth <= diagnostics.queue_capacity);
            CHECK(diagnostics.audio_capture.capture_failures == 0);
            CHECK(diagnostics.capture_failures == 0);
            if (!abort && finished) {
                const auto output = InspectMp4(session->finalized_path(), 160, 120,
                                              source != media::AudioSource::None);
                CHECK(output.valid);
                CHECK(output.sample_durations_valid);
                CHECK(output.sample_count == diagnostics.written_frames);
                const auto delta = output.duration_100ns > diagnostics.active_duration_100ns
                    ? output.duration_100ns - diagnostics.active_duration_100ns
                    : diagnostics.active_duration_100ns - output.duration_100ns;
                CHECK(delta < 1'000'000);  // one 15-fps frame + AAC endpoint granularity
                CHECK(session->Stop());  // completion is idempotent
                std::printf("PAUSE_STOP_STRESS source=%d cycle=%d active=%llu output=%llu frames=%llu audio=%llu stop_paused=%d\n",
                    static_cast<int>(source), cycle,
                    static_cast<unsigned long long>(diagnostics.active_duration_100ns),
                    static_cast<unsigned long long>(output.duration_100ns),
                    static_cast<unsigned long long>(output.sample_count),
                    static_cast<unsigned long long>(output.audio_sample_count),
                    immediate || cycle % 2 == 0);
            } else if (abort) {
                CHECK(session->state() == recording::RecordingSessionState::Aborted);
                session->Abort();
            }
            session.reset();
            CHECK(store->DeleteSession(*temporary));
        }
    }
    platform::GraphicsEnvironment::Get().Shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--stress") == 0) TestPauseStopStress();
    else TestRealDesktopRecordingSession();
    if (g_fail == 0) {
        std::printf("PASSED  %d/%d checks\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
