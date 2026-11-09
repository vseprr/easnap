#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media/media_foundation_writer.h"
#include "media/temp_session.h"
#include "platform/platform_utils.h"
#include "sink_writer_fixture.h"

namespace {

using Microsoft::WRL::ComPtr;
using easnap::media::MediaTime;

constexpr int64_t kSecond = 10'000'000;
constexpr UINT32 kWidth = 160;
// Not a multiple of 16: the decoder pads such frames, so trimming must keep
// the declared display size rather than the padded buffer size.
constexpr UINT32 kHeight = 120;
constexpr uint32_t kFrameRate = 30;
constexpr uint32_t kAudioSampleRate = 48'000;
constexpr uint16_t kAudioChannels = 2;
constexpr uint64_t kFrameCount = 120;
// Mean per-channel error allowed between a re-encoded frame and its source
// pattern. Lossy coding stays far below it; a wrong picture is far above.
constexpr double kPictureErrorLimit = 12.0;

int g_pass = 0;
int g_fail = 0;
// Log lines that say the exporter left the recording path: a decoded picture
// converted through RGB, or silence written into the audio track.
std::atomic<int> g_colour_conversions{0};
std::atomic<int> g_silence_fills{0};

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
        result_ = com_initialized_ ? ::MFStartup(MF_VERSION, MFSTARTUP_FULL)
                                   : com_result;
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

struct FileFingerprint final {
    uint64_t bytes{0};
    uint64_t hash{1469598103934665603ull};

    [[nodiscard]] bool operator==(const FileFingerprint&) const = default;
};

std::optional<FileFingerprint> FingerprintFile(const std::wstring& path) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    FileFingerprint fingerprint;
    std::array<uint8_t, 64 * 1024> buffer{};
    bool succeeded = true;
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                        &read, nullptr)) {
            succeeded = false;
            break;
        }
        if (read == 0) {
            break;
        }
        fingerprint.bytes += read;
        for (DWORD index = 0; index < read; ++index) {
            fingerprint.hash ^= buffer[index];
            fingerprint.hash *= 1099511628211ull;
        }
    }
    ::CloseHandle(file);
    return succeeded ? std::optional<FileFingerprint>(fingerprint) : std::nullopt;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int64_t FrameTimestamp(uint64_t frame) {
    return static_cast<int64_t>(frame * kSecond / kFrameRate);
}

int64_t AudioFrameTimestamp(uint64_t frame) {
    return static_cast<int64_t>(frame * kSecond / kAudioSampleRate);
}

void FillMovingPattern(easnap::platform::BgraImage& image, uint32_t frame_index) {
    for (int y = 0; y < image.height; ++y) {
        uint32_t* const row = image.row(y);
        for (int x = 0; x < image.width; ++x) {
            const uint32_t red = static_cast<uint32_t>((x + frame_index * 3) & 0xFF);
            const uint32_t green = static_cast<uint32_t>((y + frame_index * 5) & 0xFF);
            const uint32_t blue = static_cast<uint32_t>((x + y + frame_index * 7) & 0xFF);
            row[x] = 0xFF000000u | (red << 16) | (green << 8) | blue;
        }
    }
}

easnap::media::RecordingOptions TestOptions(bool audio) {
    easnap::media::RecordingOptions options;
    options.region = RECT{0, 0, static_cast<LONG>(kWidth),
                          static_cast<LONG>(kHeight)};
    options.format = easnap::media::RecordingFormat::Mp4;
    options.audio = audio ? easnap::media::AudioSource::SystemAudio
                          : easnap::media::AudioSource::None;
    options.target_frame_rate = kFrameRate;
    options.video_bitrate_bps = 1'000'000;
    options.audio_bitrate_bps = 192'000;
    return options;
}

/// `audio_frames` limits the audio to the first that many video frames' time,
/// for a source whose audio ends before its video.
bool CreateSourceMp4(const std::wstring& path, bool with_audio,
                     uint64_t audio_frames = kFrameCount) {
    auto writer = easnap::media::CreateMediaFoundationWriter();
    if (writer == nullptr) {
        return false;
    }

    easnap::media::AudioFormat audio_format;
    audio_format.sample_rate = kAudioSampleRate;
    audio_format.channels = kAudioChannels;
    audio_format.bits_per_sample = 32;
    audio_format.is_float = true;
    audio_format.channel_mask = 0x3;

    const auto options = TestOptions(with_audio);
    if (!writer->Open(path, options, with_audio ? &audio_format : nullptr)) {
        std::fwprintf(stderr, L"Source writer open failed: %ls\n",
                      writer->last_error().c_str());
        return false;
    }

    auto image = std::make_shared<easnap::platform::BgraImage>();
    image->width = static_cast<int>(kWidth);
    image->height = static_cast<int>(kHeight);
    image->pixels.resize(static_cast<std::size_t>(kWidth) * kHeight);

    constexpr uint64_t kAudioFramesPerVideoFrame =
        kAudioSampleRate / kFrameRate;
    std::vector<float> audio(
        static_cast<std::size_t>(kAudioFramesPerVideoFrame) * kAudioChannels);
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr double kToneHz = 440.0;

    for (uint64_t frame_index = 0; frame_index < kFrameCount; ++frame_index) {
        FillMovingPattern(*image, static_cast<uint32_t>(frame_index));
        const int64_t video_start = FrameTimestamp(frame_index);
        const int64_t video_end = FrameTimestamp(frame_index + 1);
        const easnap::media::VideoFrame frame{
            image, MediaTime{video_start}, MediaTime{video_end - video_start}};
        if (!writer->WriteVideoFrame(frame)) {
            std::fwprintf(stderr, L"Source video write failed: %ls\n",
                          writer->last_error().c_str());
            return false;
        }

        if (with_audio && frame_index < audio_frames) {
            const uint64_t first_audio_frame =
                frame_index * kAudioFramesPerVideoFrame;
            for (uint64_t offset = 0; offset < kAudioFramesPerVideoFrame; ++offset) {
                const double phase = kTwoPi * kToneHz *
                                     static_cast<double>(first_audio_frame + offset) /
                                     kAudioSampleRate;
                const float value = static_cast<float>(std::sin(phase) * 0.25);
                for (uint16_t channel = 0; channel < kAudioChannels; ++channel) {
                    audio[static_cast<std::size_t>(offset) * kAudioChannels +
                          channel] = value;
                }
            }
            const int64_t audio_start = AudioFrameTimestamp(first_audio_frame);
            const int64_t audio_end = AudioFrameTimestamp(
                first_audio_frame + kAudioFramesPerVideoFrame);
            const easnap::media::MediaChunk packet{
                reinterpret_cast<const uint8_t*>(audio.data()),
                audio.size() * sizeof(float), MediaTime{audio_start},
                MediaTime{audio_end - audio_start}, false, false};
            if (!writer->WriteAudioPacket(packet)) {
                std::fwprintf(stderr, L"Source audio write failed: %ls\n",
                              writer->last_error().c_str());
                return false;
            }
        }
    }

    std::wstring finalized_path;
    if (!writer->Finalize(finalized_path)) {
        std::fwprintf(stderr, L"Source finalize failed: %ls\n",
                      writer->last_error().c_str());
        return false;
    }
    return finalized_path == path && FileExists(path);
}

struct Mp4Inspection final {
    bool valid{false};
    bool has_audio{false};
    uint64_t presentation_duration{0};
    int64_t first_video_time{-1};
    int64_t video_end{0};
    uint64_t video_samples{0};
    int64_t first_audio_time{-1};
    int64_t audio_end{0};
    uint64_t audio_samples{0};
};

std::optional<uint64_t> PresentationDuration(IMFSourceReader* reader) {
    PROPVARIANT value;
    ::PropVariantInit(&value);
    const HRESULT result = reader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value);
    std::optional<uint64_t> duration;
    if (SUCCEEDED(result) && value.vt == VT_UI8) {
        duration = value.uhVal.QuadPart;
    } else if (SUCCEEDED(result) && value.vt == VT_I8 && value.hVal.QuadPart >= 0) {
        duration = static_cast<uint64_t>(value.hVal.QuadPart);
    }
    ::PropVariantClear(&value);
    return duration;
}

bool ReadDecodedEndpoint(IMFSourceReader* reader, DWORD stream,
                         int64_t fallback_duration, int64_t& first_time,
                         int64_t& endpoint, uint64_t& sample_count) {
    for (;;) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        const HRESULT result = reader->ReadSample(stream, 0, nullptr, &flags,
                                                  &time, &sample);
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            return false;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            return true;
        }
        if (sample == nullptr) {
            continue;
        }
        LONGLONG duration = 0;
        if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) {
            duration = fallback_duration;
        }
        if (first_time < 0) {
            first_time = time;
        }
        endpoint = (std::max)(endpoint, time + duration);
        ++sample_count;
    }
}

Mp4Inspection InspectMp4(const std::wstring& path) {
    Mp4Inspection inspection;
    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                                       TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(result)) {
        result = ::MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(),
                                               &reader);
    }
    if (FAILED(result)) {
        return inspection;
    }

    ComPtr<IMFMediaType> video_type;
    result = reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
        &video_type);
    GUID major{};
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(result)) {
        result = video_type->GetGUID(MF_MT_MAJOR_TYPE, &major);
    }
    if (SUCCEEDED(result)) {
        result = video_type->GetGUID(MF_MT_SUBTYPE, &subtype);
    }
    if (SUCCEEDED(result)) {
        result = ::MFGetAttributeSize(video_type.Get(), MF_MT_FRAME_SIZE,
                                      &width, &height);
    }
    const auto duration = PresentationDuration(reader.Get());
    if (FAILED(result) || major != MFMediaType_Video ||
        subtype != MFVideoFormat_H264 || width != kWidth || height != kHeight ||
        !duration.has_value()) {
        return inspection;
    }
    inspection.presentation_duration = *duration;

    ComPtr<IMFMediaType> audio_type;
    result = reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
        &audio_type);
    if (SUCCEEDED(result)) {
        GUID audio_major{};
        GUID audio_subtype{};
        result = audio_type->GetGUID(MF_MT_MAJOR_TYPE, &audio_major);
        if (SUCCEEDED(result)) {
            result = audio_type->GetGUID(MF_MT_SUBTYPE, &audio_subtype);
        }
        if (FAILED(result) || audio_major != MFMediaType_Audio ||
            audio_subtype != MFAudioFormat_AAC) {
            return inspection;
        }
        inspection.has_audio = true;
    } else if (result != MF_E_INVALIDSTREAMNUMBER) {
        return inspection;
    }

    ComPtr<IMFMediaType> decoded_video;
    result = ::MFCreateMediaType(&decoded_video);
    if (SUCCEEDED(result)) {
        result = decoded_video->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = decoded_video->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(result)) {
        result = reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
            decoded_video.Get());
    }
    if (FAILED(result) ||
        !ReadDecodedEndpoint(reader.Get(),
                             static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                             kSecond / kFrameRate, inspection.first_video_time,
                             inspection.video_end, inspection.video_samples)) {
        return inspection;
    }

    if (inspection.has_audio) {
        ComPtr<IMFSourceReader> audio_reader;
        result = ::MFCreateSourceReaderFromURL(path.c_str(), nullptr, &audio_reader);
        ComPtr<IMFMediaType> decoded_audio;
        if (SUCCEEDED(result)) {
            result = ::MFCreateMediaType(&decoded_audio);
        }
        if (SUCCEEDED(result)) {
            result = decoded_audio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        }
        if (SUCCEEDED(result)) {
            result = decoded_audio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        }
        if (SUCCEEDED(result)) {
            result = decoded_audio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        }
        if (SUCCEEDED(result)) {
            result = audio_reader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
                decoded_audio.Get());
        }
        constexpr int64_t kAacFrameDuration =
            static_cast<int64_t>(1024ull * kSecond / kAudioSampleRate);
        if (FAILED(result) ||
            !ReadDecodedEndpoint(
                audio_reader.Get(),
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                kAacFrameDuration, inspection.first_audio_time,
                inspection.audio_end, inspection.audio_samples)) {
            return inspection;
        }
    }

    inspection.valid = inspection.video_samples > 0 &&
                       (!inspection.has_audio || inspection.audio_samples > 0);
    return inspection;
}

/// Decodes the video frame of `path` shown at `at`, at its display size.
bool DecodeFrameAt(const std::wstring& path, MediaTime at,
                   easnap::platform::BgraImage& image) {
    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(result)) {
        result = ::MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
    }
    ComPtr<IMFMediaType> decoded;
    if (SUCCEEDED(result)) {
        result = ::MFCreateMediaType(&decoded);
    }
    if (SUCCEEDED(result)) {
        result = decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    if (SUCCEEDED(result)) {
        result = reader->SetCurrentMediaType(stream, nullptr, decoded.Get());
    }
    ComPtr<IMFMediaType> actual;
    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(result)) {
        result = reader->GetCurrentMediaType(stream, &actual);
    }
    if (SUCCEEDED(result)) {
        result = ::MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height);
    }
    if (FAILED(result) || width != kWidth || height != kHeight) {
        return false;
    }
    for (;;) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        result = reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample);
        if (FAILED(result) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) != 0) {
            return false;
        }
        if (sample == nullptr || time < at.count() - kSecond / kFrameRate / 2) {
            continue;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            return false;
        }
        image.width = static_cast<int>(width);
        image.height = static_cast<int>(height);
        image.pixels.resize(static_cast<std::size_t>(width) * height);
        const auto copy_rows = [&](const BYTE* top_row, int64_t pitch) {
            for (UINT32 row = 0; row < height; ++row) {
                std::memcpy(image.row(static_cast<int>(row)), top_row + row * pitch,
                            static_cast<std::size_t>(width) * 4u);
            }
        };
        ComPtr<IMF2DBuffer> buffer_2d;
        if (SUCCEEDED(buffer.As(&buffer_2d))) {
            BYTE* scanline = nullptr;
            LONG pitch = 0;
            if (FAILED(buffer_2d->Lock2D(&scanline, &pitch))) {
                return false;
            }
            copy_rows(scanline, pitch);
            return SUCCEEDED(buffer_2d->Unlock2D());
        }
        // A plain memory buffer: the media type says whether rows run bottom-up.
        UINT32 raw_stride = width * 4u;
        static_cast<void>(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride));
        const int64_t stride = static_cast<LONG>(raw_stride);
        const uint64_t row_bytes = static_cast<uint64_t>(stride < 0 ? -stride : stride);
        BYTE* bytes = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&bytes, nullptr, &length))) {
            return false;
        }
        const bool fits = row_bytes >= width * 4u && row_bytes * height <= length;
        if (fits) {
            copy_rows(stride < 0 ? bytes + row_bytes * (height - 1u) : bytes, stride);
        }
        return SUCCEEDED(buffer->Unlock()) && fits;
    }
}

/// Mean absolute difference per colour channel over rows [first_row, last_row).
double MeanChannelError(const easnap::platform::BgraImage& actual,
                        const easnap::platform::BgraImage& expected,
                        int first_row, int last_row) {
    uint64_t total = 0;
    for (int y = first_row; y < last_row; ++y) {
        for (int x = 0; x < actual.width; ++x) {
            const uint32_t a = actual.row(y)[x];
            const uint32_t e = expected.row(y)[x];
            for (int shift = 0; shift <= 16; shift += 8) {
                total += static_cast<uint64_t>(std::abs(
                    static_cast<int>((a >> shift) & 0xFF) -
                    static_cast<int>((e >> shift) & 0xFF)));
            }
        }
    }
    const double samples =
        static_cast<double>(last_row - first_row) * actual.width * 3.0;
    return samples > 0 ? static_cast<double>(total) / samples : 0.0;
}

bool DecodeFirstFrame(const std::wstring& path, easnap::platform::BgraImage& image) {
    return DecodeFrameAt(path, MediaTime{0}, image);
}

/// A trim must carry the picture itself: a decoder buffer padded beyond the
/// display size, or a misplaced chroma plane, shows up as a large error here.
void CheckTrimmedPicture(const std::wstring& trimmed_path, MediaTime start) {
    easnap::platform::BgraImage trimmed;
    CHECK(DecodeFirstFrame(trimmed_path, trimmed));
    if (trimmed.empty()) {
        return;
    }
    easnap::platform::BgraImage expected;
    expected.width = static_cast<int>(kWidth);
    expected.height = static_cast<int>(kHeight);
    expected.pixels.resize(static_cast<std::size_t>(kWidth) * kHeight);
    const int64_t start_frame = start.count() * kFrameRate / kSecond;
    double whole = 255.0;
    double bottom = 255.0;
    // The first exported frame is the one at the trim start; allow its neighbours
    // because the codec may move a frame boundary by one frame.
    for (int64_t frame = start_frame - 1; frame <= start_frame + 1; ++frame) {
        FillMovingPattern(expected, static_cast<uint32_t>(frame));
        const double error =
            MeanChannelError(trimmed, expected, 0, static_cast<int>(kHeight));
        if (error < whole) {
            whole = error;
            bottom = MeanChannelError(trimmed, expected,
                                      static_cast<int>(kHeight) - 8,
                                      static_cast<int>(kHeight));
        }
    }
    std::printf("trimmed picture error: whole %.2f, bottom rows %.2f\n", whole, bottom);
    CHECK(whole < kPictureErrorLimit);
    CHECK(bottom < kPictureErrorLimit);
}

/// The same for a source whose own coding already changed the pattern: the
/// trim's first frame is compared with the source frame it was made from.
void CheckTrimmedPictureAgainstSource(const std::wstring& trimmed_path,
                                      const std::wstring& source_path, MediaTime start) {
    easnap::platform::BgraImage trimmed;
    easnap::platform::BgraImage source;
    CHECK(DecodeFirstFrame(trimmed_path, trimmed));
    CHECK(DecodeFrameAt(source_path, start, source));
    if (trimmed.empty() || source.empty()) {
        return;
    }
    const double whole = MeanChannelError(trimmed, source, 0, static_cast<int>(kHeight));
    const double bottom = MeanChannelError(trimmed, source, static_cast<int>(kHeight) - 8,
                                           static_cast<int>(kHeight));
    std::printf("trimmed picture error against the source frame: whole %.2f, bottom rows %.2f\n",
                whole, bottom);
    CHECK(whole < kPictureErrorLimit);
    CHECK(bottom < kPictureErrorLimit);
}

void CheckDurationNear(const Mp4Inspection& inspection, MediaTime expected,
                       int64_t tolerance) {
    const int64_t actual = static_cast<int64_t>(inspection.presentation_duration);
    CHECK(actual >= expected.count() - tolerance);
    CHECK(actual <= expected.count() + tolerance);
    CHECK(inspection.video_end >= expected.count() - tolerance);
    CHECK(inspection.video_end <= expected.count() + tolerance);
}

void CheckNoPartialFiles(const std::wstring& directory,
                         const std::wstring& destination_name) {
    const std::wstring pattern = directory + L"\\" + destination_name +
                                 L".easnap-*.partial.mp4";
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    CHECK(find == INVALID_HANDLE_VALUE);
    if (find != INVALID_HANDLE_VALUE) {
        ::FindClose(find);
    }
}

void TestMp4TrimExporter() {
    MediaFoundationRuntime runtime;
    CHECK(SUCCEEDED(runtime.result()));
    if (FAILED(runtime.result())) {
        return;
    }

    auto store = easnap::media::CreateTemporarySessionStore();
    CHECK(store != nullptr);
    const auto session = store != nullptr ? store->CreateSession() : std::nullopt;
    CHECK(session.has_value());
    if (!session.has_value()) {
        return;
    }

    const std::wstring video_source = session->directory + L"\\source-video.mp4";
    const std::wstring av_source = session->directory + L"\\source-av.mp4";
    CHECK(CreateSourceMp4(video_source, false));
    CHECK(CreateSourceMp4(av_source, true));
    if (!FileExists(video_source) || !FileExists(av_source)) {
        return;
    }
    const auto video_source_before = FingerprintFile(video_source);
    const auto av_source_before = FingerprintFile(av_source);
    CHECK(video_source_before.has_value());
    CHECK(av_source_before.has_value());

    auto exporter = easnap::media::CreateMp4TrimExporter();
    CHECK(exporter != nullptr);
    CHECK(exporter != nullptr &&
          !exporter->EstimateOutputBytes(video_source, MediaTime{0},
                                         MediaTime{kSecond},
                                         TestOptions(false))
               .has_value());
    if (exporter == nullptr) {
        return;
    }

    const MediaTime interior_start{kSecond};
    const MediaTime interior_end{3 * kSecond};
    const MediaTime expected_interior = interior_end - interior_start;
    constexpr int64_t kVideoTolerance = 2 * kSecond / kFrameRate;
    constexpr int64_t kAvTolerance = 1'000'000;

    const std::wstring video_interior =
        session->directory + L"\\video-interior.mp4";
    std::vector<float> progress_values;
    CHECK(exporter->Export(
        video_source, video_interior, interior_start, interior_end,
        TestOptions(false), [&progress_values](float value) {
            progress_values.push_back(value);
            return true;
        }));
    const easnap::media::TrimExportDiagnostics trim_diagnostics =
        exporter->LastDiagnostics();
    CHECK(trim_diagnostics.video_pipeline !=
          easnap::media::TrimVideoPipeline::Unknown);
    CHECK(trim_diagnostics.total_ms >= trim_diagnostics.source_open_ms);
    CHECK(trim_diagnostics.total_ms >= trim_diagnostics.decode_process_ms);
    CHECK(trim_diagnostics.total_ms >= trim_diagnostics.encode_finalize_ms);
    std::printf("trim video pipeline: %d, adapter: %ls, total ms: %llu\n",
                static_cast<int>(trim_diagnostics.video_pipeline),
                trim_diagnostics.adapter_name.empty()
                    ? L"not-applicable"
                    : trim_diagnostics.adapter_name.c_str(),
                static_cast<unsigned long long>(trim_diagnostics.total_ms));
    CHECK(FileExists(video_interior));
    CHECK(!progress_values.empty());
    CHECK(progress_values.front() == 0.0f);
    CHECK(progress_values.back() == 1.0f);
    CHECK(std::is_sorted(progress_values.begin(), progress_values.end()));
    const Mp4Inspection video_inspection = InspectMp4(video_interior);
    CHECK(video_inspection.valid);
    CHECK(!video_inspection.has_audio);
    CHECK(video_inspection.first_video_time >= 0);
    CHECK(video_inspection.first_video_time <= kSecond / kFrameRate);
    CheckDurationNear(video_inspection, expected_interior, kVideoTolerance);
    CheckTrimmedPicture(video_interior, interior_start);

    const std::wstring av_interior = session->directory + L"\\av-interior.mp4";
    CHECK(exporter->Export(av_source, av_interior, interior_start,
                           interior_end, TestOptions(true), {}));
    CHECK(FileExists(av_interior));
    const Mp4Inspection av_inspection = InspectMp4(av_interior);
    CHECK(av_inspection.valid);
    CHECK(av_inspection.has_audio);
    CHECK(av_inspection.first_video_time >= 0);
    CHECK(av_inspection.first_video_time <= kSecond / kFrameRate);
    CHECK(av_inspection.first_audio_time >= 0);
    CHECK(av_inspection.first_audio_time <= 2 * 1024ll * kSecond /
                                                kAudioSampleRate);
    CheckDurationNear(av_inspection, expected_interior, kAvTolerance);
    const int64_t av_endpoint_delta =
        std::abs(av_inspection.video_end - av_inspection.audio_end);
    CHECK(av_endpoint_delta <= kAvTolerance);

    const std::wstring beginning = session->directory + L"\\beginning.mp4";
    CHECK(exporter->Export(video_source, beginning, MediaTime{0},
                           MediaTime{2 * kSecond}, TestOptions(false), {}));
    const Mp4Inspection beginning_inspection = InspectMp4(beginning);
    CHECK(beginning_inspection.valid);
    CHECK(beginning_inspection.first_video_time >= 0);
    CHECK(beginning_inspection.first_video_time <= kSecond / kFrameRate);
    CheckDurationNear(beginning_inspection, MediaTime{2 * kSecond},
                      kVideoTolerance);

    const std::wstring replace_destination =
        session->directory + L"\\replace-approved.mp4";
    CHECK(::CopyFileW(beginning.c_str(), replace_destination.c_str(), TRUE) !=
          FALSE);
    const auto before_unapproved_replace =
        FingerprintFile(replace_destination);
    CHECK(!exporter->Export(video_source, replace_destination, MediaTime{0},
                            MediaTime{kSecond}, TestOptions(false), {},
                            false));
    CHECK(FingerprintFile(replace_destination) == before_unapproved_replace);
    CHECK(exporter->Export(video_source, replace_destination, MediaTime{0},
                           MediaTime{kSecond}, TestOptions(false), {}, true));
    const Mp4Inspection replaced_inspection = InspectMp4(replace_destination);
    CHECK(replaced_inspection.valid);
    CheckDurationNear(replaced_inspection, MediaTime{kSecond},
                      kVideoTolerance);

    const Mp4Inspection source_inspection = InspectMp4(video_source);
    CHECK(source_inspection.valid);
    CHECK(source_inspection.presentation_duration <=
          static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()));
    const MediaTime source_endpoint{
        static_cast<int64_t>(source_inspection.presentation_duration)};
    const MediaTime end_start = source_endpoint - MediaTime{2 * kSecond};
    const std::wstring ending = session->directory + L"\\ending.mp4";
    CHECK(end_start >= MediaTime::zero());
    CHECK(exporter->Export(video_source, ending, end_start, source_endpoint,
                           TestOptions(false), {}));
    const Mp4Inspection ending_inspection = InspectMp4(ending);
    CHECK(ending_inspection.valid);
    CHECK(ending_inspection.first_video_time >= 0);
    CHECK(ending_inspection.first_video_time <= kSecond / kFrameRate);
    CheckDurationNear(ending_inspection, MediaTime{2 * kSecond},
                      kVideoTolerance);

    const std::wstring invalid_negative =
        session->directory + L"\\invalid-negative.mp4";
    CHECK(!exporter->Export(video_source, invalid_negative, MediaTime{-1},
                            MediaTime{kSecond}, TestOptions(false), {}));
    CHECK(!FileExists(invalid_negative));
    const std::wstring invalid_empty =
        session->directory + L"\\invalid-empty.mp4";
    CHECK(!exporter->Export(video_source, invalid_empty, MediaTime{kSecond},
                            MediaTime{kSecond}, TestOptions(false), {}));
    CHECK(!FileExists(invalid_empty));
    const std::wstring invalid_reverse =
        session->directory + L"\\invalid-reverse.mp4";
    CHECK(!exporter->Export(video_source, invalid_reverse,
                            MediaTime{2 * kSecond}, MediaTime{kSecond},
                            TestOptions(false), {}));
    CHECK(!FileExists(invalid_reverse));
    const std::wstring invalid_outside =
        session->directory + L"\\invalid-outside.mp4";
    CHECK(!exporter->Export(video_source, invalid_outside,
                            MediaTime{kSecond},
                            source_endpoint + MediaTime{1},
                            TestOptions(false), {}));
    CHECK(!FileExists(invalid_outside));

    const auto before_cancel = FingerprintFile(av_source);
    const std::wstring cancelled = session->directory + L"\\cancelled.mp4";
    bool cancellation_requested = false;
    CHECK(!exporter->Export(
        av_source, cancelled, MediaTime{0}, MediaTime{3 * kSecond},
        TestOptions(true), [&cancellation_requested](float value) {
            if (value >= 0.15f) {
                cancellation_requested = true;
                return false;
            }
            return true;
        }));
    CHECK(cancellation_requested);
    CHECK(!FileExists(cancelled));
    CheckNoPartialFiles(session->directory, L"cancelled.mp4");
    CHECK(before_cancel.has_value());
    CHECK(FingerprintFile(av_source) == before_cancel);

    // Cancel after real samples reach the encoder, including its first active
    // frame and later buffered work. Repeat both video-only and A/V teardown;
    // the process-level MFShutdown at test exit must have no orphan callbacks.
    constexpr float cancellation_points[]{0.001f, 0.15f, 0.60f};
    for (int attempt = 0; attempt < 12; ++attempt) {
        const bool with_audio = attempt % 2 != 0;
        const float cancel_at = cancellation_points[(attempt / 2) % 3];
        const std::wstring destination = session->directory +
                                         L"\\cancel-in-flight-" + std::to_wstring(attempt) + L".mp4";
        bool cancelled_in_flight = false;
        CHECK(!exporter->Export(
            with_audio ? av_source : video_source, destination, MediaTime{0},
            MediaTime{3 * kSecond}, TestOptions(with_audio),
            [&cancelled_in_flight, cancel_at](float value) {
                if (value >= cancel_at) {
                    cancelled_in_flight = true;
                    return false;
                }
                return true;
            }));
        CHECK(cancelled_in_flight);
        CHECK(!FileExists(destination));
        CheckNoPartialFiles(session->directory,
                            L"cancel-in-flight-" + std::to_wstring(attempt) +
                                L".mp4");
    }
    CHECK(FingerprintFile(video_source) == video_source_before);
    CHECK(FingerprintFile(av_source) == av_source_before);

    const auto before_failure = FingerprintFile(video_source);
    const std::wstring missing_parent_destination =
        session->directory + L"\\missing\\failure.mp4";
    CHECK(!exporter->Export(video_source, missing_parent_destination,
                            MediaTime{0}, MediaTime{kSecond},
                            TestOptions(false), {}));
    CHECK(!FileExists(missing_parent_destination));
    CHECK(before_failure.has_value());
    CHECK(FingerprintFile(video_source) == before_failure);

    CHECK(FingerprintFile(video_source) == video_source_before);
    CHECK(FingerprintFile(av_source) == av_source_before);
}

// Sources that are not Easnap recordings (ADR-040): another extension of the
// table, WMV and VC-1 with WMA sound, and audio that ends before the video.
void TestExternalSources() {
    MediaFoundationRuntime runtime;
    CHECK(SUCCEEDED(runtime.result()));
    auto store = easnap::media::CreateTemporarySessionStore();
    const auto session = store != nullptr ? store->CreateSession() : std::nullopt;
    CHECK(session.has_value());
    auto exporter = easnap::media::CreateMp4TrimExporter();
    CHECK(exporter != nullptr);
    if (FAILED(runtime.result()) || !session.has_value() || exporter == nullptr) {
        return;
    }
    constexpr int64_t kVideoTolerance = 2 * kSecond / kFrameRate;
    constexpr int64_t kAvTolerance = 1'000'000;
    const MediaTime start{kSecond};
    const MediaTime end{3 * kSecond};

    // Audio for the first second of four. An interval after it used to fail;
    // it now exports with a silent audio track that ends with the video.
    const std::wstring short_audio = session->directory + L"\\short-audio.mp4";
    CHECK(CreateSourceMp4(short_audio, true, kFrameRate));
    const auto short_audio_before = FingerprintFile(short_audio);
    const std::wstring after_audio = session->directory + L"\\after-audio.mp4";
    CHECK(exporter->Export(short_audio, after_audio, MediaTime{2 * kSecond},
                           MediaTime{3 * kSecond}, TestOptions(true), {}));
    const Mp4Inspection after_inspection = InspectMp4(after_audio);
    CHECK(after_inspection.valid);
    CHECK(after_inspection.has_audio);
    CHECK(after_inspection.first_audio_time >= 0);
    CHECK(after_inspection.first_audio_time <= 2 * 1024ll * kSecond / kAudioSampleRate);
    CheckDurationNear(after_inspection, MediaTime{kSecond}, kAvTolerance);
    CHECK(std::abs(after_inspection.video_end - after_inspection.audio_end) <= kAvTolerance);
    CheckTrimmedPicture(after_audio, MediaTime{2 * kSecond});
    // An interval in which the audio ends keeps the audio it has.
    const std::wstring across_end = session->directory + L"\\across-audio-end.mp4";
    CHECK(exporter->Export(short_audio, across_end, MediaTime{kSecond / 2}, end,
                           TestOptions(true), {}));
    const Mp4Inspection across_inspection = InspectMp4(across_end);
    CHECK(across_inspection.valid);
    CHECK(across_inspection.has_audio);
    CheckDurationNear(across_inspection, end - MediaTime{kSecond / 2}, kAvTolerance);
    CHECK(std::abs(across_inspection.video_end - across_inspection.audio_end) <= kAvTolerance);
    CHECK(FingerprintFile(short_audio) == short_audio_before);

    // Another MPEG-4 extension of the table.
    const std::wstring mov_source = session->directory + L"\\source.mov";
    CHECK(CreateSourceMp4(session->directory + L"\\for-mov.mp4", true));
    CHECK(::CopyFileW((session->directory + L"\\for-mov.mp4").c_str(), mov_source.c_str(),
                      TRUE) != FALSE);
    const std::wstring from_mov = session->directory + L"\\from-mov.mp4";
    CHECK(exporter->Export(mov_source, from_mov, start, end, TestOptions(true), {}));
    const Mp4Inspection mov_inspection = InspectMp4(from_mov);
    CHECK(mov_inspection.valid);
    CHECK(mov_inspection.has_audio);
    CheckDurationNear(mov_inspection, end - start, kAvTolerance);
    CHECK(g_colour_conversions == 0);

    // An extension outside the table is refused before anything is decoded.
    const std::wstring mkv_source = session->directory + L"\\source.mkv";
    CHECK(::CopyFileW(mov_source.c_str(), mkv_source.c_str(), TRUE) != FALSE);
    const std::wstring from_mkv = session->directory + L"\\from-mkv.mp4";
    CHECK(!exporter->Export(mkv_source, from_mkv, start, end, TestOptions(true), {}));
    CHECK(!FileExists(from_mkv));

    // WMV and VC-1 written by Windows' own encoders, decoded and re-encoded
    // to H.264 and AAC.
    struct AsfCase final {
        const wchar_t* name;
        GUID video;
        GUID audio;
    };
    const AsfCase asf_cases[]{
        {L"\\wmv3-wma.wmv", MFVideoFormat_WMV3, MFAudioFormat_WMAudioV8},
        {L"\\vc1.asf", MFVideoFormat_WVC1, GUID_NULL},
    };
    for (const AsfCase& item : asf_cases) {
        const std::wstring source = session->directory + item.name;
        easnap::test::SinkWriterSpec spec;
        spec.video_subtype = item.video;
        spec.audio_subtype = item.audio;
        spec.width = kWidth;
        spec.height = kHeight;
        spec.frame_rate = kFrameRate;
        spec.frame_count = static_cast<uint32_t>(kFrameCount);
        spec.paint = [](uint32_t index, easnap::platform::BgraImage& image) {
            FillMovingPattern(image, index);
        };
        const easnap::test::SinkWriterOutcome outcome =
            easnap::test::WriteWithSinkWriter(source, spec);
        if (outcome == easnap::test::SinkWriterOutcome::EncoderUnavailable) {
            std::wprintf(L"SKIP %ls: Windows has no encoder for it\n", item.name + 1);
            continue;
        }
        CHECK(outcome == easnap::test::SinkWriterOutcome::Written);
        if (outcome != easnap::test::SinkWriterOutcome::Written) {
            continue;
        }
        const bool with_audio = item.audio != GUID_NULL;
        const auto before = FingerprintFile(source);
        const std::wstring trimmed = source + L".trimmed.mp4";
        CHECK(exporter->Export(source, trimmed, start, end, TestOptions(with_audio), {}));
        const Mp4Inspection inspection = InspectMp4(trimmed);
        CHECK(inspection.valid);
        CHECK(inspection.has_audio == with_audio);
        CheckDurationNear(inspection, end - start, with_audio ? kAvTolerance : kVideoTolerance);
        CheckTrimmedPictureAgainstSource(trimmed, source, start);
        CHECK(FingerprintFile(source) == before);
        // Its picture is BT.601, the MP4 declares BT.709: the colours are
        // converted through RGB rather than passed on as decoded.
        const auto pipeline = exporter->LastDiagnostics().video_pipeline;
        CHECK(pipeline == easnap::media::TrimVideoPipeline::BoundedBgraFallback);
        std::wprintf(L"%ls trimmed with pipeline %d\n", item.name + 1,
                     static_cast<int>(pipeline));
    }
    CHECK(store->DeleteSession(*session));
    CHECK(::GetFileAttributesW(session->directory.c_str()) == INVALID_FILE_ATTRIBUTES);
}

// The exporter says why an export failed only through LogMessage. Printing
// the lines lets a failure on another machine, such as a hosted runner with a
// software encoder, show its reason in the test output.
void PrintLogLine(const wchar_t* line) noexcept {
    if (std::wcsstr(line, L"not BT.709") != nullptr) {
        ++g_colour_conversions;
    }
    if (std::wcsstr(line, L"silence") != nullptr) {
        ++g_silence_fills;
    }
    std::fwprintf(stderr, L"log: %ls\n", line);
}

}  // namespace

/// Runs one export on its own thread and gives up after `limit`: an export
/// that stalls inside the sink writer never returns, so the test ends the
/// process instead of waiting for the CTest timeout.
bool ExportWithin(easnap::media::ITrimExporter& exporter, const std::wstring& source,
                  const std::wstring& destination, MediaTime start, MediaTime end,
                  std::chrono::seconds limit, double& seconds) {
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    bool result = false;
    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&] {
        const bool exported =
            exporter.Export(source, destination, start, end, TestOptions(true), {});
        const std::scoped_lock lock(mutex);
        result = exported;
        done = true;
        finished.notify_one();
    });
    std::unique_lock lock(mutex);
    if (!finished.wait_for(lock, limit, [&] { return done; })) {
        std::fprintf(stderr, "FAIL  the export did not finish within %lld s: it stalled\n",
                     static_cast<long long>(limit.count()));
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 1);
    }
    lock.unlock();
    worker.join();
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

// A long stretch of video after the audio has ended (ADR-040, ADR-007): the
// export keeps silence at most 750 ms behind the video, so the sink writer
// never waits on audio that will not come. Both the interval that holds no
// audio and the one in which the audio ends are exported in bounded time.
void TestLongVideoOnlyTail() {
    MediaFoundationRuntime runtime;
    CHECK(SUCCEEDED(runtime.result()));
    auto store = easnap::media::CreateTemporarySessionStore();
    const auto session = store != nullptr ? store->CreateSession() : std::nullopt;
    CHECK(session.has_value());
    auto exporter = easnap::media::CreateMp4TrimExporter();
    CHECK(exporter != nullptr);
    if (FAILED(runtime.result()) || !session.has_value() || exporter == nullptr) {
        return;
    }
    constexpr uint32_t kTailSeconds = 32;
    constexpr int64_t kAvTolerance = 1'000'000;
    const std::wstring source = session->directory + L"\\long-tail.mp4";
    easnap::test::SinkWriterSpec spec;
    spec.video_subtype = MFVideoFormat_H264;
    spec.audio_subtype = MFAudioFormat_AAC;
    spec.width = kWidth;
    spec.height = kHeight;
    spec.frame_rate = kFrameRate;
    spec.frame_count = (kTailSeconds + 1u) * kFrameRate;
    spec.audio_frame_count = kFrameRate;  // One second of audio.
    spec.paint = [](uint32_t index, easnap::platform::BgraImage& image) {
        FillMovingPattern(image, index);
    };
    const easnap::test::SinkWriterOutcome outcome = easnap::test::WriteWithSinkWriter(source, spec);
    CHECK(outcome == easnap::test::SinkWriterOutcome::Written);
    if (outcome != easnap::test::SinkWriterOutcome::Written) {
        return;
    }

    struct Interval final {
        const wchar_t* name;
        MediaTime start;
        MediaTime end;
    };
    const Interval intervals[]{
        {L"\\audio-then-long-tail.mp4", MediaTime{0}, MediaTime{kTailSeconds * kSecond}},
        {L"\\long-tail-only.mp4", MediaTime{2 * kSecond}, MediaTime{kTailSeconds * kSecond}},
    };
    for (const Interval& interval : intervals) {
        const std::wstring destination = session->directory + interval.name;
        double seconds = 0.0;
        CHECK(ExportWithin(*exporter, source, destination, interval.start, interval.end,
                           std::chrono::seconds(90), seconds));
        std::wprintf(L"%ls: %.0f s exported in %.1f s\n", interval.name + 1,
                     static_cast<double>((interval.end - interval.start).count()) / kSecond,
                     seconds);
        const Mp4Inspection inspection = InspectMp4(destination);
        CHECK(inspection.valid);
        CHECK(inspection.has_audio);
        CheckDurationNear(inspection, interval.end - interval.start, kAvTolerance);
        CHECK(std::abs(inspection.video_end - inspection.audio_end) <= kAvTolerance);
    }
    CHECK(store->DeleteSession(*session));
    CHECK(::GetFileAttributesW(session->directory.c_str()) == INVALID_FILE_ATTRIBUTES);
}

int main() {
    easnap::platform::SetLogSink(PrintLogLine);
    TestMp4TrimExporter();
    // Every source above was made by the production writer, as recordings
    // are: none may leave the NV12 path or gain silence, on any machine.
    CHECK(g_colour_conversions == 0);
    CHECK(g_silence_fills == 0);
    TestExternalSources();
    TestLongVideoOnlyTail();
    if (g_fail == 0) {
        std::printf("PASSED  %d/%d checks\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
