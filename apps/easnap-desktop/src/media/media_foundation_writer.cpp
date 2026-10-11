#include "media/media_foundation_writer.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace easnap::media {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kMinimumVideoBitrate = 100'000;
constexpr uint32_t kMaximumVideoBitrate = 100'000'000;
constexpr uint32_t kMaximumFrameRate = 240;
constexpr uint32_t kAacBitsPerSample = 16;
constexpr uint32_t kAacBytesPerSecond96Kbps = 12'000;
constexpr uint32_t kAacBytesPerSecond128Kbps = 16'000;
constexpr uint32_t kAacBytesPerSecond160Kbps = 20'000;
constexpr uint32_t kAacBytesPerSecond192Kbps = 24'000;
constexpr uint32_t kAacProfileLevel2 = 0x29;
constexpr uint32_t kMonoChannelMask = 0x4;
constexpr uint32_t kStereoChannelMask = 0x3;

bool IsSupportedAacBitrate(uint32_t bitrate) noexcept {
    return bitrate == kAacBytesPerSecond96Kbps * 8 ||
           bitrate == kAacBytesPerSecond128Kbps * 8 ||
           bitrate == kAacBytesPerSecond160Kbps * 8 ||
           bitrate == kAacBytesPerSecond192Kbps * 8;
}

std::wstring DescribeHresult(const wchar_t* operation, HRESULT result) {
    wchar_t code[16]{};
    _snwprintf_s(code, _TRUNCATE, L"0x%08lX", static_cast<unsigned long>(result));

    wchar_t system_message[512]{};
    const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                           nullptr, static_cast<DWORD>(result), 0, system_message,
                                           static_cast<DWORD>(_countof(system_message)), nullptr);
    std::size_t trimmed_length = std::wcslen(system_message);
    while (trimmed_length > 0 && (system_message[trimmed_length - 1] == L'\r' ||
                                  system_message[trimmed_length - 1] == L'\n')) {
        system_message[--trimmed_length] = L'\0';
    }

    std::wstring message(operation);
    message += L" failed (HRESULT ";
    message += code;
    message += L")";
    if (result == MF_E_TOPO_CODEC_NOT_FOUND) {
        message += L": no compatible Media Foundation encoder is installed";
    } else if (length != 0 && system_message[0] != L'\0') {
        message += L": ";
        message += system_message;
    }
    return message;
}

uint8_t ClampByte(int value) noexcept {
    return static_cast<uint8_t>((std::clamp)(value, 0, 255));
}

int RoundedDivideBy256(int value) noexcept {
    return value >= 0 ? (value + 128) / 256 : -((-value + 128) / 256);
}

struct Rgb final {
    int red;
    int green;
    int blue;
};

Rgb UnpackBgra(uint32_t pixel) noexcept {
    return Rgb{static_cast<int>((pixel >> 16) & 0xFFu),
               static_cast<int>((pixel >> 8) & 0xFFu),
               static_cast<int>(pixel & 0xFFu)};
}

uint8_t ToLimitedRangeLuma(const Rgb& color) noexcept {
    return ClampByte(((47 * color.red + 157 * color.green + 16 * color.blue + 128) / 256) + 16);
}

void ConvertBgraToNv12(const platform::BgraImage& image, uint8_t* destination) noexcept {
    const int width = image.width;
    const int height = image.height;
    uint8_t* const luma = destination;
    uint8_t* const chroma = destination + static_cast<std::size_t>(width) * height;

    for (int y = 0; y < height; y += 2) {
        const uint32_t* const upper = image.row(y);
        const uint32_t* const lower = image.row(y + 1);
        uint8_t* const upper_luma = luma + static_cast<std::size_t>(y) * width;
        uint8_t* const lower_luma = upper_luma + width;
        uint8_t* const uv = chroma + static_cast<std::size_t>(y / 2) * width;

        for (int x = 0; x < width; x += 2) {
            const Rgb c00 = UnpackBgra(upper[x]);
            const Rgb c01 = UnpackBgra(upper[x + 1]);
            const Rgb c10 = UnpackBgra(lower[x]);
            const Rgb c11 = UnpackBgra(lower[x + 1]);

            upper_luma[x] = ToLimitedRangeLuma(c00);
            upper_luma[x + 1] = ToLimitedRangeLuma(c01);
            lower_luma[x] = ToLimitedRangeLuma(c10);
            lower_luma[x + 1] = ToLimitedRangeLuma(c11);

            const Rgb average{(c00.red + c01.red + c10.red + c11.red + 2) / 4,
                              (c00.green + c01.green + c10.green + c11.green + 2) / 4,
                              (c00.blue + c01.blue + c10.blue + c11.blue + 2) / 4};
            uv[x] = ClampByte(128 + RoundedDivideBy256(
                                           -26 * average.red - 87 * average.green + 112 * average.blue));
            uv[x + 1] = ClampByte(128 + RoundedDivideBy256(
                                               112 * average.red - 102 * average.green - 10 * average.blue));
        }
    }
}

class MediaFoundationWriter final : public IMediaWriter,
                                    public IMediaFoundationNv12Writer {
public:
    MediaFoundationWriter() {
        // The command flags, COM startup result and sink state are declared
        // after worker_. Starting in its member initializer lets ThreadMain
        // race their initialization (including resetting a completed startup
        // to E_PENDING). Publish this only after every member is constructed.
        worker_ = std::thread(&MediaFoundationWriter::ThreadMain, this);
    }

    ~MediaFoundationWriter() override {
        ShutdownWorker();
    }

    bool Open(const std::wstring& output_path, const RecordingOptions& options,
              const AudioFormat* audio_format) override {
        const std::scoped_lock api_lock(api_mutex_);
        try {
            requested_path_ = output_path;
            requested_options_ = options;
            requested_native_nv12_ = false;
            requested_d3d_manager_.Reset();
            requested_audio_format_ = audio_format != nullptr
                                          ? std::optional<AudioFormat>(*audio_format)
                                          : std::nullopt;
        } catch (const std::bad_alloc&) {
            SetError(L"Preparing the writer open command", E_OUTOFMEMORY);
            return false;
        }
        return Execute(Command::Open);
    }

    bool Open(const std::wstring& output_path, const RecordingOptions& options,
              const AudioFormat* audio_format,
              IMFDXGIDeviceManager* device_manager) override {
        const std::scoped_lock api_lock(api_mutex_);
        try {
            requested_path_ = output_path;
            requested_options_ = options;
            requested_native_nv12_ = true;
            requested_d3d_manager_ = device_manager;
            requested_audio_format_ = audio_format != nullptr
                                          ? std::optional<AudioFormat>(*audio_format)
                                          : std::nullopt;
        } catch (const std::bad_alloc&) {
            SetError(L"Preparing the native-NV12 writer open command",
                     E_OUTOFMEMORY);
            return false;
        }
        return Execute(Command::Open);
    }

    bool WriteVideoFrame(const VideoFrame& frame) override {
        const std::scoped_lock api_lock(api_mutex_);
        requested_frame_ = &frame;
        const bool result = Execute(Command::WriteVideoFrame);
        requested_frame_ = nullptr;
        return result;
    }

    [[nodiscard]] bool EncoderReadsSurfaces() const override {
        return encoder_reads_surfaces_.load(std::memory_order_acquire);
    }

    bool WriteVideoSample(IMFSample* sample, MediaTime timestamp,
                          MediaTime duration) override {
        const std::scoped_lock api_lock(api_mutex_);
        requested_video_sample_ = sample;
        requested_video_timestamp_ = timestamp;
        requested_video_duration_ = duration;
        const bool result = Execute(Command::WriteVideoSample);
        requested_video_sample_ = nullptr;
        return result;
    }

    bool WriteAudioPacket(const MediaChunk& packet) override {
        const std::scoped_lock api_lock(api_mutex_);
        requested_audio_packet_ = &packet;
        const bool result = Execute(Command::WriteAudioPacket);
        requested_audio_packet_ = nullptr;
        return result;
    }

    bool Finalize(std::wstring& out_path) override {
        const std::scoped_lock api_lock(api_mutex_);
        completed_path_.clear();
        const bool result = Execute(Command::Finalize);
        if (result) {
            out_path = completed_path_;
        } else {
            out_path.clear();
        }
        return result;
    }

    void Abort() override {
        const std::scoped_lock api_lock(api_mutex_);
        static_cast<void>(Execute(Command::Abort));
    }

    [[nodiscard]] bool has_error() const override {
        return has_error_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::wstring last_error() const override {
        const std::scoped_lock lock(error_mutex_);
        return last_error_;
    }

private:
    enum class Command {
        None,
        Open,
        WriteVideoFrame,
        WriteVideoSample,
        WriteAudioPacket,
        Finalize,
        Abort,
        Shutdown,
    };

    bool Execute(Command command) {
        std::unique_lock lock(command_mutex_);
        if (worker_exited_) {
            lock.unlock();
            SetError(L"Dispatching work to the Media Foundation thread", E_UNEXPECTED);
            return false;
        }
        command_ = command;
        command_pending_ = true;
        command_completed_ = false;
        command_changed_.notify_all();
        command_changed_.wait(lock, [this] { return command_completed_ || worker_exited_; });
        return command_completed_ && command_result_;
    }

    void ShutdownWorker() noexcept {
        try {
            const std::scoped_lock api_lock(api_mutex_);
            if (worker_.joinable()) {
                static_cast<void>(Execute(Command::Shutdown));
            }
        } catch (...) {
            // Joining below is still mandatory; the worker owns all COM/MF state.
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void ThreadMain() noexcept {
        const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_initialized = SUCCEEDED(com_result);
        startup_result_ = com_initialized ? ::MFStartup(MF_VERSION, MFSTARTUP_FULL) : com_result;
        const bool mf_started = SUCCEEDED(startup_result_);

        bool shutdown = false;
        while (!shutdown) {
            Command command = Command::None;
            {
                std::unique_lock lock(command_mutex_);
                command_changed_.wait(lock, [this] { return command_pending_; });
                command = command_;
            }

            bool result = false;
            try {
                switch (command) {
                    case Command::Open:
                        result = CoreOpen();
                        break;
                    case Command::WriteVideoFrame:
                        result = requested_frame_ != nullptr && CoreWriteVideoFrame(*requested_frame_);
                        break;
                    case Command::WriteVideoSample:
                        result = requested_video_sample_ != nullptr &&
                                 CoreWriteVideoSample(
                                     requested_video_sample_,
                                     requested_video_timestamp_,
                                     requested_video_duration_);
                        break;
                    case Command::WriteAudioPacket:
                        result = requested_audio_packet_ != nullptr &&
                                 CoreWriteAudioPacket(*requested_audio_packet_);
                        break;
                    case Command::Finalize:
                        result = CoreFinalize();
                        break;
                    case Command::Abort:
                        CoreAbort();
                        result = true;
                        break;
                    case Command::Shutdown:
                        CoreAbort();
                        result = true;
                        shutdown = true;
                        break;
                    case Command::None:
                        SetError(L"Processing an empty Media Foundation command", E_UNEXPECTED);
                        break;
                }
            } catch (const std::bad_alloc&) {
                SetError(L"Allocating Media Foundation writer state", E_OUTOFMEMORY);
            } catch (...) {
                SetError(L"Executing a Media Foundation writer command", E_UNEXPECTED);
            }

            {
                const std::scoped_lock lock(command_mutex_);
                command_result_ = result;
                command_pending_ = false;
                command_completed_ = true;
                command_changed_.notify_all();
            }
        }

        sink_writer_.Reset();
        if (mf_started) {
            static_cast<void>(::MFShutdown());
        }
        if (com_initialized) {
            ::CoUninitialize();
        }
        {
            const std::scoped_lock lock(command_mutex_);
            worker_exited_ = true;
            command_changed_.notify_all();
        }
    }

    bool CoreOpen() {
        if (open_) {
            SetError(L"Opening an already-open Media Foundation writer", MF_E_INVALIDREQUEST);
            return false;
        }
        if (has_error()) {
            return false;
        }
        if (FAILED(startup_result_)) {
            SetError(L"Initializing COM and Media Foundation", startup_result_);
            return false;
        }
        if (!ValidateOpenRequest()) {
            return false;
        }

        ComPtr<IMFAttributes> attributes;
        HRESULT result = ::MFCreateAttributes(&attributes, 4);
        if (SUCCEEDED(result)) {
            result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }
        if (SUCCEEDED(result)) {
            // Native WASAPI formats commonly require float-to-PCM conversion,
            // resampling, or channel conversion before the AAC encoder.
            result = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        }
        if (SUCCEEDED(result)) {
            // Keep the sink writer's documented throttling enabled so its
            // internal encoder pipeline cannot become a whole-recording queue.
            result = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE);
        }
        if (SUCCEEDED(result) && requested_d3d_manager_ != nullptr) {
            result = attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER,
                                            requested_d3d_manager_.Get());
        }

        ComPtr<IMFSinkWriter> writer;
        if (SUCCEEDED(result)) {
            result = ::MFCreateSinkWriterFromURL(requested_path_.c_str(), nullptr, attributes.Get(), &writer);
        }
        if (FAILED(result)) {
            SetError(L"Creating the MP4 sink writer and output file", result);
            return false;
        }

        ComPtr<IMFMediaType> output_type;
        result = ::MFCreateMediaType(&output_type);
        if (SUCCEEDED(result)) {
            result = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AVG_BITRATE, requested_options_.video_bitrate_bps);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        }
        if (SUCCEEDED(result)) {
            result = SetCommonVideoAttributes(output_type.Get());
        }

        DWORD stream_index = 0;
        if (SUCCEEDED(result)) {
            result = writer->AddStream(output_type.Get(), &stream_index);
        }
        if (FAILED(result)) {
            SetError(L"Configuring the H.264 output stream", result);
            return false;
        }

        ComPtr<IMFMediaType> input_type;
        result = ::MFCreateMediaType(&input_type);
        if (SUCCEEDED(result)) {
            result = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_DEFAULT_STRIDE, width_);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_SAMPLE_SIZE, nv12_buffer_size_);
        }
        if (SUCCEEDED(result)) {
            result = SetCommonVideoAttributes(input_type.Get());
        }
        if (SUCCEEDED(result)) {
            result = writer->SetInputMediaType(stream_index, input_type.Get(), nullptr);
        }
        if (FAILED(result)) {
            SetError(L"Configuring NV12 input or locating an H.264 encoder", result);
            return false;
        }

        DWORD audio_stream_index = 0;
        if (requested_audio_format_.has_value() &&
            !ConfigureAudioStream(writer.Get(), audio_stream_index)) {
            return false;
        }

        result = writer->BeginWriting();
        if (FAILED(result)) {
            SetError(L"Starting the H.264/AAC sink writer", result);
            return false;
        }

        // A Direct3D-aware encoder takes the surface; every other one locks it.
        bool reads_surfaces = false;
        if (requested_native_nv12_ && requested_d3d_manager_ != nullptr) {
            ComPtr<IMFTransform> encoder;
            ComPtr<IMFAttributes> encoder_attributes;
            if (SUCCEEDED(writer->GetServiceForStream(stream_index, GUID_NULL,
                                                      IID_PPV_ARGS(&encoder))) &&
                SUCCEEDED(encoder->GetAttributes(&encoder_attributes))) {
                reads_surfaces =
                    ::MFGetAttributeUINT32(encoder_attributes.Get(),
                                           MF_SA_D3D11_AWARE, FALSE) != FALSE;
            }
        }
        encoder_reads_surfaces_.store(reads_surfaces, std::memory_order_release);

        sink_writer_ = std::move(writer);
        // Record the begun sink before copying metadata that may allocate, so
        // exception cleanup also drains its pending native work through Abort.
        open_ = true;
        stream_index_ = stream_index;
        audio_stream_index_ = audio_stream_index;
        output_path_ = requested_path_;
        options_ = requested_options_;
        audio_format_ = requested_audio_format_;
        frames_written_ = 0;
        audio_packets_written_ = 0;
        native_nv12_ = requested_native_nv12_;
        last_frame_end_ = MediaTime::zero();
        last_audio_end_ = MediaTime::zero();
        last_audio_tick_ = MediaTime::zero();
        {
            const std::scoped_lock lock(error_mutex_);
            last_error_.clear();
        }
        return true;
    }

    bool ValidateOpenRequest() {
        if (requested_options_.format != RecordingFormat::Mp4) {
            SetError(L"Opening the MP4 writer with a non-MP4 recording format", E_INVALIDARG);
            return false;
        }
        const bool audio_requested = requested_options_.audio == AudioSource::SystemAudio ||
                                     requested_options_.audio == AudioSource::Microphone ||
                                     requested_options_.audio == AudioSource::Both;
        const bool supported_audio = requested_options_.audio == AudioSource::None ||
                                     audio_requested;
        if (!supported_audio ||
            (audio_requested != requested_audio_format_.has_value())) {
            SetError(L"Validating the requested MP4 audio stream", E_INVALIDARG);
            return false;
        }
        if (requested_options_.audio == AudioSource::None &&
            requested_audio_format_.has_value()) {
            SetError(L"Rejecting an audio format for a video-only MP4", E_INVALIDARG);
            return false;
        }
        if (audio_requested) {
            const AudioFormat& format = *requested_audio_format_;
            const bool supported_sample_type =
                (format.is_float &&
                 (format.bits_per_sample == 32 || format.bits_per_sample == 64)) ||
                (!format.is_float &&
                 (format.bits_per_sample == 8 || format.bits_per_sample == 16 ||
                  format.bits_per_sample == 24 || format.bits_per_sample == 32));
            const uint64_t block_align =
                static_cast<uint64_t>(format.channels) * (format.bits_per_sample / 8u);
            const uint64_t bytes_per_second = block_align * format.sample_rate;
            if (!format.valid() || !supported_sample_type || format.channels > 8 ||
                format.sample_rate < 8'000 || format.sample_rate > 192'000 ||
                block_align == 0 || block_align > (std::numeric_limits<UINT32>::max)() ||
                bytes_per_second > (std::numeric_limits<UINT32>::max)() ||
                (format.channels > 2 && format.channel_mask == 0) ||
                !IsSupportedAacBitrate(requested_options_.audio_bitrate_bps)) {
                SetError(L"Validating the native WASAPI format for AAC conversion",
                         E_INVALIDARG);
                return false;
            }
            aac_sample_rate_ = format.sample_rate == 44'100 ? 44'100 : 48'000;
            aac_channels_ = format.channels == 1 ? 1 : 2;
        } else {
            aac_sample_rate_ = 0;
            aac_channels_ = 0;
        }

        const int64_t width = static_cast<int64_t>(requested_options_.region.right) -
                              requested_options_.region.left;
        const int64_t height = static_cast<int64_t>(requested_options_.region.bottom) -
                               requested_options_.region.top;
        if (width <= 0 || height <= 0 || width > (std::numeric_limits<int>::max)() ||
            height > (std::numeric_limits<int>::max)() || (width & 1) != 0 || (height & 1) != 0) {
            SetError(L"Validating even, positive H.264 frame dimensions", E_INVALIDARG);
            return false;
        }
        if (requested_options_.target_frame_rate == 0 ||
            requested_options_.target_frame_rate > kMaximumFrameRate) {
            SetError(L"Validating the H.264 target frame rate", E_INVALIDARG);
            return false;
        }
        if (requested_options_.video_bitrate_bps < kMinimumVideoBitrate ||
            requested_options_.video_bitrate_bps > kMaximumVideoBitrate) {
            SetError(L"Validating the H.264 target bitrate", E_INVALIDARG);
            return false;
        }
        if (requested_path_.empty() || ::PathIsRelativeW(requested_path_.c_str()) != FALSE ||
            _wcsicmp(::PathFindExtensionW(requested_path_.c_str()), L".mp4") != 0) {
            SetError(L"Validating the absolute MP4 output path", E_INVALIDARG);
            return false;
        }
        if (::GetFileAttributesW(requested_path_.c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetError(L"Protecting an existing MP4 output file",
                     HRESULT_FROM_WIN32(ERROR_FILE_EXISTS));
            return false;
        }
        const std::size_t separator = requested_path_.find_last_of(L"\\/");
        if (separator == std::wstring::npos) {
            SetError(L"Locating the MP4 output directory", E_INVALIDARG);
            return false;
        }
        const std::wstring parent = requested_path_.substr(0, separator);
        const DWORD parent_attributes = ::GetFileAttributesW(parent.c_str());
        if (parent_attributes == INVALID_FILE_ATTRIBUTES ||
            (parent_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            SetError(L"Opening the existing MP4 output directory",
                     HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
            return false;
        }

        const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        const uint64_t nv12_bytes = pixels + pixels / 2;
        if (pixels > (std::numeric_limits<std::size_t>::max)() / sizeof(uint32_t) ||
            nv12_bytes > (std::numeric_limits<DWORD>::max)()) {
            SetError(L"Sizing the bounded NV12 conversion buffer", E_INVALIDARG);
            return false;
        }
        width_ = static_cast<UINT32>(width);
        height_ = static_cast<UINT32>(height);
        nv12_buffer_size_ = static_cast<DWORD>(nv12_bytes);
        return true;
    }

    HRESULT SetCommonVideoAttributes(IMFMediaType* type) const {
        HRESULT result = ::MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width_, height_);
        if (SUCCEEDED(result)) {
            result = ::MFSetAttributeRatio(type, MF_MT_FRAME_RATE, options_for_configuration().target_frame_rate, 1);
        }
        if (SUCCEEDED(result)) {
            result = ::MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        }
        if (SUCCEEDED(result)) {
            result = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        }
        if (SUCCEEDED(result)) {
            result = type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        }
        if (SUCCEEDED(result)) {
            result = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        }
        if (SUCCEEDED(result)) {
            result = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        }
        return result;
    }

    bool ConfigureAudioStream(IMFSinkWriter* writer, DWORD& stream_index) {
        if (writer == nullptr || !requested_audio_format_.has_value()) {
            SetError(L"Configuring an absent AAC stream", E_INVALIDARG);
            return false;
        }
        const AudioFormat& input_format = *requested_audio_format_;
        const UINT32 output_channel_mask =
            aac_channels_ == 1 ? kMonoChannelMask : kStereoChannelMask;

        ComPtr<IMFMediaType> output_type;
        HRESULT result = ::MFCreateMediaType(&output_type);
        if (SUCCEEDED(result)) {
            result = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,
                                            kAacBitsPerSample);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                            aac_sample_rate_);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, aac_channels_);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,
                                            output_channel_mask);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                            requested_options_.audio_bitrate_bps / 8);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        }
        if (SUCCEEDED(result)) {
            result = output_type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,
                                            kAacProfileLevel2);
        }
        if (SUCCEEDED(result)) {
            result = writer->AddStream(output_type.Get(), &stream_index);
        }
        if (FAILED(result)) {
            SetError(L"Configuring the AAC output stream", result);
            return false;
        }

        const UINT32 input_block_align = input_format.block_align();
        const UINT32 input_channel_mask =
            input_format.channel_mask != 0
                ? input_format.channel_mask
                : (input_format.channels == 1 ? kMonoChannelMask : kStereoChannelMask);
        ComPtr<IMFMediaType> input_type;
        result = ::MFCreateMediaType(&input_type);
        if (SUCCEEDED(result)) {
            result = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetGUID(MF_MT_SUBTYPE,
                                         input_format.is_float ? MFAudioFormat_Float
                                                               : MFAudioFormat_PCM);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,
                                           input_format.bits_per_sample);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                           input_format.sample_rate);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                           input_format.channels);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,
                                           input_channel_mask);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                           input_block_align);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                           input_format.bytes_per_second());
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        }
        if (SUCCEEDED(result)) {
            result = input_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        }
        if (SUCCEEDED(result)) {
            result = writer->SetInputMediaType(stream_index, input_type.Get(), nullptr);
        }
        if (FAILED(result)) {
            SetError(L"Configuring native WASAPI input conversion or locating an AAC encoder",
                     result);
            return false;
        }
        return true;
    }

    const RecordingOptions& options_for_configuration() const noexcept {
        return open_ ? options_ : requested_options_;
    }

    bool CoreWriteVideoFrame(const VideoFrame& frame) {
        if (!open_ || sink_writer_ == nullptr || native_nv12_) {
            SetError(L"Writing a frame before opening the MP4 writer", MF_E_INVALIDREQUEST);
            return false;
        }
        if (has_error()) {
            return false;
        }
        const std::size_t expected_pixels = static_cast<std::size_t>(width_) * height_;
        if (frame.image == nullptr || frame.image->width != static_cast<int>(width_) ||
            frame.image->height != static_cast<int>(height_) ||
            frame.image->pixels.size() != expected_pixels || frame.timestamp < MediaTime::zero() ||
            frame.duration <= MediaTime::zero() ||
            frame.timestamp.count() > (std::numeric_limits<int64_t>::max)() - frame.duration.count() ||
            (frames_written_ != 0 && frame.timestamp < last_frame_end_)) {
            SetError(L"Validating BGRA frame dimensions or monotonic timestamps", E_INVALIDARG);
            return false;
        }

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT result = ::MFCreateMemoryBuffer(nv12_buffer_size_, &buffer);
        if (FAILED(result)) {
            SetError(L"Allocating the bounded NV12 conversion buffer", result);
            return false;
        }

        BYTE* bytes = nullptr;
        DWORD maximum_length = 0;
        result = buffer->Lock(&bytes, &maximum_length, nullptr);
        if (FAILED(result) || bytes == nullptr || maximum_length < nv12_buffer_size_) {
            if (SUCCEEDED(result)) {
                static_cast<void>(buffer->Unlock());
                result = E_UNEXPECTED;
            }
            SetError(L"Locking the NV12 conversion buffer", result);
            return false;
        }
        ConvertBgraToNv12(*frame.image, bytes);
        const HRESULT unlock_result = buffer->Unlock();
        if (FAILED(unlock_result)) {
            SetError(L"Unlocking the NV12 conversion buffer", unlock_result);
            return false;
        }
        result = buffer->SetCurrentLength(nv12_buffer_size_);
        if (FAILED(result)) {
            SetError(L"Committing the NV12 conversion buffer length", result);
            return false;
        }

        ComPtr<IMFSample> sample;
        result = ::MFCreateSample(&sample);
        if (SUCCEEDED(result)) {
            result = sample->AddBuffer(buffer.Get());
        }
        if (SUCCEEDED(result)) {
            result = sample->SetSampleTime(frame.timestamp.count());
        }
        if (SUCCEEDED(result)) {
            result = sample->SetSampleDuration(frame.duration.count());
        }
        if (FAILED(result)) {
            SetError(L"Creating the timestamped NV12 Media Foundation sample", result);
            return false;
        }

        result = sink_writer_->WriteSample(stream_index_, sample.Get());
        if (FAILED(result)) {
            SetError(L"Submitting the video sample to the H.264 encoder", result);
            return false;
        }

        last_frame_end_ = frame.timestamp + frame.duration;
        ++frames_written_;
        return true;
    }

    bool CoreWriteVideoSample(IMFSample* source_sample, MediaTime timestamp,
                              MediaTime duration) {
        if (!open_ || sink_writer_ == nullptr || !native_nv12_ ||
            source_sample == nullptr) {
            SetError(L"Writing native NV12 before opening its MP4 stream",
                     MF_E_INVALIDREQUEST);
            return false;
        }
        if (has_error()) {
            return false;
        }
        if (timestamp < MediaTime::zero() || duration <= MediaTime::zero() ||
            timestamp.count() > (std::numeric_limits<int64_t>::max)() -
                                    duration.count() ||
            (frames_written_ != 0 && timestamp < last_frame_end_)) {
            SetError(L"Validating native NV12 timestamps", E_INVALIDARG);
            return false;
        }

        DWORD buffer_count = 0;
        HRESULT result = source_sample->GetBufferCount(&buffer_count);
        if (SUCCEEDED(result) && buffer_count == 0) {
            result = MF_E_BUFFERTOOSMALL;
        }
        // Preserve the decoder's sample identity: IMFTrackedSample releases its
        // pooled GPU surface when the sample (not just its buffers) is released.
        // The sink must retain that sample until asynchronous encoding finishes.
        // This internal API consumes the decoded sample's timing metadata.
        if (SUCCEEDED(result)) {
            result = source_sample->SetSampleTime(timestamp.count());
        }
        if (SUCCEEDED(result)) {
            result = source_sample->SetSampleDuration(duration.count());
        }
        if (FAILED(result)) {
            SetError(L"Referencing the decoded native NV12 sample", result);
            return false;
        }
        result = sink_writer_->WriteSample(stream_index_, source_sample);
        if (FAILED(result)) {
            SetError(L"Submitting native NV12 to the H.264 encoder", result);
            return false;
        }
        last_frame_end_ = timestamp + duration;
        ++frames_written_;
        return true;
    }

    bool CoreWriteAudioPacket(const MediaChunk& packet) {
        if (!open_ || sink_writer_ == nullptr || !audio_format_.has_value()) {
            SetError(L"Writing audio before opening an AAC stream", MF_E_INVALIDREQUEST);
            return false;
        }
        if (has_error()) {
            return false;
        }
        const UINT32 block_align = audio_format_->block_align();
        if (packet.data == nullptr || packet.size == 0 || block_align == 0 ||
            packet.size % block_align != 0 ||
            packet.size > (std::numeric_limits<DWORD>::max)() ||
            packet.timestamp < MediaTime::zero() || packet.duration <= MediaTime::zero() ||
            packet.timestamp.count() >
                (std::numeric_limits<int64_t>::max)() - packet.duration.count() ||
            (audio_packets_written_ != 0 && packet.timestamp < last_audio_end_)) {
            SetError(L"Validating native audio data or monotonic timestamps", E_INVALIDARG);
            return false;
        }

        ComPtr<IMFMediaBuffer> buffer;
        const DWORD byte_count = static_cast<DWORD>(packet.size);
        HRESULT result = ::MFCreateMemoryBuffer(byte_count, &buffer);
        if (FAILED(result)) {
            SetError(L"Allocating the bounded native audio sample buffer", result);
            return false;
        }

        BYTE* destination = nullptr;
        DWORD maximum_length = 0;
        result = buffer->Lock(&destination, &maximum_length, nullptr);
        if (FAILED(result) || destination == nullptr || maximum_length < byte_count) {
            if (SUCCEEDED(result)) {
                static_cast<void>(buffer->Unlock());
                result = E_UNEXPECTED;
            }
            SetError(L"Locking the native audio sample buffer", result);
            return false;
        }
        std::memcpy(destination, packet.data, byte_count);
        const HRESULT unlock_result = buffer->Unlock();
        if (FAILED(unlock_result)) {
            SetError(L"Unlocking the native audio sample buffer", unlock_result);
            return false;
        }
        result = buffer->SetCurrentLength(byte_count);
        if (FAILED(result)) {
            SetError(L"Committing the native audio sample buffer length", result);
            return false;
        }

        ComPtr<IMFSample> sample;
        result = ::MFCreateSample(&sample);
        if (SUCCEEDED(result)) {
            result = sample->AddBuffer(buffer.Get());
        }
        if (SUCCEEDED(result)) {
            result = sample->SetSampleTime(packet.timestamp.count());
        }
        if (SUCCEEDED(result)) {
            result = sample->SetSampleDuration(packet.duration.count());
        }
        if (FAILED(result)) {
            SetError(L"Creating the timestamped native audio sample", result);
            return false;
        }

        result = sink_writer_->WriteSample(audio_stream_index_, sample.Get());
        if (FAILED(result)) {
            SetError(L"Submitting the audio sample to the AAC encoder", result);
            return false;
        }

        last_audio_end_ = packet.timestamp + packet.duration;
        ++audio_packets_written_;
        return true;
    }

    bool CoreFinalize() {
        if (!open_ || sink_writer_ == nullptr) {
            SetError(L"Finalizing an unopened MP4 writer", MF_E_INVALIDREQUEST);
            return false;
        }
        const bool prior_error = has_error();
        const bool has_frames = frames_written_ != 0;
        const bool has_required_audio =
            !audio_format_.has_value() || audio_packets_written_ != 0;
        const HRESULT result = sink_writer_->Finalize();
        sink_writer_.Reset();
        open_ = false;

        if (FAILED(result)) {
            SetError(L"Finalizing the H.264/AAC MP4 sink", result);
            return false;
        }
        if (!has_frames) {
            SetError(L"Finalizing an MP4 with no submitted video frames", MF_E_INVALIDREQUEST);
            return false;
        }
        if (!has_required_audio) {
            SetError(L"Finalizing an A/V MP4 with no submitted audio packets",
                     MF_E_INVALIDREQUEST);
            return false;
        }
        if (prior_error) {
            return false;
        }
        completed_path_ = output_path_;
        return true;
    }

    void CoreAbort() noexcept {
        if (open_ && sink_writer_ != nullptr) {
            // Cancellation still has to complete pending native encoder work
            // before releasing the sink and shutting down Media Foundation.
            // No async callback is configured, so Finalize drains synchronously
            // on this worker. The owner discards the staging file regardless of
            // the result; it is never published as a completed recording.
            static_cast<void>(sink_writer_->Finalize());
        }
        sink_writer_.Reset();
        open_ = false;
        frames_written_ = 0;
        audio_packets_written_ = 0;
        last_audio_tick_ = MediaTime::zero();
        audio_format_.reset();
        aac_sample_rate_ = 0;
        aac_channels_ = 0;
    }

    void SetError(const wchar_t* operation, HRESULT result) {
        const std::wstring message = DescribeHresult(operation, result);
        {
            const std::scoped_lock lock(error_mutex_);
            last_error_ = message;
        }
        has_error_.store(true, std::memory_order_relaxed);
    }

    mutable std::mutex api_mutex_;
    mutable std::mutex error_mutex_;
    std::mutex command_mutex_;
    std::condition_variable command_changed_;
    std::thread worker_;
    Command command_{Command::None};
    bool command_pending_{false};
    bool command_completed_{false};
    bool command_result_{false};
    bool worker_exited_{false};

    std::wstring requested_path_;
    RecordingOptions requested_options_{};
    std::optional<AudioFormat> requested_audio_format_;
    const VideoFrame* requested_frame_{nullptr};
    IMFSample* requested_video_sample_{nullptr};
    MediaTime requested_video_timestamp_{0};
    MediaTime requested_video_duration_{0};
    const MediaChunk* requested_audio_packet_{nullptr};
    MediaTime requested_audio_timeline_position_{0};
    std::wstring completed_path_;

    HRESULT startup_result_{E_PENDING};
    ComPtr<IMFSinkWriter> sink_writer_;
    ComPtr<IMFDXGIDeviceManager> requested_d3d_manager_;
    DWORD stream_index_{0};
    DWORD audio_stream_index_{0};
    UINT32 width_{0};
    UINT32 height_{0};
    DWORD nv12_buffer_size_{0};
    RecordingOptions options_{};
    std::optional<AudioFormat> audio_format_;
    uint32_t aac_sample_rate_{0};
    uint16_t aac_channels_{0};
    std::wstring output_path_;
    MediaTime last_frame_end_{0};
    MediaTime last_audio_end_{0};
    MediaTime last_audio_tick_{0};
    uint64_t frames_written_{0};
    uint64_t audio_packets_written_{0};
    bool requested_native_nv12_{false};
    bool native_nv12_{false};
    bool open_{false};

    std::atomic<bool> has_error_{false};
    std::atomic<bool> encoder_reads_surfaces_{false};
    std::wstring last_error_;
};

}  // namespace

std::unique_ptr<IMediaWriter> CreateMediaFoundationWriter() {
    try {
        return std::make_unique<MediaFoundationWriter>();
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<IMediaFoundationNv12Writer>
CreateMediaFoundationNv12Writer() {
    try {
        return std::make_unique<MediaFoundationWriter>();
    } catch (...) {
        return nullptr;
    }
}

}  // namespace easnap::media
