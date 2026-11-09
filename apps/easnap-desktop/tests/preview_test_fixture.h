// Real preview fixtures built through the production MP4 writer.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "media/media_foundation_writer.h"

namespace easnap::test {

/// A moving H.264 picture, optionally with a real silent stereo AAC track. PCM
/// is generated directly; no recording session or audio device is involved.
/// Time is counted in 48 kHz audio frames, so A/V timestamps share the exact
/// active-time origin and end whatever the frame durations are.
struct PreviewSourceSpec final {
    uint32_t width{640};
    uint32_t height{360};
    /// The nominal rate the writer is opened with.
    uint32_t frame_rate{30};
    uint32_t frame_count{360};
    bool include_audio{false};
    /// Durations of consecutive frames in 48 kHz audio frames, repeated in
    /// turn; empty means 48,000 / frame_rate for every frame.
    std::vector<uint32_t> frame_durations{};
};

inline bool CreatePreviewSource(const std::wstring& path, const PreviewSourceSpec& spec) {
    constexpr int64_t second = 10'000'000;
    constexpr uint32_t sample_rate = 48'000;
    constexpr uint16_t channels = 2;
    const uint32_t width = spec.width;
    const uint32_t height = spec.height;
    if (spec.frame_count == 0 || spec.frame_rate == 0 || width == 0 || height == 0 ||
        (spec.frame_durations.empty() && sample_rate % spec.frame_rate != 0)) {
        return false;
    }
    for (const uint32_t duration : spec.frame_durations) {
        if (duration == 0) return false;
    }

    auto writer = media::CreateMediaFoundationWriter();
    if (!writer) {
        std::fprintf(stderr, "Audio preview fixture: writer creation failed\n");
        return false;
    }
    const auto fail = [&](const wchar_t* stage) {
        std::fwprintf(stderr, L"Audio preview fixture %ls failed: %ls\n",
                      stage, writer->last_error().c_str());
        writer->Abort();
        return false;
    };

    media::AudioFormat audio_format;
    audio_format.sample_rate = sample_rate;
    audio_format.channels = channels;
    audio_format.bits_per_sample = 32;
    audio_format.is_float = true;
    audio_format.channel_mask = 0x3;
    media::RecordingOptions options;
    options.region = RECT{0, 0, static_cast<LONG>(width),
                          static_cast<LONG>(height)};
    options.format = media::RecordingFormat::Mp4;
    options.audio = spec.include_audio ? media::AudioSource::SystemAudio : media::AudioSource::None;
    options.target_frame_rate = spec.frame_rate;
    options.video_bitrate_bps = 4'000'000;
    options.audio_bitrate_bps = 192'000;
    if (!writer->Open(path, options, spec.include_audio ? &audio_format : nullptr)) return fail(L"open");

    auto image = std::make_shared<platform::BgraImage>();
    image->width = static_cast<int>(width);
    image->height = static_cast<int>(height);
    image->pixels.resize(static_cast<std::size_t>(width) * height);
    std::vector<float> silence;
    int64_t first_audio_frame = 0;
    for (uint32_t index = 0; index < spec.frame_count; ++index) {
        for (uint32_t y = 0; y < height; ++y) {
            auto* row = image->row(static_cast<int>(y));
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t red = (x + index * 3u) & 0xffu;
                const uint32_t green = (y + index * 5u) & 0xffu;
                const uint32_t blue = (x + y + index * 7u) & 0xffu;
                row[x] = 0xff000000u | (red << 16u) | (green << 8u) | blue;
            }
        }
        const uint32_t audio_frames_per_video_frame =
            spec.frame_durations.empty()
                ? sample_rate / spec.frame_rate
                : spec.frame_durations[index % spec.frame_durations.size()];
        const int64_t next_audio_frame = first_audio_frame + audio_frames_per_video_frame;
        const media::MediaTime start{first_audio_frame * second / sample_rate};
        const media::MediaTime end{next_audio_frame * second / sample_rate};
        // WriteVideoFrame synchronously consumes this image on its MTA worker,
        // so one frame allocation can be reused after each call returns.
        if (!writer->WriteVideoFrame({image, start, end - start})) {
            return fail(L"video write");
        }
        if (spec.include_audio) {
            silence.assign(static_cast<std::size_t>(audio_frames_per_video_frame) * channels, 0.0f);
            const media::MediaChunk packet{
                reinterpret_cast<const uint8_t*>(silence.data()),
                silence.size() * sizeof(float), start, end - start, false, false};
            if (!writer->WriteAudioPacket(packet)) return fail(L"audio write");
        }
        first_audio_frame = next_audio_frame;
    }

    std::wstring finalized;
    if (!writer->Finalize(finalized)) return fail(L"finalize");
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return finalized == path && attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Moving 640x360 H.264 at 30 fps (twelve seconds by default), optionally with a
// real silent stereo AAC track.
inline bool CreatePreviewSource(const std::wstring& path, bool include_audio,
                                uint32_t duration_seconds = 12) {
    if (duration_seconds == 0 || duration_seconds > 60) return false;
    PreviewSourceSpec spec;
    spec.frame_count = duration_seconds * spec.frame_rate;
    spec.include_audio = include_audio;
    return CreatePreviewSource(path, spec);
}

inline bool CreateAudioPreviewSource(const std::wstring& path) {
    return CreatePreviewSource(path, true);
}

inline bool CreateVideoPreviewSource(const std::wstring& path,
                                     uint32_t duration_seconds = 12) {
    return CreatePreviewSource(path, false, duration_seconds);
}

}  // namespace easnap::test
