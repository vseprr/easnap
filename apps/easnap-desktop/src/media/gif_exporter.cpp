#include "media/mf_video_aperture.h"
// Easnap - finalized-MP4 adapter for the presentation-independent GIF encoder.
#include "media/gif_encoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

namespace easnap::media {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kMaximumSourceStreams = 32;
constexpr int64_t kMediaTicksPerSecond = 10'000'000;
constexpr int64_t kDeterministicFallbackFrameDuration = 333'333;

void LogFailure(const wchar_t* message) {
    platform::LogMessage(L"[GIF] %s", message);
}

void LogFailure(const wchar_t* operation, HRESULT result) {
    platform::LogHresult(operation, result);
}

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

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool com_initialized_{false};
    bool mf_started_{false};
};

std::optional<MediaTime> ReadPresentationDuration(IMFSourceReader* reader,
                                                  HRESULT& read_result) {
    PROPVARIANT value;
    ::PropVariantInit(&value);
    read_result = reader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value);
    if (FAILED(read_result)) {
        LogFailure(L"Reading the GIF source presentation duration", read_result);
        return std::nullopt;
    }
    std::optional<MediaTime> duration;
    if (value.vt == VT_UI8 &&
        value.uhVal.QuadPart <=
            static_cast<ULONGLONG>((std::numeric_limits<int64_t>::max)())) {
        duration = MediaTime{static_cast<int64_t>(value.uhVal.QuadPart)};
    } else if (value.vt == VT_I8 && value.hVal.QuadPart > 0) {
        duration = MediaTime{value.hVal.QuadPart};
    }
    ::PropVariantClear(&value);
    return duration;
}

class MfGifFrameSource final : public IGifFrameSource {
public:
    static std::unique_ptr<MfGifFrameSource> Create(
        std::wstring source_path, MediaTime start, MediaTime end,
        TrimExportFailure& failure) {
        auto source = std::unique_ptr<MfGifFrameSource>(
            new MfGifFrameSource(std::move(source_path), start, end));
        if (!source->OpenReader(true)) {
            failure = source->failure_;
            return nullptr;
        }
        return source;
    }

    [[nodiscard]] uint32_t width() const noexcept override { return width_; }
    [[nodiscard]] uint32_t height() const noexcept override { return height_; }
    [[nodiscard]] MediaTime duration() const noexcept override {
        return end_ - start_;
    }

    void SetCancellationProbe(std::function<bool()> probe) override {
        cancellation_probe_ = std::move(probe);
    }

    bool Reset() override {
        pending_.reset();
        end_of_stream_ = false;
        return OpenReader(false);
    }

    GifFrameReadResult Read(VideoFrame& frame) override {
        frame = {};
        for (;;) {
            RawFrame next;
            const RawReadResult result = ReadRaw(next);
            if (result == RawReadResult::Error) {
                return GifFrameReadResult::Error;
            }
            if (result == RawReadResult::Frame) {
                if (!pending_.has_value()) {
                    pending_ = std::move(next);
                    continue;
                }
                RawFrame current = std::move(*pending_);
                pending_ = std::move(next);
                if (pending_->timestamp <= current.timestamp) {
                    SetError(TrimExportFailureStage::VideoDecode,
                             L"The decoded MP4 has non-monotonic video timestamps");
                    return GifFrameReadResult::Error;
                }
                fallback_frame_duration_ =
                    pending_->timestamp - current.timestamp;
                if (pending_->timestamp >= end_) {
                    end_of_stream_ = true;
                    pending_.reset();
                }
                if (Clip(current, pending_.has_value() ? pending_->timestamp : end_,
                         frame)) {
                    return GifFrameReadResult::Frame;
                }
                if (end_of_stream_) {
                    return GifFrameReadResult::End;
                }
                continue;
            }

            if (!pending_.has_value()) {
                return GifFrameReadResult::End;
            }
            RawFrame current = std::move(*pending_);
            pending_.reset();
            const MediaTime natural_end =
                current.duration > MediaTime::zero()
                    ? current.timestamp + current.duration
                    : current.timestamp + fallback_frame_duration_;
            if (Clip(current, (std::min)(natural_end, end_), frame)) {
                return GifFrameReadResult::Frame;
            }
            return GifFrameReadResult::End;
        }
    }

    [[nodiscard]] std::wstring last_error() const override { return error_; }
    [[nodiscard]] TrimExportFailure last_failure() const override {
        return failure_;
    }

private:
    struct RawFrame final {
        std::shared_ptr<const platform::BgraImage> image;
        MediaTime timestamp{0};
        MediaTime duration{0};
    };

    struct DecodedFormat final {
        UINT32 buffer_width{0};
        UINT32 buffer_height{0};
        UINT32 crop_left{0};
        UINT32 crop_top{0};
        LONG stride{0};
        std::optional<MediaTime> nominal_frame_duration;
    };

    enum class RawReadResult { Frame, End, Error };

    MfGifFrameSource(std::wstring source_path, MediaTime start, MediaTime end)
        : source_path_(std::move(source_path)), start_(start), end_(end) {}

    void SetError(TrimExportFailureStage stage, const wchar_t* message) {
        error_ = message;
        failure_.stage = stage;
        failure_.has_hresult = false;
        failure_.hresult = 0;
        failure_.detail = message;
        LogFailure(message);
    }

    void SetError(TrimExportFailureStage stage, const wchar_t* message,
                  HRESULT result) {
        SetError(stage, message);
        failure_.has_hresult = FAILED(result);
        failure_.hresult = FAILED(result) ? static_cast<int32_t>(result) : 0;
        LogFailure(message, result);
    }

    bool QueryDecodedFormat(UINT32 expected_width, UINT32 expected_height,
                            DecodedFormat& format, HRESULT& result) const {
        format = {};
        ComPtr<IMFMediaType> actual_type;
        result = reader_->GetCurrentMediaType(video_stream_, &actual_type);
        GUID major{};
        GUID subtype{};
        if (SUCCEEDED(result)) {
            result = actual_type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        }
        if (SUCCEEDED(result)) {
            result = actual_type->GetGUID(MF_MT_SUBTYPE, &subtype);
        }
        if (SUCCEEDED(result)) {
            result = ::MFGetAttributeSize(actual_type.Get(), MF_MT_FRAME_SIZE,
                                          &format.buffer_width,
                                          &format.buffer_height);
        }
        if (SUCCEEDED(result) &&
            (major != MFMediaType_Video || subtype != MFVideoFormat_RGB32 ||
             format.buffer_width == 0 || format.buffer_height == 0 ||
             format.buffer_width >
                 static_cast<UINT32>((std::numeric_limits<int>::max)()) ||
             format.buffer_height >
                 static_cast<UINT32>((std::numeric_limits<int>::max)()) ||
             format.buffer_width > static_cast<UINT32>(
                         (std::numeric_limits<LONG>::max)() / 4))) {
            result = MF_E_INVALIDMEDIATYPE;
        }
        UINT32 raw_stride = 0;
        if (SUCCEEDED(result) &&
            SUCCEEDED(actual_type->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                             &raw_stride))) {
            format.stride = static_cast<LONG>(raw_stride);
        } else if (SUCCEEDED(result)) {
            format.stride = static_cast<LONG>(format.buffer_width * 4u);
        }
        if (SUCCEEDED(result) &&
            (format.stride == 0 ||
             static_cast<uint64_t>(
                 std::abs(static_cast<int64_t>(format.stride))) <
                 static_cast<uint64_t>(format.buffer_width) * 4u ||
             expected_width == 0 || expected_height == 0 ||
             expected_width > format.buffer_width ||
             expected_height > format.buffer_height ||
             !ResolveExpectedAperture(
                 actual_type.Get(), format.buffer_width, format.buffer_height,
                 expected_width, expected_height, format.crop_left,
                 format.crop_top))) {
            result = MF_E_INVALIDMEDIATYPE;
        }

        UINT32 numerator = 0;
        UINT32 denominator = 0;
        if (SUCCEEDED(result) &&
            SUCCEEDED(::MFGetAttributeRatio(actual_type.Get(), MF_MT_FRAME_RATE,
                                            &numerator, &denominator)) &&
            numerator > 0 && denominator > 0) {
            const uint64_t ticks =
                static_cast<uint64_t>(kMediaTicksPerSecond) * denominator;
            format.nominal_frame_duration =
                MediaTime{static_cast<int64_t>((std::max)(
                    uint64_t{1}, (ticks + numerator / 2u) / numerator))};
        }
        return SUCCEEDED(result);
    }

    bool AcceptInitialDecodedTypeChange() {
        if (decoded_frame_count_ != 0 || accepted_initial_type_change_) {
            SetError(TrimExportFailureStage::MediaTypeNegotiation,
                     L"The decoded GIF video type changed after decoding began");
            return false;
        }
        DecodedFormat format;
        HRESULT result = S_OK;
        if (!QueryDecodedFormat(width_, height_, format, result)) {
            SetError(TrimExportFailureStage::MediaTypeNegotiation,
                     L"Media Foundation changed to an unsupported decoded GIF video type",
                     result);
            ComPtr<IMFMediaType> reported_type;
            GUID reported_major{};
            GUID reported_subtype{};
            UINT32 reported_width = 0;
            UINT32 reported_height = 0;
            UINT32 reported_stride = 0;
            if (SUCCEEDED(reader_->GetCurrentMediaType(video_stream_,
                                                        &reported_type))) {
                static_cast<void>(reported_type->GetGUID(MF_MT_MAJOR_TYPE,
                                                         &reported_major));
                static_cast<void>(reported_type->GetGUID(MF_MT_SUBTYPE,
                                                         &reported_subtype));
                static_cast<void>(::MFGetAttributeSize(
                    reported_type.Get(), MF_MT_FRAME_SIZE, &reported_width,
                    &reported_height));
                static_cast<void>(reported_type->GetUINT32(
                    MF_MT_DEFAULT_STRIDE, &reported_stride));
            }
            wchar_t detail[320]{};
            ::swprintf(
                detail, ARRAYSIZE(detail),
                L"Unsupported decoded transition: major=%08X subtype=%08X size=%ux%u stride=%ld; expected RGB32 %ux%u",
                reported_major.Data1, reported_subtype.Data1, reported_width,
                reported_height, static_cast<LONG>(reported_stride), width_,
                height_);
            error_ = detail;
            failure_.detail = detail;
            platform::LogMessage(L"[GIF] %s", detail);
            return false;
        }
        decoded_width_ = format.buffer_width;
        decoded_height_ = format.buffer_height;
        crop_left_ = format.crop_left;
        crop_top_ = format.crop_top;
        stride_ = format.stride;
        accepted_initial_type_change_ = true;
        platform::LogMessage(
            L"[GIF] Accepted the initial Media Foundation RGB32 type transition (buffer %ux%u, crop %u,%u %ux%u, stride %ld)",
            decoded_width_, decoded_height_, crop_left_, crop_top_, width_,
            height_, stride_);
        return true;
    }

    bool OpenReader(bool describe) {
        error_.clear();
        failure_ = {};
        reader_.Reset();
        decoded_frame_count_ = 0;
        accepted_initial_type_change_ = false;
        ComPtr<IMFAttributes> attributes;
        HRESULT result = ::MFCreateAttributes(&attributes, 4);
        if (SUCCEEDED(result)) {
            result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                                           TRUE);
        }
        if (SUCCEEDED(result)) {
            result = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        }
        if (SUCCEEDED(result)) {
            result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                                           TRUE);
        }
        if (SUCCEEDED(result)) {
            result = attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
        }
        if (SUCCEEDED(result)) {
            result = ::MFCreateSourceReaderFromURL(source_path_.c_str(),
                                                   attributes.Get(), &reader_);
        }
        if (FAILED(result)) {
            SetError(describe ? TrimExportFailureStage::SourceOpen
                              : TrimExportFailureStage::SourceReset,
                     L"Opening the MP4 GIF source failed", result);
            return false;
        }

        if (describe) {
            HRESULT duration_result = S_OK;
            const auto source_duration =
                ReadPresentationDuration(reader_.Get(), duration_result);
            if (!source_duration.has_value() || *source_duration <= MediaTime::zero() ||
                start_ < MediaTime::zero() || end_ <= start_ ||
                start_ >= *source_duration || end_ > *source_duration) {
                if (FAILED(duration_result)) {
                    SetError(TrimExportFailureStage::SourceValidation,
                             L"Reading the MP4 presentation duration failed",
                             duration_result);
                } else {
                    SetError(TrimExportFailureStage::SourceValidation,
                             L"The requested GIF interval is outside the MP4 duration");
                }
                return false;
            }
        }

        bool found_video = false;
        UINT32 native_width = 0;
        UINT32 native_height = 0;
        for (DWORD stream = 0; stream < kMaximumSourceStreams; ++stream) {
            ComPtr<IMFMediaType> native_type;
            result = reader_->GetNativeMediaType(stream, 0, &native_type);
            if (result == MF_E_INVALIDSTREAMNUMBER) {
                break;
            }
            if (FAILED(result)) {
                continue;
            }
            GUID major{};
            GUID subtype{};
            if (SUCCEEDED(native_type->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
                SUCCEEDED(native_type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                major == MFMediaType_Video) {
                if (subtype != MFVideoFormat_H264) {
                    SetError(TrimExportFailureStage::SourceValidation,
                             L"The GIF source video stream is not H.264");
                    return false;
                }
                result = ::MFGetAttributeSize(native_type.Get(), MF_MT_FRAME_SIZE,
                                              &native_width, &native_height);
                if (FAILED(result) || native_width == 0 || native_height == 0) {
                    SetError(describe
                                 ? TrimExportFailureStage::SourceValidation
                                 : TrimExportFailureStage::SourceReset,
                             L"Reading the GIF source display dimensions failed",
                             result);
                    return false;
                }
                video_stream_ = stream;
                found_video = true;
                break;
            }
        }
        if (!found_video) {
            SetError(describe ? TrimExportFailureStage::SourceValidation
                              : TrimExportFailureStage::SourceReset,
                     L"The GIF source MP4 has no H.264 video stream");
            return false;
        }
        if (!describe &&
            (native_width != width_ || native_height != height_)) {
            SetError(TrimExportFailureStage::SourceReset,
                     L"The GIF source dimensions changed between encoding passes");
            return false;
        }
        if (describe) {
            width_ = native_width;
            height_ = native_height;
        }

        result = reader_->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (SUCCEEDED(result)) {
            result = reader_->SetStreamSelection(video_stream_, TRUE);
        }
        ComPtr<IMFMediaType> decoded_type;
        if (SUCCEEDED(result)) {
            result = ::MFCreateMediaType(&decoded_type);
        }
        if (SUCCEEDED(result)) {
            result = decoded_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(result)) {
            result = decoded_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        }
        if (SUCCEEDED(result)) {
            result = reader_->SetCurrentMediaType(video_stream_, nullptr,
                                                  decoded_type.Get());
        }
        DecodedFormat format;
        if (SUCCEEDED(result)) {
            static_cast<void>(
                QueryDecodedFormat(width_, height_, format, result));
        }
        if (FAILED(result)) {
            SetError(describe ? TrimExportFailureStage::MediaTypeNegotiation
                              : TrimExportFailureStage::SourceReset,
                     L"The decoded GIF source video format is invalid", result);
            return false;
        }
        if (describe) {
            initial_fallback_frame_duration_ =
                format.nominal_frame_duration.value_or(
                    MediaTime{kDeterministicFallbackFrameDuration});
        }
        decoded_width_ = format.buffer_width;
        decoded_height_ = format.buffer_height;
        crop_left_ = format.crop_left;
        crop_top_ = format.crop_top;
        stride_ = format.stride;
        fallback_frame_duration_ = initial_fallback_frame_duration_;
        return true;
    }

    RawReadResult ReadRaw(RawFrame& frame) {
        if (end_of_stream_) {
            return RawReadResult::End;
        }
        for (;;) {
            if (cancellation_probe_ && !cancellation_probe_()) {
                error_ = L"GIF export was cancelled while decoding the MP4 source";
                failure_ = {};
                return RawReadResult::Error;
            }
            DWORD actual_stream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            const HRESULT result = reader_->ReadSample(
                video_stream_, 0, &actual_stream, &flags, &timestamp, &sample);
            if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
                SetError(TrimExportFailureStage::VideoDecode,
                         L"Reading and decoding the MP4 GIF source failed",
                         FAILED(result) ? result : E_FAIL);
                return RawReadResult::Error;
            }
            if ((flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) != 0) {
                SetError(TrimExportFailureStage::MediaTypeNegotiation,
                         L"The native GIF source video type changed during export");
                return RawReadResult::Error;
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 &&
                !AcceptInitialDecodedTypeChange()) {
                return RawReadResult::Error;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                end_of_stream_ = true;
                return RawReadResult::End;
            }
            if (sample == nullptr) {
                continue;
            }
            if (timestamp < 0) {
                SetError(TrimExportFailureStage::VideoDecode,
                         L"The GIF source contains a negative video timestamp");
                return RawReadResult::Error;
            }
            std::shared_ptr<const platform::BgraImage> image;
            if (!CopySample(sample.Get(), image)) {
                return RawReadResult::Error;
            }
            LONGLONG duration = 0;
            if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) {
                duration = 0;
            }
            frame.image = std::move(image);
            frame.timestamp = MediaTime{timestamp};
            frame.duration = MediaTime{duration};
            ++decoded_frame_count_;
            return RawReadResult::Frame;
        }
    }

    bool CopySample(IMFSample* sample,
                    std::shared_ptr<const platform::BgraImage>& image) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT result = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(result)) {
            SetError(TrimExportFailureStage::DecodedFrameAccess,
                     L"Accessing a decoded GIF video sample failed", result);
            return false;
        }
        try {
            auto output = std::make_shared<platform::BgraImage>();
            output->Reset(static_cast<int>(width_), static_cast<int>(height_));
            ComPtr<IMF2DBuffer> buffer_2d;
            result = buffer.As(&buffer_2d);
            if (SUCCEEDED(result)) {
                BYTE* scanline = nullptr;
                LONG pitch = 0;
                result = buffer_2d->Lock2D(&scanline, &pitch);
                if (SUCCEEDED(result) && scanline != nullptr &&
                    static_cast<uint64_t>(std::abs(static_cast<int64_t>(pitch))) >=
                        static_cast<uint64_t>(decoded_width_) * 4u) {
                    for (UINT32 row = 0; row < height_; ++row) {
                        const int64_t source_row =
                            static_cast<int64_t>(crop_top_ + row) * pitch;
                        std::memcpy(
                            output->row(static_cast<int>(row)),
                            scanline + source_row +
                                static_cast<std::size_t>(crop_left_) * 4u,
                            static_cast<std::size_t>(width_) * 4u);
                    }
                } else if (SUCCEEDED(result)) {
                    result = E_UNEXPECTED;
                }
                if (scanline != nullptr) {
                    const HRESULT unlock = buffer_2d->Unlock2D();
                    if (SUCCEEDED(result)) {
                        result = unlock;
                    }
                }
            } else {
                BYTE* bytes = nullptr;
                DWORD byte_count = 0;
                result = buffer->Lock(&bytes, nullptr, &byte_count);
                const uint64_t stride = static_cast<uint64_t>(
                    std::abs(static_cast<int64_t>(stride_)));
                const uint64_t required = stride * decoded_height_;
                if (SUCCEEDED(result) && bytes != nullptr &&
                    stride >= static_cast<uint64_t>(decoded_width_) * 4u &&
                    required <= byte_count) {
                    BYTE* first_row = bytes;
                    if (stride_ < 0) {
                        first_row += stride * (decoded_height_ - 1u);
                    }
                    for (UINT32 row = 0; row < height_; ++row) {
                        const int64_t source_row =
                            static_cast<int64_t>(crop_top_ + row) * stride_;
                        std::memcpy(
                            output->row(static_cast<int>(row)),
                            first_row + source_row +
                                static_cast<std::size_t>(crop_left_) * 4u,
                            static_cast<std::size_t>(width_) * 4u);
                    }
                } else if (SUCCEEDED(result)) {
                    result = E_UNEXPECTED;
                }
                if (bytes != nullptr) {
                    const HRESULT unlock = buffer->Unlock();
                    if (SUCCEEDED(result)) {
                        result = unlock;
                    }
                }
            }
            if (FAILED(result)) {
                SetError(TrimExportFailureStage::DecodedFrameAccess,
                         L"Copying a bounded decoded GIF frame failed", result);
                return false;
            }
            output->MakeOpaque();
            image = std::move(output);
            return true;
        } catch (const std::bad_alloc&) {
            SetError(TrimExportFailureStage::DecodedFrameAccess,
                     L"Allocating a bounded decoded GIF frame failed");
            return false;
        }
    }

    bool Clip(const RawFrame& source, MediaTime natural_end,
              VideoFrame& output) const {
        const MediaTime clipped_start = (std::max)(source.timestamp, start_);
        const MediaTime clipped_end = (std::min)(natural_end, end_);
        if (clipped_end <= clipped_start) {
            return false;
        }
        output.image = source.image;
        output.timestamp = clipped_start - start_;
        output.duration = clipped_end - clipped_start;
        return true;
    }

    std::wstring source_path_;
    MediaTime start_{0};
    MediaTime end_{0};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t decoded_width_{0};
    uint32_t decoded_height_{0};
    uint32_t crop_left_{0};
    uint32_t crop_top_{0};
    LONG stride_{0};
    DWORD video_stream_{0};
    MediaTime initial_fallback_frame_duration_{
        kDeterministicFallbackFrameDuration};
    MediaTime fallback_frame_duration_{kDeterministicFallbackFrameDuration};
    ComPtr<IMFSourceReader> reader_;
    std::optional<RawFrame> pending_;
    std::wstring error_;
    TrimExportFailure failure_;
    std::function<bool()> cancellation_probe_;
    uint64_t decoded_frame_count_{0};
    bool accepted_initial_type_change_{false};
    bool end_of_stream_{false};
};

bool IsAbsoluteMp4Path(const std::wstring& path) noexcept {
    return !path.empty() && ::PathIsRelativeW(path.c_str()) == FALSE &&
           _wcsicmp(::PathFindExtensionW(path.c_str()), L".mp4") == 0;
}

bool ExportOnWorker(const std::wstring& source_path,
                    const std::wstring& destination_path, MediaTime start,
                    MediaTime end, const RecordingOptions& options,
                    const ITrimExporter::ProgressCallback& progress,
                    bool replace_existing, TrimExportFailure& failure) {
    failure = {};
    MediaFoundationRuntime runtime;
    if (FAILED(runtime.result())) {
        LogFailure(L"Starting Media Foundation for GIF export", runtime.result());
        failure.stage = TrimExportFailureStage::SourceOpen;
        failure.has_hresult = true;
        failure.hresult = static_cast<int32_t>(runtime.result());
        failure.detail = L"Starting Media Foundation for GIF export failed";
        return false;
    }
    if (!IsAbsoluteMp4Path(source_path)) {
        LogFailure(L"The GIF source must be an absolute .mp4 path");
        failure.stage = TrimExportFailureStage::SourceValidation;
        failure.detail = L"The GIF source must be an absolute .mp4 path";
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(source_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        LogFailure(L"The GIF source MP4 does not exist or is not a file");
        failure.stage = TrimExportFailureStage::SourceValidation;
        failure.detail = L"The GIF source MP4 does not exist or is not a file";
        return false;
    }
    if (options.format != RecordingFormat::Gif) {
        LogFailure(L"GIF export requires GIF recording options");
        failure.stage = TrimExportFailureStage::SourceValidation;
        failure.detail = L"GIF export requires GIF recording options";
        return false;
    }

    auto source = MfGifFrameSource::Create(source_path, start, end, failure);
    if (source == nullptr) {
        return false;
    }
    GifEncodeOptions gif_options;
    gif_options.output_width = options.gif_width;
    gif_options.output_height = options.gif_height;
    gif_options.maximum_frame_rate = options.gif_fps;
    gif_options.maximum_palette_colors = options.gif_palette_colors;
    gif_options.quantization = options.gif_fixed_rgb332
                                   ? GifQuantizationPolicy::Rgb332Ordered
                                   : (options.gif_dither
                                          ? GifQuantizationPolicy::Balanced
                                          : GifQuantizationPolicy::Sharp);
    gif_options.palette = options.gif_low_size ? GifPalettePolicy::FixedWebSafe
        : options.gif_fixed_rgb332 ? GifPalettePolicy::FixedRgb332
                                  : GifPalettePolicy::AdaptiveMedianCut;
    gif_options.temporal_tolerance = options.gif_low_size ? 8 : 0;
    gif_options.transparent_sparse_deltas =
        options.gif_sparse_transparency;
    gif_options.distributed_cadence = options.gif_distributed_cadence;
    gif_options.loop_count = 0;
    GifEncodeDiagnostics diagnostics;
    std::wstring error;
    return ExportGif(*source, destination_path, gif_options, progress,
                     replace_existing, diagnostics, error, &failure);
}

class GifExporter final : public ITrimExporter {
public:
    bool Export(const std::wstring& source_path,
                const std::wstring& destination_path, MediaTime start,
                MediaTime end, const RecordingOptions& options,
                ProgressCallback progress,
                bool replace_existing = false) override {
        bool exported = false;
        TrimExportFailure failure;
        try {
            std::thread worker([&] {
                try {
                    exported = ExportOnWorker(source_path, destination_path, start,
                                              end, options, progress,
                                              replace_existing, failure);
                } catch (const std::bad_alloc&) {
                    LogFailure(L"Allocating bounded GIF export state failed");
                    failure.stage = TrimExportFailureStage::GifEncoding;
                    failure.detail = L"Allocating bounded GIF export state failed";
                } catch (...) {
                    LogFailure(L"An unexpected GIF export error occurred");
                    failure.stage = TrimExportFailureStage::Unknown;
                    failure.detail = L"An unexpected GIF export error occurred";
                }
            });
            worker.join();
        } catch (const std::system_error&) {
            LogFailure(L"Starting the GIF export worker failed");
            failure.stage = TrimExportFailureStage::Unknown;
            failure.detail = L"Starting the GIF export worker failed";
        } catch (...) {
            LogFailure(L"Preparing GIF export failed");
            failure.stage = TrimExportFailureStage::Unknown;
            failure.detail = L"Preparing GIF export failed";
        }
        {
            const std::scoped_lock lock(failure_mutex_);
            failure_ = std::move(failure);
        }
        return exported;
    }

    [[nodiscard]] std::optional<uint64_t> EstimateOutputBytes(
        const std::wstring&, MediaTime, MediaTime,
        const RecordingOptions&) const override {
        return std::nullopt;
    }

    [[nodiscard]] TrimExportFailure LastFailure() const override {
        const std::scoped_lock lock(failure_mutex_);
        return failure_;
    }

private:
    mutable std::mutex failure_mutex_;
    TrimExportFailure failure_;
};

}  // namespace

std::unique_ptr<ITrimExporter> CreateGifExporter() {
    return std::make_unique<GifExporter>();
}

}  // namespace easnap::media
