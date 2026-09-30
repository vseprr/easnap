// Easnap - Windows Audio Resampler DSP wrapper used by dual-source capture.

#include "media/audio_resampler.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace easnap::media::detail {
namespace {

constexpr DWORD kOutputBufferBytes = 64 * 1024;
constexpr int64_t kTicksPerSecond = 10'000'000;

[[nodiscard]] uint32_t EffectiveChannelMask(const AudioFormat& format) noexcept {
    if (format.channel_mask != 0) {
        return format.channel_mask;
    }
    return format.channels == 1 ? 0x4u : (format.channels == 2 ? 0x3u : 0u);
}

[[nodiscard]] HRESULT CreateAudioType(const AudioFormat& format,
                                      IMFMediaType** result) noexcept {
    if (result == nullptr) {
        return E_POINTER;
    }
    *result = nullptr;
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    HRESULT hr = ::MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) {
        hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetGUID(MF_MT_SUBTYPE,
                           format.is_float ? MFAudioFormat_Float
                                           : MFAudioFormat_PCM);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channels);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                             format.sample_rate);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,
                             format.bits_per_sample);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                             format.block_align());
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                             format.bytes_per_second());
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,
                             EffectiveChannelMask(format));
    }
    if (FAILED(hr)) {
        return hr;
    }
    *result = type.Detach();
    return S_OK;
}

[[nodiscard]] MediaTime FramesToTime(uint64_t frames,
                                     uint32_t sample_rate) noexcept {
    const uint64_t seconds = frames / sample_rate;
    const uint64_t remainder = frames % sample_rate;
    return MediaTime{static_cast<int64_t>(
        seconds * static_cast<uint64_t>(kTicksPerSecond) +
        (remainder * static_cast<uint64_t>(kTicksPerSecond)) / sample_rate)};
}

}  // namespace

AudioResampler::~AudioResampler() {
    transform_.Reset();
    if (mf_started_) {
        ::MFShutdown();
    }
}

bool AudioResampler::Initialize(const AudioFormat& input,
                                const AudioFormat& output) noexcept {
    transform_.Reset();
    input_format_ = {};
    output_format_ = {};
    last_hresult_ = 0;
    const bool valid_output = output.valid() && output.is_float &&
                              output.bits_per_sample == 32 &&
                              output.channels == 2 &&
                              output.channel_mask == 0x3;
    if (!input.valid() || !valid_output || input.channels > 8 ||
        (input.channels > 2 && input.channel_mask == 0)) {
        last_hresult_ = E_INVALIDARG;
        return false;
    }
    if (!mf_started_) {
        const HRESULT startup = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(startup)) {
            last_hresult_ = startup;
            return false;
        }
        mf_started_ = true;
    }

    HRESULT hr = ::CoCreateInstance(CLSID_CResamplerMediaObject, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&transform_));
    Microsoft::WRL::ComPtr<IMFMediaType> input_type;
    Microsoft::WRL::ComPtr<IMFMediaType> output_type;
    if (SUCCEEDED(hr)) {
        hr = CreateAudioType(input, &input_type);
    }
    if (SUCCEEDED(hr)) {
        hr = CreateAudioType(output, &output_type);
    }
    if (SUCCEEDED(hr)) {
        hr = transform_->SetInputType(0, input_type.Get(), 0);
    }
    if (SUCCEEDED(hr)) {
        hr = transform_->SetOutputType(0, output_type.Get(), 0);
    }
    if (SUCCEEDED(hr)) {
        Microsoft::WRL::ComPtr<IWMResamplerProps> properties;
        if (SUCCEEDED(transform_.As(&properties))) {
            hr = properties->SetHalfFilterLength(60);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (FAILED(hr)) {
        last_hresult_ = hr;
        transform_.Reset();
        return false;
    }
    input_format_ = input;
    output_format_ = output;
    has_next_output_timestamp_ = false;
    discontinuity_pending_ = false;
    return true;
}

bool AudioResampler::Convert(const AudioPacket& input,
                             std::vector<AudioPacket>& output) noexcept {
    output.clear();
    if (!transform_ || !input.consistent() || input.frame_count == 0 ||
        input.data.size() > (std::numeric_limits<DWORD>::max)() ||
        input.format.sample_rate != input_format_.sample_rate ||
        input.format.channels != input_format_.channels ||
        input.format.bits_per_sample != input_format_.bits_per_sample ||
        input.format.is_float != input_format_.is_float ||
        input.format.channel_mask != input_format_.channel_mask) {
        last_hresult_ = E_INVALIDARG;
        return false;
    }
    if (HasFlag(input.flags, AudioPacketFlags::Discontinuity) ||
        HasFlag(input.flags, AudioPacketFlags::DeviceChanged)) {
        Flush();
        discontinuity_pending_ = true;
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = ::MFCreateSample(&sample);
    if (SUCCEEDED(hr)) {
        hr = ::MFCreateMemoryBuffer(static_cast<DWORD>(input.data.size()),
                                    &buffer);
    }
    BYTE* destination = nullptr;
    if (SUCCEEDED(hr)) {
        hr = buffer->Lock(&destination, nullptr, nullptr);
    }
    if (SUCCEEDED(hr)) {
        std::memcpy(destination, input.data.data(), input.data.size());
        buffer->Unlock();
        destination = nullptr;
        hr = buffer->SetCurrentLength(static_cast<DWORD>(input.data.size()));
    }
    if (SUCCEEDED(hr)) {
        hr = sample->AddBuffer(buffer.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = sample->SetSampleTime(input.timestamp.count());
    }
    if (SUCCEEDED(hr)) {
        hr = sample->SetSampleDuration(
            FramesToTime(input.frame_count, input_format_.sample_rate).count());
    }
    if (SUCCEEDED(hr)) {
        hr = transform_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        if (destination != nullptr) {
            buffer->Unlock();
        }
        last_hresult_ = hr;
        return false;
    }
    if (!has_next_output_timestamp_) {
        next_output_timestamp_ = input.timestamp;
        has_next_output_timestamp_ = true;
    }
    output_timestamp_source_ = input.timestamp_source;
    return Pull(output, false);
}

bool AudioResampler::Pull(std::vector<AudioPacket>& output,
                          bool draining) noexcept {
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        MFT_OUTPUT_STREAM_INFO stream_info{};
        HRESULT hr = transform_->GetOutputStreamInfo(0, &stream_info);
        if (FAILED(hr)) {
            last_hresult_ = hr;
            return false;
        }

        Microsoft::WRL::ComPtr<IMFSample> supplied_sample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> supplied_buffer;
        MFT_OUTPUT_DATA_BUFFER output_buffer{};
        output_buffer.dwStreamID = 0;
        if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            hr = ::MFCreateSample(&supplied_sample);
            if (SUCCEEDED(hr)) {
                hr = ::MFCreateMemoryBuffer(
                    (std::max)(kOutputBufferBytes, stream_info.cbSize),
                    &supplied_buffer);
            }
            if (SUCCEEDED(hr)) {
                hr = supplied_sample->AddBuffer(supplied_buffer.Get());
            }
            if (FAILED(hr)) {
                last_hresult_ = hr;
                return false;
            }
            output_buffer.pSample = supplied_sample.Get();
        }

        DWORD status = 0;
        hr = transform_->ProcessOutput(0, 1, &output_buffer, &status);
        if (output_buffer.pEvents != nullptr) {
            output_buffer.pEvents->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (FAILED(hr) || output_buffer.pSample == nullptr) {
            last_hresult_ = FAILED(hr) ? hr : E_FAIL;
            return false;
        }

        Microsoft::WRL::ComPtr<IMFSample> produced;
        produced.Attach(output_buffer.pSample);
        if (supplied_sample.Get() == output_buffer.pSample) {
            supplied_sample.Detach();
        }
        Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
        hr = produced->ConvertToContiguousBuffer(&contiguous);
        DWORD length = 0;
        if (SUCCEEDED(hr)) {
            hr = contiguous->GetCurrentLength(&length);
        }
        if (FAILED(hr) || length == 0 ||
            length % output_format_.block_align() != 0) {
            last_hresult_ = FAILED(hr) ? hr : E_FAIL;
            return false;
        }
        AudioPacket packet;
        packet.format = output_format_;
        packet.frame_count = length / output_format_.block_align();
        packet.data.resize(length);
        BYTE* data = nullptr;
        if (SUCCEEDED(hr)) {
            hr = contiguous->Lock(&data, nullptr, nullptr);
        }
        if (SUCCEEDED(hr)) {
            std::memcpy(packet.data.data(), data, length);
            contiguous->Unlock();
            data = nullptr;
        }
        LONGLONG sample_time = 0;
        if (FAILED(produced->GetSampleTime(&sample_time)) || sample_time < 0) {
            sample_time = next_output_timestamp_.count();
        }
        packet.timestamp = MediaTime{sample_time};
        packet.device_position = static_cast<uint64_t>(
            (static_cast<uint64_t>(packet.timestamp.count()) *
             output_format_.sample_rate) /
            kTicksPerSecond);
        packet.timestamp_source = output_timestamp_source_;
        packet.flags = discontinuity_pending_ ? AudioPacketFlags::Discontinuity
                                              : AudioPacketFlags::None;
        discontinuity_pending_ = false;
        const MediaTime duration =
            FramesToTime(packet.frame_count, output_format_.sample_rate);
        next_output_timestamp_ = packet.timestamp + duration;
        output.push_back(std::move(packet));
        if (!draining && output.size() >= 8) {
            return true;
        }
    }
    last_hresult_ = MF_E_TRANSFORM_STREAM_CHANGE;
    return false;
}

bool AudioResampler::Drain(std::vector<AudioPacket>& output) noexcept {
    output.clear();
    if (!transform_) {
        last_hresult_ = E_UNEXPECTED;
        return false;
    }
    const HRESULT hr =
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        last_hresult_ = hr;
        return false;
    }
    const bool pulled = Pull(output, true);
    Flush();
    return pulled;
}

void AudioResampler::Flush() noexcept {
    if (!transform_) {
        return;
    }
    static_cast<void>(
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
    static_cast<void>(
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    has_next_output_timestamp_ = false;
}

}  // namespace easnap::media::detail
