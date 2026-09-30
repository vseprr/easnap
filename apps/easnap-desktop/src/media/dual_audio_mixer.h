// Easnap - bounded, clock-aligned two-source PCM mixer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "media/audio_format.h"

namespace easnap::media::detail {

enum class DualAudioInput : uint8_t { SystemAudio, Microphone };

enum class DualAudioMixerPushResult : uint8_t {
    Success,            // All frames placed into mixer buffer
    LateDiscarded,      // Packet ended at or before output position; dropped as obsolete late data
    TruncatedOverflow,  // Partial frames placed up to capacity; tail truncated (capacity failure)
    CapacityExceeded,   // Start frame at or beyond capacity; no frames placed (capacity failure)
    InvalidPacket,      // Malformed packet (inconsistent, invalid format, negative time)
};

struct DualAudioMixerDiagnostics final {
    uint64_t packets_accepted{0};
    uint64_t packets_dropped_late{0};
    uint64_t frames_dropped_late{0};
    uint64_t capacity_failures{0};
    uint64_t frames_dropped_overflow{0};
    uint64_t invalid_packets{0};
    std::size_t peak_buffered_frames{0};
};

/// A fixed-capacity mixer for two streams already converted to one PCM format.
///
/// Within an established run, sample-count continuity is preserved so normal
/// packet scheduling and QPC jitter cannot create audible micro-gaps or pops.
/// Discontinuities and silence gaps exceeding tolerance re-anchor to QPC.
/// Late packets arriving after the look-behind output frontier are discarded
/// as obsolete without failing the mixer. Missing source frames are silence.
/// Each input has -6 dB headroom and the defensive clamp guarantees finite
/// [-1, 1] output.
class DualAudioMixer final {
public:
    static constexpr uint32_t kDefaultChunkFrames = 480;       // 10 ms at 48 kHz
    static constexpr uint32_t kDefaultCapacityFrames = 24'000; // 500 ms
    static constexpr uint64_t kContinuityToleranceFrames = 240; // 5 ms at 48 kHz

    explicit DualAudioMixer(
        AudioFormat format,
        uint32_t capacity_frames = kDefaultCapacityFrames,
        uint32_t chunk_frames = kDefaultChunkFrames);

    [[nodiscard]] bool valid() const noexcept;
    void Reset() noexcept;
    [[nodiscard]] DualAudioMixerPushResult Push(
        DualAudioInput input, const AudioPacket& packet) noexcept;
    /// Emits one fixed-size packet if its end is at or before watermark.
    [[nodiscard]] bool PopUntil(MediaTime watermark,
                                AudioPacket& packet) noexcept;

    [[nodiscard]] MediaTime latest_buffered_end() const noexcept;
    [[nodiscard]] std::size_t buffered_frames() const noexcept;
    [[nodiscard]] DualAudioMixerDiagnostics diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    static constexpr uint64_t kEmptyTag =
        (std::numeric_limits<uint64_t>::max)();

    struct SourceBuffer final {
        std::vector<float> samples;
        std::vector<uint64_t> tags;
        uint64_t expected_next_frame{0};
        uint64_t latest_end_frame{0};
        bool run_active{false};
    };

    [[nodiscard]] bool SameFormat(const AudioFormat& format) const noexcept;
    [[nodiscard]] bool TimeToFrame(MediaTime time,
                                   uint64_t& frame) const noexcept;
    [[nodiscard]] MediaTime FrameToTime(uint64_t frame) const noexcept;
    [[nodiscard]] SourceBuffer& Buffer(DualAudioInput input) noexcept;
    [[nodiscard]] const SourceBuffer& Buffer(DualAudioInput input) const noexcept;

    AudioFormat format_{};
    uint32_t capacity_frames_{0};
    uint32_t chunk_frames_{0};
    SourceBuffer system_{};
    SourceBuffer microphone_{};
    uint64_t next_output_frame_{0};
    bool output_started_{false};
    bool output_emitted_{false};
    bool discontinuity_pending_{false};
    DualAudioMixerDiagnostics diagnostics_{};
};

}  // namespace easnap::media::detail
