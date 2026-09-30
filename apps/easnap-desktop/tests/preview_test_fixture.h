// Real preview fixtures built through the production MP4 writer.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "media/media_foundation_writer.h"

namespace easnap::test {

// Moving 640x360 H.264 at 30 fps (twelve seconds by default), optionally with a real silent
// stereo AAC track. PCM is generated directly; no recording session or audio
// device is involved. A/V timestamps share the exact active-time origin and end.
inline bool CreatePreviewSource(const std::wstring& path, bool include_audio,
                                uint32_t duration_seconds = 12) {
    constexpr int64_t second = 10'000'000;
    constexpr uint32_t width = 640;
    constexpr uint32_t height = 360;
    constexpr uint32_t frame_rate = 30;
    constexpr uint32_t sample_rate = 48'000;
    constexpr uint16_t channels = 2;
    if (duration_seconds == 0 || duration_seconds > 60) return false;
    const uint32_t frame_count = duration_seconds * frame_rate;
    constexpr uint32_t audio_frames_per_video_frame = sample_rate / frame_rate;
    static_assert(sample_rate % frame_rate == 0);

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
    options.audio = include_audio ? media::AudioSource::SystemAudio : media::AudioSource::None;
    options.target_frame_rate = frame_rate;
    options.video_bitrate_bps = 4'000'000;
    options.audio_bitrate_bps = 192'000;
    if (!writer->Open(path, options, include_audio ? &audio_format : nullptr)) return fail(L"open");

    auto image = std::make_shared<platform::BgraImage>();
    image->width = static_cast<int>(width);
    image->height = static_cast<int>(height);
    image->pixels.resize(static_cast<std::size_t>(width) * height);
    const std::vector<float> silence(
        static_cast<std::size_t>(audio_frames_per_video_frame) * channels,
        0.0f);
    for (uint32_t index = 0; index < frame_count; ++index) {
        for (uint32_t y = 0; y < height; ++y) {
            auto* row = image->row(static_cast<int>(y));
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t red = (x + index * 3u) & 0xffu;
                const uint32_t green = (y + index * 5u) & 0xffu;
                const uint32_t blue = (x + y + index * 7u) & 0xffu;
                row[x] = 0xff000000u | (red << 16u) | (green << 8u) | blue;
            }
        }
        const media::MediaTime start{
            static_cast<int64_t>(index) * second / frame_rate};
        const media::MediaTime end{
            static_cast<int64_t>(index + 1u) * second / frame_rate};
        // WriteVideoFrame synchronously consumes this image on its MTA worker,
        // so one frame allocation can be reused after each call returns.
        if (!writer->WriteVideoFrame({image, start, end - start})) {
            return fail(L"video write");
        }
        const int64_t first_audio_frame =
            static_cast<int64_t>(index) * audio_frames_per_video_frame;
        const media::MediaTime audio_start{
            first_audio_frame * second / sample_rate};
        const media::MediaTime audio_end{
            (first_audio_frame + audio_frames_per_video_frame) * second /
            sample_rate};
        const media::MediaChunk packet{
            reinterpret_cast<const uint8_t*>(silence.data()),
            silence.size() * sizeof(float), audio_start,
            audio_end - audio_start, false, false};
        if (include_audio && !writer->WriteAudioPacket(packet)) return fail(L"audio write");
    }

    std::wstring finalized;
    if (!writer->Finalize(finalized)) return fail(L"finalize");
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return finalized == path && attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool CreateAudioPreviewSource(const std::wstring& path) {
    return CreatePreviewSource(path, true);
}

inline bool CreateVideoPreviewSource(const std::wstring& path,
                                     uint32_t duration_seconds = 12) {
    return CreatePreviewSource(path, false, duration_seconds);
}

}  // namespace easnap::test
