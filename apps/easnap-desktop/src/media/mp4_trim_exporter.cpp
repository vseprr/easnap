#include "media/media_interfaces.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "media/media_foundation_writer.h"
#include "media/mf_d3d_device.h"

namespace easnap::media {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int64_t kMediaTimeUnitsPerSecond = 10'000'000;
constexpr DWORD kMaximumSourceStreams = 32;

class MediaFoundationRuntime final {
public:
    MediaFoundationRuntime() noexcept {
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

    MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
    MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool com_initialized_{false};
    bool mf_started_{false};
};

class IncompleteFile final {
public:
    explicit IncompleteFile(std::wstring path) : path_(std::move(path)) {}

    ~IncompleteFile() {
        if (armed_ && !path_.empty() && !::DeleteFileW(path_.c_str())) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) {
                platform::LogMessage(
                    L"[Trim] Failed to remove an incomplete MP4 (error %lu)", error);
            }
        }
    }

    IncompleteFile(const IncompleteFile&) = delete;
    IncompleteFile& operator=(const IncompleteFile&) = delete;

    void Disarm() noexcept { armed_ = false; }

private:
    std::wstring path_;
    bool armed_{true};
};

struct SourceDescription final {
    ComPtr<IMFSourceReader> reader;
    DWORD video_stream{0};
    std::optional<DWORD> audio_stream;
    UINT32 width{0};
    UINT32 height{0};
    LONG video_stride{0};
    UINT32 frame_rate_numerator{0};
    UINT32 frame_rate_denominator{0};
    AudioFormat audio_format{};
    MediaTime duration{0};
    bool native_nv12{false};
    bool d3d_requested{false};
    bool dxgi_sample_seen{false};
};

using DiagnosticsClock = std::chrono::steady_clock;

struct StageAccumulator final {
    DiagnosticsClock::duration decode{};
    DiagnosticsClock::duration process{};
    DiagnosticsClock::duration encode_submit{};
};

void LogFailure(const wchar_t* operation, HRESULT result) {
    platform::LogHresult(operation, result);
}

void LogFailure(const wchar_t* message) {
    platform::LogMessage(L"[Trim] %s", message);
}

bool IsAbsoluteMp4Path(const std::wstring& path) noexcept {
    return !path.empty() && ::PathIsRelativeW(path.c_str()) == FALSE &&
           _wcsicmp(::PathFindExtensionW(path.c_str()), L".mp4") == 0;
}

std::optional<std::wstring> NormalizedAbsolutePath(const std::wstring& path) {
    const DWORD required = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(required);
    const DWORD written = ::GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    return std::wstring(buffer.data(), written);
}

bool ValidatePaths(const std::wstring& source_path,
                   const std::wstring& destination_path,
                   bool replace_existing) {
    if (!IsAbsoluteMp4Path(source_path) || !IsAbsoluteMp4Path(destination_path)) {
        LogFailure(L"Source and destination must be absolute .mp4 paths");
        return false;
    }

    const DWORD source_attributes = ::GetFileAttributesW(source_path.c_str());
    if (source_attributes == INVALID_FILE_ATTRIBUTES ||
        (source_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        LogFailure(L"The source MP4 does not exist or is not a file");
        return false;
    }
    const DWORD destination_attributes =
        ::GetFileAttributesW(destination_path.c_str());
    if (destination_attributes != INVALID_FILE_ATTRIBUTES &&
        (!replace_existing ||
         (destination_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        LogFailure(L"The destination MP4 already exists without replacement approval");
        return false;
    }

    const auto normalized_source = NormalizedAbsolutePath(source_path);
    const auto normalized_destination = NormalizedAbsolutePath(destination_path);
    if (!normalized_source.has_value() || !normalized_destination.has_value()) {
        LogFailure(L"The source or destination path could not be normalized");
        return false;
    }
    if (_wcsicmp(normalized_source->c_str(), normalized_destination->c_str()) == 0) {
        LogFailure(L"The source and destination MP4 paths must be different");
        return false;
    }

    const std::size_t separator = destination_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        LogFailure(L"The destination MP4 directory could not be located");
        return false;
    }
    const std::wstring parent = destination_path.substr(0, separator);
    const DWORD parent_attributes = ::GetFileAttributesW(parent.c_str());
    if (parent_attributes == INVALID_FILE_ATTRIBUTES ||
        (parent_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        LogFailure(L"The destination MP4 directory does not exist");
        return false;
    }
    return true;
}

std::optional<std::wstring> MakeStagingPath(const std::wstring& destination_path) {
    GUID identifier{};
    const HRESULT result = ::CoCreateGuid(&identifier);
    if (FAILED(result)) {
        LogFailure(L"Creating an identifier for the temporary trim output", result);
        return std::nullopt;
    }
    wchar_t text[40]{};
    if (::StringFromGUID2(identifier, text, static_cast<int>(std::size(text))) == 0) {
        LogFailure(L"Formatting the temporary trim output identifier");
        return std::nullopt;
    }

    std::wstring path = destination_path;
    path += L".easnap-";
    path += text;
    path += L".partial.mp4";
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        LogFailure(L"The temporary trim output path unexpectedly exists");
        return std::nullopt;
    }
    return path;
}

std::optional<MediaTime> ReadPresentationDuration(IMFSourceReader* reader) {
    PROPVARIANT value;
    ::PropVariantInit(&value);
    const HRESULT result = reader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value);
    if (FAILED(result)) {
        LogFailure(L"Reading the source MP4 duration", result);
        return std::nullopt;
    }

    std::optional<MediaTime> duration;
    if (value.vt == VT_UI8 &&
        value.uhVal.QuadPart <= static_cast<ULONGLONG>((std::numeric_limits<int64_t>::max)())) {
        duration = MediaTime{static_cast<int64_t>(value.uhVal.QuadPart)};
    } else if (value.vt == VT_I8 && value.hVal.QuadPart >= 0) {
        duration = MediaTime{value.hVal.QuadPart};
    }
    ::PropVariantClear(&value);

    if (!duration.has_value() || *duration <= MediaTime::zero()) {
        LogFailure(L"The source MP4 has no valid positive duration");
        return std::nullopt;
    }
    return duration;
}

bool DiscoverStreams(IMFSourceReader* reader, DWORD& video_stream,
                     std::optional<DWORD>& audio_stream) {
    bool found_video = false;
    bool found_audio = false;
    for (DWORD stream = 0; stream < kMaximumSourceStreams; ++stream) {
        ComPtr<IMFMediaType> type;
        const HRESULT result = reader->GetNativeMediaType(stream, 0, &type);
        if (result == MF_E_INVALIDSTREAMNUMBER) {
            break;
        }
        if (FAILED(result)) {
            continue;
        }

        GUID major_type{};
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major_type)) ||
            FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            continue;
        }
        if (major_type == MFMediaType_Video && !found_video) {
            if (subtype != MFVideoFormat_H264) {
                LogFailure(L"The source video stream is not H.264");
                return false;
            }
            video_stream = stream;
            found_video = true;
        } else if (major_type == MFMediaType_Audio && !found_audio) {
            if (subtype != MFAudioFormat_AAC) {
                LogFailure(L"The source audio stream is not AAC");
                return false;
            }
            audio_stream = stream;
            found_audio = true;
        }
    }
    if (!found_video) {
        LogFailure(L"The source MP4 has no H.264 video stream");
        return false;
    }
    return true;
}

bool ConfigureVideoDecoder(SourceDescription& source, bool native_nv12) {
    ComPtr<IMFMediaType> decoded_type;
    HRESULT result = ::MFCreateMediaType(&decoded_type);
    if (SUCCEEDED(result)) {
        result = decoded_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = decoded_type->SetGUID(
            MF_MT_SUBTYPE,
            native_nv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(result)) {
        result = source.reader->SetCurrentMediaType(source.video_stream, nullptr,
                                                    decoded_type.Get());
    }
    if (FAILED(result)) {
        LogFailure(native_nv12
                       ? L"Configuring native NV12 H.264 video decoding"
                       : L"Configuring incremental RGB32 H.264 video decoding",
                   result);
        return false;
    }

    ComPtr<IMFMediaType> actual_type;
    result = source.reader->GetCurrentMediaType(source.video_stream, &actual_type);
    if (SUCCEEDED(result)) {
        result = ::MFGetAttributeSize(actual_type.Get(), MF_MT_FRAME_SIZE,
                                      &source.width, &source.height);
    }
    if (SUCCEEDED(result)) {
        result = ::MFGetAttributeRatio(actual_type.Get(), MF_MT_FRAME_RATE,
                                       &source.frame_rate_numerator,
                                       &source.frame_rate_denominator);
    }
    GUID actual_subtype{};
    if (SUCCEEDED(result)) {
        result = actual_type->GetGUID(MF_MT_SUBTYPE, &actual_subtype);
    }
    UINT32 stride = 0;
    if (SUCCEEDED(result) &&
        SUCCEEDED(actual_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        source.video_stride = static_cast<LONG>(stride);
    } else if (SUCCEEDED(result) &&
               source.width <= static_cast<UINT32>((std::numeric_limits<LONG>::max)() / 4)) {
        source.video_stride = static_cast<LONG>(
            native_nv12 ? source.width : source.width * 4u);
    }

    const uint64_t pixels = static_cast<uint64_t>(source.width) * source.height;
    if (FAILED(result) || source.width == 0 || source.height == 0 ||
        (source.width & 1u) != 0 || (source.height & 1u) != 0 ||
        source.frame_rate_numerator == 0 || source.frame_rate_denominator == 0 ||
        source.video_stride == 0 ||
        actual_subtype !=
            (native_nv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32) ||
        pixels > (std::numeric_limits<std::size_t>::max)() / sizeof(uint32_t)) {
        LogFailure(L"The decoded video format is invalid or unsupported");
        return false;
    }
    source.native_nv12 = native_nv12;
    return true;
}

bool ConfigureAudioDecoder(SourceDescription& source) {
    if (!source.audio_stream.has_value()) {
        return true;
    }

    ComPtr<IMFMediaType> decoded_type;
    HRESULT result = ::MFCreateMediaType(&decoded_type);
    if (SUCCEEDED(result)) {
        result = decoded_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(result)) {
        result = decoded_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    }
    if (SUCCEEDED(result)) {
        result = decoded_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    }
    if (SUCCEEDED(result)) {
        result = source.reader->SetCurrentMediaType(*source.audio_stream, nullptr,
                                                    decoded_type.Get());
    }
    if (FAILED(result)) {
        LogFailure(L"Configuring incremental AAC audio decoding", result);
        return false;
    }

    ComPtr<IMFMediaType> actual_type;
    GUID subtype{};
    UINT32 channels = 0;
    UINT32 bits_per_sample = 0;
    UINT32 channel_mask = 0;
    result = source.reader->GetCurrentMediaType(*source.audio_stream, &actual_type);
    if (SUCCEEDED(result)) {
        result = actual_type->GetGUID(MF_MT_SUBTYPE, &subtype);
    }
    if (SUCCEEDED(result)) {
        result = actual_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                        &source.audio_format.sample_rate);
    }
    if (SUCCEEDED(result)) {
        result = actual_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    }
    if (SUCCEEDED(result)) {
        result = actual_type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,
                                        &bits_per_sample);
    }
    if (actual_type != nullptr) {
        static_cast<void>(actual_type->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK,
                                                 &channel_mask));
    }
    if (FAILED(result) || subtype != MFAudioFormat_Float || channels == 0 ||
        channels > (std::numeric_limits<uint16_t>::max)() ||
        bits_per_sample != 32) {
        LogFailure(L"The decoded audio format is invalid or unsupported");
        return false;
    }

    source.audio_format.channels = static_cast<uint16_t>(channels);
    source.audio_format.bits_per_sample = static_cast<uint16_t>(bits_per_sample);
    source.audio_format.is_float = true;
    source.audio_format.channel_mask = channel_mask;
    if (!source.audio_format.valid() || source.audio_format.block_align() == 0) {
        LogFailure(L"The decoded PCM audio format is invalid");
        return false;
    }
    return true;
}

std::optional<SourceDescription> OpenSource(
    const std::wstring& source_path, IMFDXGIDeviceManager* device_manager,
    bool native_nv12) {
    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 5);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                                       native_nv12 ? FALSE : TRUE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    }
    if (SUCCEEDED(result) && device_manager != nullptr) {
        result = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,
                                        device_manager);
    }

    SourceDescription source;
    source.d3d_requested = device_manager != nullptr;
    if (SUCCEEDED(result)) {
        result = ::MFCreateSourceReaderFromURL(source_path.c_str(), attributes.Get(),
                                               &source.reader);
    }
    if (FAILED(result)) {
        LogFailure(L"Opening the source MP4 with Media Foundation", result);
        return std::nullopt;
    }

    const auto duration = ReadPresentationDuration(source.reader.Get());
    if (!duration.has_value() ||
        !DiscoverStreams(source.reader.Get(), source.video_stream,
                         source.audio_stream)) {
        return std::nullopt;
    }
    source.duration = *duration;

    result = source.reader->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(result)) {
        result = source.reader->SetStreamSelection(source.video_stream, TRUE);
    }
    if (SUCCEEDED(result) && source.audio_stream.has_value()) {
        result = source.reader->SetStreamSelection(*source.audio_stream, TRUE);
    }
    if (FAILED(result)) {
        LogFailure(L"Selecting the source MP4 media streams", result);
        return std::nullopt;
    }
    if (!ConfigureVideoDecoder(source, native_nv12) ||
        !ConfigureAudioDecoder(source)) {
        return std::nullopt;
    }
    return source;
}

int64_t FramesToMediaTime(uint64_t frames, uint32_t sample_rate) noexcept {
    if (sample_rate == 0) {
        return 0;
    }
    const uint64_t seconds = frames / sample_rate;
    const uint64_t remainder = frames % sample_rate;
    if (seconds > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) /
                      kMediaTimeUnitsPerSecond) {
        return (std::numeric_limits<int64_t>::max)();
    }
    const uint64_t result = seconds * kMediaTimeUnitsPerSecond +
                            remainder * kMediaTimeUnitsPerSecond / sample_rate;
    return result <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())
               ? static_cast<int64_t>(result)
               : (std::numeric_limits<int64_t>::max)();
}

uint64_t MediaTimeToFramesCeiling(int64_t time, uint32_t sample_rate) noexcept {
    if (time <= 0 || sample_rate == 0) {
        return 0;
    }
    const uint64_t unsigned_time = static_cast<uint64_t>(time);
    const uint64_t seconds = unsigned_time / kMediaTimeUnitsPerSecond;
    const uint64_t remainder = unsigned_time % kMediaTimeUnitsPerSecond;
    if (seconds > (std::numeric_limits<uint64_t>::max)() / sample_rate) {
        return (std::numeric_limits<uint64_t>::max)();
    }
    const uint64_t whole_frames = seconds * sample_rate;
    const uint64_t partial_frames =
        (remainder * sample_rate + kMediaTimeUnitsPerSecond - 1) /
        kMediaTimeUnitsPerSecond;
    if (whole_frames > (std::numeric_limits<uint64_t>::max)() - partial_frames) {
        return (std::numeric_limits<uint64_t>::max)();
    }
    return whole_frames + partial_frames;
}

bool CopyDecodedVideo(IMFSample* sample, const SourceDescription& source,
                      std::shared_ptr<platform::BgraImage>& image) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(result)) {
        LogFailure(L"Accessing a decoded video sample", result);
        return false;
    }

    try {
        auto mutable_image = std::make_shared<platform::BgraImage>();
        mutable_image->width = static_cast<int>(source.width);
        mutable_image->height = static_cast<int>(source.height);
        mutable_image->pixels.resize(static_cast<std::size_t>(source.width) * source.height);

        ComPtr<IMF2DBuffer> buffer_2d;
        result = buffer.As(&buffer_2d);
        if (SUCCEEDED(result)) {
            BYTE* scanline = nullptr;
            LONG pitch = 0;
            result = buffer_2d->Lock2D(&scanline, &pitch);
            if (SUCCEEDED(result) && scanline != nullptr &&
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(pitch))) >=
                    static_cast<uint64_t>(source.width) * 4u) {
                for (UINT32 row = 0; row < source.height; ++row) {
                    std::memcpy(mutable_image->row(static_cast<int>(row)),
                                scanline + static_cast<int64_t>(row) * pitch,
                                static_cast<std::size_t>(source.width) * 4u);
                }
            } else if (SUCCEEDED(result)) {
                result = E_UNEXPECTED;
            }
            if (scanline != nullptr) {
                const HRESULT unlock_result = buffer_2d->Unlock2D();
                if (SUCCEEDED(result)) {
                    result = unlock_result;
                }
            }
        } else {
            BYTE* bytes = nullptr;
            DWORD byte_count = 0;
            result = buffer->Lock(&bytes, nullptr, &byte_count);
            const uint64_t stride = static_cast<uint64_t>(
                std::abs(static_cast<int64_t>(source.video_stride)));
            const uint64_t required = stride * source.height;
            if (SUCCEEDED(result) && bytes != nullptr &&
                stride >= static_cast<uint64_t>(source.width) * 4u &&
                required <= byte_count) {
                BYTE* first_row = bytes;
                if (source.video_stride < 0) {
                    first_row += stride * (source.height - 1u);
                }
                for (UINT32 row = 0; row < source.height; ++row) {
                    std::memcpy(mutable_image->row(static_cast<int>(row)),
                                first_row + static_cast<int64_t>(row) *
                                                source.video_stride,
                                static_cast<std::size_t>(source.width) * 4u);
                }
            } else if (SUCCEEDED(result)) {
                result = E_UNEXPECTED;
            }
            if (bytes != nullptr) {
                const HRESULT unlock_result = buffer->Unlock();
                if (SUCCEEDED(result)) {
                    result = unlock_result;
                }
            }
        }

        if (FAILED(result)) {
            LogFailure(L"Copying a bounded decoded video frame", result);
            return false;
        }
        for (uint32_t& pixel : mutable_image->pixels) {
            pixel |= 0xFF000000u;
        }
        image = std::move(mutable_image);
        return true;
    } catch (const std::bad_alloc&) {
        LogFailure(L"Allocating a bounded decoded video frame", E_OUTOFMEMORY);
        return false;
    }
}

bool CopyDecodedAudio(IMFSample* sample, std::vector<uint8_t>& bytes) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(result)) {
        LogFailure(L"Accessing a decoded audio sample", result);
        return false;
    }

    BYTE* data = nullptr;
    DWORD byte_count = 0;
    result = buffer->Lock(&data, nullptr, &byte_count);
    if (FAILED(result) || (data == nullptr && byte_count != 0)) {
        LogFailure(L"Locking a decoded audio sample",
                   FAILED(result) ? result : E_UNEXPECTED);
        return false;
    }
    try {
        bytes.assign(data, data + byte_count);
    } catch (const std::bad_alloc&) {
        static_cast<void>(buffer->Unlock());
        LogFailure(L"Allocating a bounded decoded audio sample", E_OUTOFMEMORY);
        return false;
    }
    result = buffer->Unlock();
    if (FAILED(result)) {
        LogFailure(L"Unlocking a decoded audio sample", result);
        return false;
    }
    return true;
}

int64_t DefaultVideoDuration(const SourceDescription& source) noexcept {
    const uint64_t numerator =
        static_cast<uint64_t>(kMediaTimeUnitsPerSecond) *
        source.frame_rate_denominator;
    const uint64_t rounded =
        (numerator + source.frame_rate_numerator / 2u) /
        source.frame_rate_numerator;
    return static_cast<int64_t>((std::max)(uint64_t{1}, rounded));
}

bool ReportProgress(const ITrimExporter::ProgressCallback& callback, float progress) {
    if (!callback) {
        return true;
    }
    try {
        return callback((std::clamp)(progress, 0.0f, 1.0f));
    } catch (...) {
        LogFailure(L"The trim progress callback raised an exception");
        return false;
    }
}

bool ProcessVideoSample(IMFSample* sample, int64_t source_time,
                        const SourceDescription& source, MediaTime start,
                        MediaTime end, IMediaWriter& writer,
                        MediaTime& last_video_end, uint64_t& frames_written,
                        StageAccumulator& stages) {
    LONGLONG duration = 0;
    if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) {
        duration = DefaultVideoDuration(source);
    }
    if (source_time > (std::numeric_limits<int64_t>::max)() - duration) {
        LogFailure(L"A decoded video timestamp overflows the media timeline");
        return false;
    }

    const int64_t source_end = source_time + duration;
    if (source_end <= start.count() || source_time >= end.count()) {
        return true;
    }
    const int64_t clipped_start = (std::max)(source_time, start.count());
    const int64_t clipped_end = (std::min)(source_end, end.count());
    MediaTime output_start{clipped_start - start.count()};
    output_start = (std::max)(output_start, last_video_end);
    const MediaTime output_end{clipped_end - start.count()};
    if (output_end <= output_start) {
        return true;
    }

    std::shared_ptr<platform::BgraImage> image;
    if (!CopyDecodedVideo(sample, source, image)) {
        return false;
    }
    const VideoFrame frame{std::move(image), output_start,
                           output_end - output_start};
    const auto encode_started = DiagnosticsClock::now();
    const bool wrote = writer.WriteVideoFrame(frame);
    stages.encode_submit += DiagnosticsClock::now() - encode_started;
    if (!wrote) {
        LogFailure(writer.last_error().c_str());
        return false;
    }
    last_video_end = output_end;
    ++frames_written;
    return true;
}

bool ProcessNativeVideoSample(
    IMFSample* sample, int64_t source_time, SourceDescription& source,
    MediaTime start, MediaTime end, IMediaFoundationNv12Writer& writer,
    MediaTime& last_video_end, uint64_t& frames_written,
    StageAccumulator& stages) {
    LONGLONG duration = 0;
    if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) {
        duration = DefaultVideoDuration(source);
    }
    if (source_time > (std::numeric_limits<int64_t>::max)() - duration) {
        LogFailure(L"A decoded NV12 timestamp overflows the media timeline");
        return false;
    }
    const int64_t source_end = source_time + duration;
    if (source_end <= start.count() || source_time >= end.count()) {
        return true;
    }
    const int64_t clipped_start = (std::max)(source_time, start.count());
    const int64_t clipped_end = (std::min)(source_end, end.count());
    MediaTime output_start{clipped_start - start.count()};
    output_start = (std::max)(output_start, last_video_end);
    const MediaTime output_end{clipped_end - start.count()};
    if (output_end <= output_start) {
        return true;
    }

    DWORD buffer_count = 0;
    if (SUCCEEDED(sample->GetBufferCount(&buffer_count))) {
        for (DWORD index = 0; index < buffer_count; ++index) {
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFDXGIBuffer> dxgi_buffer;
            if (SUCCEEDED(sample->GetBufferByIndex(index, &buffer)) &&
                SUCCEEDED(buffer.As(&dxgi_buffer))) {
                source.dxgi_sample_seen = true;
                break;
            }
        }
    }
    const auto encode_started = DiagnosticsClock::now();
    const bool wrote = writer.WriteVideoSample(sample, output_start,
                                               output_end - output_start);
    stages.encode_submit += DiagnosticsClock::now() - encode_started;
    if (!wrote) {
        LogFailure(writer.last_error().c_str());
        return false;
    }
    last_video_end = output_end;
    ++frames_written;
    return true;
}

bool ProcessVideoForWriter(IMFSample* sample, int64_t source_time,
                           SourceDescription& source, MediaTime start,
                           MediaTime end, IMediaWriter& writer,
                           MediaTime& last_video_end,
                           uint64_t& frames_written,
                           StageAccumulator& stages) {
    return ProcessVideoSample(sample, source_time, source, start, end, writer,
                              last_video_end, frames_written, stages);
}

bool ProcessVideoForWriter(IMFSample* sample, int64_t source_time,
                           SourceDescription& source, MediaTime start,
                           MediaTime end, IMediaFoundationNv12Writer& writer,
                           MediaTime& last_video_end,
                           uint64_t& frames_written,
                           StageAccumulator& stages) {
    return ProcessNativeVideoSample(sample, source_time, source, start, end,
                                    writer, last_video_end, frames_written,
                                    stages);
}

template <typename Writer>
bool ProcessAudioSample(IMFSample* sample, int64_t source_time,
                        const SourceDescription& source, MediaTime start,
                        MediaTime end, Writer& writer,
                        MediaTime& last_audio_end, uint64_t& packets_written,
                        StageAccumulator& stages) {
    std::vector<uint8_t> bytes;
    if (!CopyDecodedAudio(sample, bytes)) {
        return false;
    }
    const uint32_t block_align = source.audio_format.block_align();
    if (bytes.empty() || block_align == 0 || bytes.size() % block_align != 0) {
        LogFailure(L"A decoded audio sample has an invalid PCM payload");
        return false;
    }

    const uint64_t frame_count = bytes.size() / block_align;
    uint64_t first_frame = 0;
    if (source_time < start.count()) {
        first_frame = MediaTimeToFramesCeiling(start.count() - source_time,
                                               source.audio_format.sample_rate);
    }
    uint64_t end_frame = frame_count;
    if (source_time < end.count()) {
        end_frame = (std::min)(
            frame_count,
            MediaTimeToFramesCeiling(end.count() - source_time,
                                     source.audio_format.sample_rate));
    } else {
        end_frame = 0;
    }
    first_frame = (std::min)(first_frame, frame_count);
    if (end_frame <= first_frame) {
        return true;
    }

    const uint64_t retained_frames = end_frame - first_frame;
    const int64_t first_frame_offset =
        FramesToMediaTime(first_frame, source.audio_format.sample_rate);
    const int64_t retained_duration =
        FramesToMediaTime(retained_frames, source.audio_format.sample_rate);
    if (retained_duration <= 0 ||
        source_time > (std::numeric_limits<int64_t>::max)() - first_frame_offset) {
        LogFailure(L"A decoded audio sample has an invalid timeline");
        return false;
    }

    const int64_t retained_source_time = source_time + first_frame_offset;
    MediaTime output_start{(std::max)(int64_t{0},
                                      retained_source_time - start.count())};
    output_start = (std::max)(output_start, last_audio_end);
    const std::size_t byte_offset = static_cast<std::size_t>(first_frame) * block_align;
    const std::size_t byte_count = static_cast<std::size_t>(retained_frames) * block_align;
    const MediaChunk packet{bytes.data() + byte_offset, byte_count, output_start,
                            MediaTime{retained_duration}, false, false};
    const auto encode_started = DiagnosticsClock::now();
    const bool wrote = writer.WriteAudioPacket(packet);
    stages.encode_submit += DiagnosticsClock::now() - encode_started;
    if (!wrote) {
        LogFailure(writer.last_error().c_str());
        return false;
    }
    last_audio_end = output_start + packet.duration;
    ++packets_written;
    return true;
}

template <typename Writer>
bool TranscodeInterval(SourceDescription& source, MediaTime start, MediaTime end,
                       Writer& writer,
                       const ITrimExporter::ProgressCallback& progress,
                       StageAccumulator& stages) {
    bool video_done = false;
    bool audio_done = !source.audio_stream.has_value();
    MediaTime last_video_end{0};
    MediaTime last_audio_end{0};
    uint64_t frames_written = 0;
    uint64_t audio_packets_written = 0;
    float last_progress = 0.0f;
    bool video_started = false;
    bool audio_started = false;

    while (!video_done || !audio_done) {
        DWORD actual_stream = 0;
        DWORD flags = 0;
        LONGLONG source_time = 0;
        ComPtr<IMFSample> sample;
        const auto decode_started = DiagnosticsClock::now();
        const HRESULT result = source.reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &actual_stream,
            &flags, &source_time, &sample);
        stages.decode += DiagnosticsClock::now() - decode_started;
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            LogFailure(L"Reading and decoding the source MP4",
                       FAILED(result) ? result : E_FAIL);
            return false;
        }
        if ((flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) != 0) {
            LogFailure(L"The source MP4 changes media format during the trim interval");
            return false;
        }

        const bool is_video = actual_stream == source.video_stream;
        const bool is_audio = source.audio_stream.has_value() &&
                              actual_stream == *source.audio_stream;
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 &&
            ((is_video && video_started) || (is_audio && audio_started))) {
            LogFailure(L"The decoded media format changes during the trim interval");
            return false;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            if (is_video) {
                video_done = true;
            } else if (is_audio) {
                audio_done = true;
            }
        } else if (sample != nullptr) {
            if (source_time < 0) {
                LogFailure(L"The source MP4 contains a negative media timestamp");
                return false;
            }
            if (is_video) {
                video_started = true;
                if (source_time >= end.count()) {
                    video_done = true;
                    static_cast<void>(source.reader->SetStreamSelection(
                        source.video_stream, FALSE));
                } else {
                    const auto process_started = DiagnosticsClock::now();
                    const bool processed = ProcessVideoForWriter(
                        sample.Get(), source_time, source, start, end, writer,
                        last_video_end, frames_written, stages);
                    stages.process += DiagnosticsClock::now() - process_started;
                    if (!processed) {
                        return false;
                    }
                }
            } else if (is_audio) {
                audio_started = true;
                if (source_time >= end.count()) {
                    audio_done = true;
                    static_cast<void>(source.reader->SetStreamSelection(
                        *source.audio_stream, FALSE));
                } else {
                    const auto process_started = DiagnosticsClock::now();
                    const bool processed = ProcessAudioSample(
                        sample.Get(), source_time, source, start, end, writer,
                        last_audio_end, audio_packets_written, stages);
                    stages.process += DiagnosticsClock::now() - process_started;
                    if (!processed) {
                        return false;
                    }
                }
            }
        }

        const float processed = static_cast<float>((std::min)(source_time, end.count()));
        const float denominator = static_cast<float>((std::max)(int64_t{1}, end.count()));
        const float current_progress =
            (std::min)(0.99f, (std::max)(0.0f, processed / denominator) * 0.99f);
        if (current_progress > last_progress &&
            !ReportProgress(progress, current_progress)) {
            LogFailure(L"MP4 trim export cancelled by the progress callback");
            return false;
        }
        last_progress = current_progress;
    }

    if (frames_written == 0) {
        LogFailure(L"The requested interval contains no decodable video frames");
        return false;
    }
    if (source.audio_stream.has_value() && audio_packets_written == 0) {
        LogFailure(L"The requested interval contains no decodable audio frames");
        return false;
    }
    return true;
}

bool ExportOnWorker(const std::wstring& source_path,
                    const std::wstring& destination_path, MediaTime start,
                    MediaTime end, const RecordingOptions& options,
                    const ITrimExporter::ProgressCallback& progress,
                    bool replace_existing,
                    TrimExportDiagnostics& diagnostics) {
    using Clock = std::chrono::steady_clock;
    const auto total_started = Clock::now();
    MediaFoundationRuntime runtime;
    if (FAILED(runtime.result())) {
        LogFailure(L"Starting COM and Media Foundation for MP4 trim export",
                   runtime.result());
        return false;
    }
    if (!ValidatePaths(source_path, destination_path, replace_existing)) {
        return false;
    }
    if (start < MediaTime::zero() || end <= start) {
        LogFailure(L"The requested MP4 trim interval is empty or invalid");
        return false;
    }
    if (options.format != RecordingFormat::Mp4) {
        LogFailure(L"MP4 trim export requires MP4 recording options");
        return false;
    }
    if (!ReportProgress(progress, 0.0f)) {
        LogFailure(L"MP4 trim export cancelled before processing began");
        return false;
    }

    const auto staging_path = MakeStagingPath(destination_path);
    if (!staging_path.has_value()) {
        return false;
    }
    IncompleteFile incomplete_file(*staging_path);

    const auto source_open_started = Clock::now();
    auto d3d_device = MfD3dDevice::Create();
    std::optional<SourceDescription> source;
    std::unique_ptr<IMediaFoundationNv12Writer> nv12_writer;

    const auto abandon_staging = [&] {
        if (nv12_writer != nullptr) {
            nv12_writer->Abort();
            nv12_writer.reset();
        }
        static_cast<void>(::DeleteFileW(staging_path->c_str()));
    };
    const auto try_native_path = [&](IMFDXGIDeviceManager* manager) {
        source = OpenSource(source_path, manager, true);
        if (!source.has_value()) {
            return false;
        }
        nv12_writer = CreateMediaFoundationNv12Writer();
        return nv12_writer != nullptr;
    };

    bool native_path =
        d3d_device != nullptr && try_native_path(d3d_device->manager());
    if (!native_path) {
        source.reset();
        nv12_writer.reset();
        native_path = try_native_path(nullptr);
    }
    if (!native_path) {
        source = OpenSource(source_path, nullptr, false);
    }
    if (!source.has_value()) {
        return false;
    }
    if (start >= source->duration || end > source->duration) {
        LogFailure(L"The requested trim interval lies outside the source MP4 duration");
        return false;
    }

    RecordingOptions output_options = options;
    output_options.region = RECT{0, 0, static_cast<LONG>(source->width),
                                 static_cast<LONG>(source->height)};
    const uint64_t rounded_frame_rate =
        (static_cast<uint64_t>(source->frame_rate_numerator) +
         source->frame_rate_denominator / 2u) /
        source->frame_rate_denominator;
    if (rounded_frame_rate == 0 ||
        rounded_frame_rate > (std::numeric_limits<uint32_t>::max)()) {
        LogFailure(L"The source MP4 frame rate is unsupported");
        return false;
    }
    output_options.target_frame_rate = static_cast<uint32_t>(rounded_frame_rate);
    output_options.audio = source->audio_stream.has_value()
                               ? AudioSource::SystemAudio
                               : AudioSource::None;

    const AudioFormat* audio_format =
        source->audio_stream.has_value() ? &source->audio_format : nullptr;
    std::unique_ptr<IMediaWriter> fallback_writer;
    if (native_path &&
        !nv12_writer->Open(*staging_path, output_options, audio_format,
                           source->d3d_requested ? d3d_device->manager()
                                                 : nullptr)) {
        platform::LogMessage(
            L"[Trim] Native NV12 sink negotiation failed; retrying the bounded BGRA fallback");
        abandon_staging();
        source = OpenSource(source_path, nullptr, false);
        native_path = false;
        if (!source.has_value()) {
            return false;
        }
        audio_format = source->audio_stream.has_value()
                           ? &source->audio_format
                           : nullptr;
    }
    if (!native_path) {
        fallback_writer = CreateMediaFoundationWriter();
        if (fallback_writer == nullptr ||
            !fallback_writer->Open(*staging_path, output_options,
                                   audio_format)) {
            if (fallback_writer != nullptr) {
                LogFailure(fallback_writer->last_error().c_str());
            } else {
                LogFailure(L"Creating the production MP4 fallback writer");
            }
            return false;
        }
    }
    const auto source_open_finished = Clock::now();
    const auto process_started = source_open_finished;
    StageAccumulator stages;
    const bool transcoded =
        native_path
            ? TranscodeInterval(*source, start, end, *nv12_writer, progress,
                                stages)
            : TranscodeInterval(*source, start, end, *fallback_writer, progress,
                                stages);
    if (!transcoded) {
        if (native_path) {
            nv12_writer->Abort();
        } else {
            fallback_writer->Abort();
        }
        return false;
    }
    const auto process_finished = Clock::now();

    const auto finalize_started = process_finished;
    std::wstring finalized_path;
    const bool finalized = native_path
                               ? nv12_writer->Finalize(finalized_path)
                               : fallback_writer->Finalize(finalized_path);
    if (!finalized) {
        LogFailure((native_path ? nv12_writer->last_error()
                                : fallback_writer->last_error())
                       .c_str());
        return false;
    }
    const auto finalize_finished = Clock::now();
    if (finalized_path != *staging_path) {
        LogFailure(L"The MP4 writer finalized an unexpected output path");
        return false;
    }
    if (!ReportProgress(progress, 1.0f)) {
        LogFailure(L"MP4 trim export cancelled before destination publication");
        return false;
    }
    const auto publication_started = Clock::now();
    const DWORD publication_flags =
        MOVEFILE_WRITE_THROUGH |
        (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!::MoveFileExW(staging_path->c_str(), destination_path.c_str(),
                       publication_flags)) {
        LogFailure(L"Publishing the finalized MP4 trim destination",
                   HRESULT_FROM_WIN32(::GetLastError()));
        return false;
    }
    const auto publication_finished = Clock::now();

    incomplete_file.Disarm();
    const auto milliseconds = [](Clock::duration value) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(value)
            .count();
    };
    const wchar_t* path_name =
        native_path
            ? (source->dxgi_sample_seen ? L"d3d11-dxgi-nv12"
                                        : L"system-memory-nv12")
            : L"bounded-bgra-fallback";
    diagnostics.video_pipeline =
        native_path
            ? (source->dxgi_sample_seen
                   ? TrimVideoPipeline::D3d11DxgiNv12
                   : TrimVideoPipeline::SystemMemoryNv12)
            : TrimVideoPipeline::BoundedBgraFallback;
    if (source->d3d_requested && d3d_device != nullptr) {
        diagnostics.adapter_name = d3d_device->adapter_name();
    }
    diagnostics.source_open_ms = static_cast<uint64_t>(
        milliseconds(source_open_finished - source_open_started));
    const Clock::duration process_without_encode =
        stages.process >= stages.encode_submit
            ? stages.process - stages.encode_submit
            : Clock::duration::zero();
    diagnostics.decode_process_ms = static_cast<uint64_t>(
        milliseconds(stages.decode + process_without_encode));
    diagnostics.encode_finalize_ms = static_cast<uint64_t>(
        milliseconds(stages.encode_submit +
                     (finalize_finished - finalize_started)));
    diagnostics.publication_ms = static_cast<uint64_t>(
        milliseconds(publication_finished - publication_started));
    diagnostics.total_ms = static_cast<uint64_t>(
        milliseconds(publication_finished - total_started));
    platform::LogMessage(
        L"[TrimTiming] path=%s adapter=%s source-open=%lld ms decode-process=%llu ms encode-finalize=%llu ms publication=%lld ms total=%lld ms",
        path_name,
        diagnostics.adapter_name.empty() ? L"not-applicable"
                                         : diagnostics.adapter_name.c_str(),
        milliseconds(source_open_finished - source_open_started),
        diagnostics.decode_process_ms, diagnostics.encode_finalize_ms,
        milliseconds(publication_finished - publication_started),
        milliseconds(publication_finished - total_started));
    return true;
}

class Mp4TrimExporter final : public ITrimExporter {
public:
    bool Export(const std::wstring& source_path,
                const std::wstring& destination_path, MediaTime start,
                MediaTime end, const RecordingOptions& options,
                ProgressCallback progress,
                bool replace_existing = false) override {
        bool result = false;
        TrimExportDiagnostics diagnostics;
        try {
            std::thread worker([&] {
                try {
                    result = ExportOnWorker(source_path, destination_path, start,
                                            end, options, progress,
                                            replace_existing, diagnostics);
                } catch (const std::bad_alloc&) {
                    LogFailure(L"Allocating bounded MP4 trim state", E_OUTOFMEMORY);
                } catch (...) {
                    LogFailure(L"An unexpected MP4 trim export error occurred");
                }
            });
            worker.join();
        } catch (const std::system_error&) {
            LogFailure(L"Starting the MP4 trim worker thread");
        } catch (...) {
            LogFailure(L"Preparing the MP4 trim export");
        }
        try {
            const std::scoped_lock lock(diagnostics_mutex_);
            diagnostics_ = std::move(diagnostics);
        } catch (const std::bad_alloc&) {
            // Diagnostics are advisory and must never turn a published export
            // into an apparent failure.
        }
        return result;
    }

    [[nodiscard]] std::optional<uint64_t> EstimateOutputBytes(
        const std::wstring&, MediaTime, MediaTime,
        const RecordingOptions&) const override {
        return std::nullopt;
    }

    [[nodiscard]] TrimExportDiagnostics LastDiagnostics() const override {
        const std::scoped_lock lock(diagnostics_mutex_);
        return diagnostics_;
    }

private:
    mutable std::mutex diagnostics_mutex_;
    TrimExportDiagnostics diagnostics_{};
};

}  // namespace

std::unique_ptr<ITrimExporter> CreateMp4TrimExporter() {
    try {
        return std::make_unique<Mp4TrimExporter>();
    } catch (...) {
        return nullptr;
    }
}

}  // namespace easnap::media
