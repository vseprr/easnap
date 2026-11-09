// Test media written with the Media Foundation sink writer: the containers
// and codecs of the support table (ADR-040) that the production writer does
// not make, such as WMV with WMA audio or an MP4 that holds only AAC.
#pragma once

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "platform/platform_utils.h"

namespace easnap::test {

enum class SinkWriterOutcome { Written,
                               EncoderUnavailable,
                               Failed };

struct SinkWriterSpec final {
    /// GUID_NULL writes no video stream.
    GUID video_subtype{GUID_NULL};
    /// AAC or WMA (MFAudioFormat_WMAudioV8); GUID_NULL writes no audio stream.
    GUID audio_subtype{GUID_NULL};
    uint32_t width{160};
    uint32_t height{120};
    uint32_t frame_rate{30};
    /// The file lasts this many frames, with or without a video stream.
    uint32_t frame_count{30};
    /// Audio covers the time of the first this-many frames: fewer than
    /// `frame_count` makes audio that ends before the video.
    uint32_t audio_frame_count{UINT32_MAX};
    /// Paints frame `index`; a flat grey that brightens over time when empty.
    std::function<void(uint32_t index, platform::BgraImage& image)> paint{};
};

namespace sink_writer_detail {

using Microsoft::WRL::ComPtr;

constexpr int64_t kSecond = 10'000'000;
constexpr uint32_t kSampleRate = 44'100;
constexpr uint32_t kChannels = 2;

inline bool EncoderExists(const GUID& category, const GUID& major, const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO output{major, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(::MFTEnumEx(category, MFT_ENUM_FLAG_ALL, nullptr, &output, &activates, &count))) {
        return false;
    }
    for (UINT32 index = 0; index < count; ++index) {
        activates[index]->Release();
    }
    ::CoTaskMemFree(activates);
    return count != 0;
}

/// The first stereo 44.1 kHz type the WMA encoder offers.
inline ComPtr<IMFMediaType> WmaOutputType(const GUID& subtype) {
    ComPtr<IMFCollection> types;
    if (FAILED(::MFTranscodeGetAudioOutputAvailableTypes(subtype, MFT_ENUM_FLAG_ALL, nullptr,
                                                         &types))) {
        return nullptr;
    }
    DWORD count = 0;
    static_cast<void>(types->GetElementCount(&count));
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IUnknown> element;
        ComPtr<IMFMediaType> type;
        if (FAILED(types->GetElement(index, &element)) || FAILED(element.As(&type))) {
            continue;
        }
        if (::MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) == kSampleRate &&
            ::MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0) == kChannels) {
            return type;
        }
    }
    return nullptr;
}

inline ComPtr<IMFMediaType> AudioOutputType(const GUID& subtype) {
    if (subtype != MFAudioFormat_AAC) {
        return WmaOutputType(subtype);
    }
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(&type)) ||
        FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
        FAILED(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC)) ||
        FAILED(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
        FAILED(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate)) ||
        FAILED(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels)) ||
        FAILED(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24'000))) {
        return nullptr;
    }
    return type;
}

inline bool SetVideoType(IMFMediaType* type, const GUID& subtype, const SinkWriterSpec& spec) {
    return SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
           SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, subtype)) &&
           SUCCEEDED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) &&
           SUCCEEDED(::MFSetAttributeSize(type, MF_MT_FRAME_SIZE, spec.width, spec.height)) &&
           SUCCEEDED(::MFSetAttributeRatio(type, MF_MT_FRAME_RATE, spec.frame_rate, 1)) &&
           SUCCEEDED(::MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
}

inline bool WriteBuffer(IMFSinkWriter* writer, DWORD stream, const void* data, DWORD bytes,
                        int64_t start, int64_t end) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE* destination = nullptr;
    if (FAILED(::MFCreateMemoryBuffer(bytes, &buffer)) ||
        FAILED(buffer->Lock(&destination, nullptr, nullptr))) {
        return false;
    }
    std::memcpy(destination, data, bytes);
    return SUCCEEDED(buffer->Unlock()) && SUCCEEDED(buffer->SetCurrentLength(bytes)) &&
           SUCCEEDED(::MFCreateSample(&sample)) && SUCCEEDED(sample->AddBuffer(buffer.Get())) &&
           SUCCEEDED(sample->SetSampleTime(start)) &&
           SUCCEEDED(sample->SetSampleDuration(end - start)) &&
           SUCCEEDED(writer->WriteSample(stream, sample.Get()));
}

}  // namespace sink_writer_detail

/// Writes `path` with the encoders Windows offers. The container follows the
/// codecs: ASF for WMV/VC-1 or WMA, MPEG-4 otherwise. Needs COM and Media
/// Foundation started by the caller.
inline SinkWriterOutcome WriteWithSinkWriter(const std::wstring& path, const SinkWriterSpec& spec) {
    using namespace sink_writer_detail;
    const bool has_video = spec.video_subtype != GUID_NULL;
    const bool has_audio = spec.audio_subtype != GUID_NULL;
    if ((!has_video && !has_audio) || spec.frame_rate == 0 || spec.frame_count == 0) {
        return SinkWriterOutcome::Failed;
    }
    if ((has_video && !EncoderExists(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video,
                                     spec.video_subtype)) ||
        (has_audio && !EncoderExists(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                                     spec.audio_subtype))) {
        return SinkWriterOutcome::EncoderUnavailable;
    }
    const bool asf = spec.video_subtype == MFVideoFormat_WMV3 ||
                     spec.video_subtype == MFVideoFormat_WVC1 ||
                     (has_audio && spec.audio_subtype != MFAudioFormat_AAC);
    const auto fail = [&](const char* stage, HRESULT result) {
        std::fprintf(stderr, "Sink writer fixture: %s failed (0x%08lX)\n", stage,
                     static_cast<unsigned long>(result));
        return SinkWriterOutcome::Failed;
    };

    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(result)) {
        result = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                                     asf ? MFTranscodeContainerType_ASF
                                         : MFTranscodeContainerType_MPEG4);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    }
    if (SUCCEEDED(result)) {
        // Each stream is written on its own; audio that ends early must not
        // hold the video back.
        result = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    }
    ComPtr<IMFSinkWriter> writer;
    if (SUCCEEDED(result)) {
        result = ::MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer);
    }
    if (FAILED(result)) {
        return fail("creation", result);
    }

    DWORD video_stream = 0;
    if (has_video) {
        ComPtr<IMFMediaType> output;
        ComPtr<IMFMediaType> input;
        result = ::MFCreateMediaType(&output);
        if (SUCCEEDED(result)) {
            result = SetVideoType(output.Get(), spec.video_subtype, spec) &&
                             SUCCEEDED(output->SetUINT32(MF_MT_AVG_BITRATE, 2'000'000))
                         ? S_OK
                         : E_FAIL;
        }
        if (SUCCEEDED(result)) {
            result = writer->AddStream(output.Get(), &video_stream);
        }
        if (SUCCEEDED(result)) {
            result = ::MFCreateMediaType(&input);
        }
        if (SUCCEEDED(result)) {
            result = SetVideoType(input.Get(), MFVideoFormat_RGB32, spec) &&
                             SUCCEEDED(input->SetUINT32(MF_MT_DEFAULT_STRIDE, spec.width * 4u))
                         ? S_OK
                         : E_FAIL;
        }
        if (SUCCEEDED(result)) {
            result = writer->SetInputMediaType(video_stream, input.Get(), nullptr);
        }
        if (FAILED(result)) {
            return fail("video stream", result);
        }
    }

    DWORD audio_stream = 0;
    if (has_audio) {
        const ComPtr<IMFMediaType> output = AudioOutputType(spec.audio_subtype);
        if (output == nullptr) {
            return SinkWriterOutcome::EncoderUnavailable;
        }
        ComPtr<IMFMediaType> input;
        result = writer->AddStream(output.Get(), &audio_stream);
        if (SUCCEEDED(result)) {
            result = ::MFCreateMediaType(&input);
        }
        if (SUCCEEDED(result) &&
            (FAILED(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
             FAILED(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) ||
             FAILED(input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
             FAILED(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate)) ||
             FAILED(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels)) ||
             FAILED(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2u)) ||
             FAILED(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                     kSampleRate * kChannels * 2u)))) {
            result = E_FAIL;
        }
        if (SUCCEEDED(result)) {
            result = writer->SetInputMediaType(audio_stream, input.Get(), nullptr);
        }
        if (FAILED(result)) {
            return fail("audio stream", result);
        }
    }

    result = writer->BeginWriting();
    if (FAILED(result)) {
        return fail("begin", result);
    }
    platform::BgraImage image;
    image.width = static_cast<int>(spec.width);
    image.height = static_cast<int>(spec.height);
    image.pixels.resize(static_cast<std::size_t>(spec.width) * spec.height);
    std::vector<int16_t> silence;
    uint64_t audio_frames_written = 0;
    for (uint32_t index = 0; index < spec.frame_count; ++index) {
        const int64_t start = static_cast<int64_t>(index) * kSecond / spec.frame_rate;
        const int64_t end = static_cast<int64_t>(index + 1u) * kSecond / spec.frame_rate;
        if (has_video) {
            if (spec.paint) {
                spec.paint(index, image);
            } else {
                const uint32_t level = (index * 8u) & 0xFFu;
                for (uint32_t& pixel : image.pixels) {
                    pixel = 0xFF000000u | (level << 16u) | (level << 8u) | level;
                }
            }
            if (!WriteBuffer(writer.Get(), video_stream, image.pixels.data(),
                             static_cast<DWORD>(image.pixels.size() * sizeof(uint32_t)), start,
                             end)) {
                return fail("video sample", E_FAIL);
            }
        }
        if (has_audio && index < spec.audio_frame_count) {
            const uint64_t audio_end = static_cast<uint64_t>(index + 1u) * kSampleRate / spec.frame_rate;
            const uint64_t frames = audio_end - audio_frames_written;
            silence.assign(static_cast<std::size_t>(frames) * kChannels, 0);
            const int64_t audio_start_time =
                static_cast<int64_t>(audio_frames_written * kSecond / kSampleRate);
            const int64_t audio_end_time = static_cast<int64_t>(audio_end * kSecond / kSampleRate);
            if (frames != 0 &&
                !WriteBuffer(writer.Get(), audio_stream, silence.data(),
                             static_cast<DWORD>(silence.size() * sizeof(int16_t)),
                             audio_start_time, audio_end_time)) {
                return fail("audio sample", E_FAIL);
            }
            audio_frames_written = audio_end;
        }
    }
    result = writer->Finalize();
    return SUCCEEDED(result) ? SinkWriterOutcome::Written : fail("finalize", result);
}

}  // namespace easnap::test
