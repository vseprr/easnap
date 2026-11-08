// Easnap - sample-accurate PCM mapping onto the pause-excluding media timeline.
#pragma once

#include <cstdint>
#include <optional>

#include "media/audio_format.h"

namespace easnap::recording {

enum class AudioPacketMapResult {
    Mapped,
    Dropped,
    Invalid,
};

enum class AudioTimelineDecision : uint8_t {
    SegmentStart,
    Continuous,
    ClockCalibration,
    ExplicitDiscontinuity,
    DevicePositionDiscontinuity,
    QpcDiscontinuity,
    FallbackTimestampGap,
};

struct MappedAudioPacket {
    media::MediaTime timestamp{0};
    media::MediaTime duration{0};
    uint32_t first_frame{0};
    uint32_t frame_count{0};
    bool discontinuity{false};
    bool qpc_precise{false};
    AudioTimelineDecision decision{AudioTimelineDecision::Continuous};
    int64_t qpc_error_100ns{0};
};

/// Chooses the next writer-safe sparse-audio tick. A mapped packet that is
/// waiting for video caps the tick but is not treated as already committed.
[[nodiscard]] std::optional<media::MediaTime> NextAudioStreamTick(
    media::MediaTime video_position, media::MediaTime interleave_lag,
    media::MediaTime committed_audio_end, media::MediaTime last_tick,
    std::optional<media::MediaTime> pending_audio_start) noexcept;

/// Latest audio end the writer may accept once video has been written up to
/// `video_end`: audio may lead video by `maximum_lead`. The sum saturates at
/// the MediaTime range instead of overflowing.
[[nodiscard]] media::MediaTime AudioWriterGateLimit(
    media::MediaTime video_end, media::MediaTime maximum_lead) noexcept;

/// Removes the frames of `mapped` that begin before `committed_end`, the end
/// of the audio already written, in whole frames rounded up. The kept suffix
/// starts exactly at `committed_end`. Returns Mapped when nothing overlaps or
/// a suffix remains, Dropped when every frame overlaps, and Invalid when
/// `sample_rate` is zero or the trimmed packet cannot be represented.
/// `mapped` changes only when a suffix remains.
[[nodiscard]] AudioPacketMapResult TrimAudioOverlap(
    MappedAudioPacket& mapped, media::MediaTime committed_end,
    uint32_t sample_rate) noexcept;

/// Maps QPC-anchored WASAPI packets onto recording active time. Within a
/// continuous device run, every boundary is derived from cumulative PCM frame
/// counts, so callback scheduling never perturbs sample timestamps.
class AudioTimelineMapper final {
public:
    [[nodiscard]] bool Reset(const media::AudioFormat& format) noexcept;
    [[nodiscard]] bool BeginSegment(media::MediaTime active_offset,
                                    media::MediaTime qpc_anchor) noexcept;
    [[nodiscard]] AudioPacketMapResult Map(
        const media::AudioPacket& packet,
        MappedAudioPacket& mapped) noexcept;

    [[nodiscard]] media::MediaTime last_end() const noexcept { return last_end_; }

private:
    [[nodiscard]] bool StartRun(const media::AudioPacket& packet,
                                uint32_t first_frame, bool qpc_reliable,
                                bool explicit_discontinuity,
                                AudioTimelineDecision decision,
                                int64_t qpc_error_100ns,
                                MappedAudioPacket& mapped) noexcept;
    [[nodiscard]] bool MapContinuous(const media::AudioPacket& packet,
                                     uint32_t first_frame,
                                     bool device_position_reliable,
                                     MappedAudioPacket& mapped) noexcept;

    media::AudioFormat format_{};
    media::MediaTime segment_active_offset_{0};
    media::MediaTime segment_qpc_anchor_{0};
    media::MediaTime run_mapped_start_{0};
    media::MediaTime run_source_qpc_start_{0};
    media::MediaTime last_end_{0};
    uint64_t run_frame_count_{0};
    uint64_t next_device_position_{0};
    bool segment_active_{false};
    bool run_active_{false};
    bool run_qpc_precise_{false};
    bool run_device_position_reliable_{false};
};

}  // namespace easnap::recording
