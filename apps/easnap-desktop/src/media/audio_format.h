// Easnap - permanent audio format descriptor and owned audio packet.
//
// These types are the audio equivalent of VideoFrame: they carry owned data with
// full format metadata through the bounded recording pipeline. No WASAPI pointer
// may escape the capture callback scope, so every packet copies its payload into
// application-owned storage before IAudioCaptureClient::ReleaseBuffer.
#pragma once

#include <cstdint>
#include <vector>

#include "media/media_types.h"

namespace easnap::media {

/// Describes the native PCM format acquired from WASAPI. The format is
/// discovered at capture start through IAudioClient::GetMixFormat and preserved
/// exactly — no premature resampling is applied in this layer.
struct AudioFormat {
    uint32_t sample_rate{0};       ///< e.g. 44100, 48000
    uint16_t channels{0};          ///< e.g. 1, 2
    uint16_t bits_per_sample{0};   ///< e.g. 16, 32
    bool is_float{false};          ///< IEEE 754 float vs integer PCM
    uint32_t channel_mask{0};      ///< SPEAKER_* layout bitmask from WAVEFORMATEXTENSIBLE

    /// Bytes per multi-channel audio frame (one sample per channel).
    [[nodiscard]] uint32_t block_align() const noexcept {
        return static_cast<uint32_t>(channels) * (bits_per_sample / 8u);
    }

    /// Byte rate at the native sample rate.
    [[nodiscard]] uint32_t bytes_per_second() const noexcept {
        return sample_rate * block_align();
    }

    /// Minimum validity: nonzero rate, channels, and bits.
    [[nodiscard]] bool valid() const noexcept {
        return sample_rate > 0 && channels > 0 && bits_per_sample > 0 &&
               (bits_per_sample % 8) == 0;
    }
};

/// Discontinuity / status flags for audio packets.
enum class AudioPacketFlags : uint32_t {
    None          = 0,
    Discontinuity = 1u << 0,  ///< Gap detected (device glitch, queue overrun, etc.)
    Silent        = 1u << 1,  ///< WASAPI reported AUDCLNT_BUFFERFLAGS_SILENT
    DeviceChanged = 1u << 2,  ///< Endpoint invalidation detected during capture
    TimestampError = 1u << 3, ///< WASAPI could not provide a reliable position
};

/// Clock source used to resolve AudioPacket::timestamp. High-precision sources
/// are suitable for drift/discontinuity checks; CaptureQpcFallback is a
/// bounded last resort that remains suitable for placing a packet in active
/// time without pretending callback scheduling is sample-accurate.
enum class AudioTimestampSource : uint8_t {
    WasapiQpc,
    AudioClockCorrelation,
    CaptureQpcFallback,
};

[[nodiscard]] inline AudioPacketFlags operator|(AudioPacketFlags a, AudioPacketFlags b) noexcept {
    return static_cast<AudioPacketFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

[[nodiscard]] inline AudioPacketFlags operator&(AudioPacketFlags a, AudioPacketFlags b) noexcept {
    return static_cast<AudioPacketFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline AudioPacketFlags& operator|=(AudioPacketFlags& a, AudioPacketFlags b) noexcept {
    a = a | b;
    return a;
}

[[nodiscard]] inline bool HasFlag(AudioPacketFlags flags, AudioPacketFlags test) noexcept {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

/// One owned audio packet. Data is copied from the WASAPI capture buffer before
/// ReleaseBuffer and is valid for the packet's entire lifetime. No WASAPI
/// pointer escapes the capture callback scope.
struct AudioPacket {
    std::vector<uint8_t> data;           ///< Owned PCM payload
    uint32_t frame_count{0};             ///< Audio frames (samples / channels)
    AudioFormat format;                  ///< Source format at capture time
    MediaTime timestamp{0};              ///< QPC-relative time of the first frame (100 ns units)
    uint64_t device_position{0};          ///< Position of the first frame in the device stream
    AudioPacketFlags flags{AudioPacketFlags::None};
    AudioTimestampSource timestamp_source{AudioTimestampSource::WasapiQpc};

    /// Payload size consistent with frame count and format.
    [[nodiscard]] bool consistent() const noexcept {
        return format.valid() &&
               data.size() == static_cast<size_t>(frame_count) * format.block_align();
    }
};

}  // namespace easnap::media
