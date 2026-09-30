// Easnap - sample-accurate PCM mapping onto the pause-excluding media timeline.

#include "recording/audio_timeline.h"

#include <algorithm>
#include <limits>

namespace easnap::recording {
namespace {

constexpr int64_t kMediaTicksPerSecond = 10'000'000;
constexpr media::MediaTime kQpcDiscontinuityThreshold{500'000};  // 50 ms

[[nodiscard]] bool SameFormat(const media::AudioFormat& left,
                              const media::AudioFormat& right) noexcept {
    return left.sample_rate == right.sample_rate &&
           left.channels == right.channels &&
           left.bits_per_sample == right.bits_per_sample &&
           left.is_float == right.is_float &&
           left.channel_mask == right.channel_mask;
}

[[nodiscard]] bool FramesToTime(uint64_t frames, uint32_t sample_rate,
                                media::MediaTime& time) noexcept {
    if (sample_rate == 0) {
        return false;
    }
    const uint64_t seconds = frames / sample_rate;
    const uint64_t remainder = frames % sample_rate;
    constexpr uint64_t maximum =
        static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    if (seconds > maximum / static_cast<uint64_t>(kMediaTicksPerSecond)) {
        return false;
    }
    const uint64_t ticks =
        seconds * static_cast<uint64_t>(kMediaTicksPerSecond) +
        (remainder * static_cast<uint64_t>(kMediaTicksPerSecond)) /
            sample_rate;
    if (ticks > maximum) {
        return false;
    }
    time = media::MediaTime{static_cast<int64_t>(ticks)};
    return true;
}

[[nodiscard]] uint64_t TimeToFramesCeiling(media::MediaTime time,
                                            uint32_t sample_rate) noexcept {
    if (time <= media::MediaTime::zero() || sample_rate == 0) {
        return 0;
    }
    const uint64_t ticks = static_cast<uint64_t>(time.count());
    const uint64_t seconds = ticks / kMediaTicksPerSecond;
    const uint64_t remainder = ticks % kMediaTicksPerSecond;
    const uint64_t whole = seconds * sample_rate;
    const uint64_t partial =
        (remainder * sample_rate + kMediaTicksPerSecond - 1) /
        kMediaTicksPerSecond;
    return whole + partial;
}

[[nodiscard]] media::MediaTime AbsoluteDifference(media::MediaTime left,
                                                   media::MediaTime right) noexcept {
    return left >= right ? left - right : right - left;
}

[[nodiscard]] bool AddTime(media::MediaTime left, media::MediaTime right,
                           media::MediaTime& result) noexcept {
    if (right < media::MediaTime::zero() ||
        left.count() > (std::numeric_limits<int64_t>::max)() - right.count()) {
        return false;
    }
    result = left + right;
    return true;
}

}  // namespace

std::optional<media::MediaTime> NextAudioStreamTick(
    media::MediaTime video_position, media::MediaTime interleave_lag,
    media::MediaTime committed_audio_end, media::MediaTime last_tick,
    std::optional<media::MediaTime> pending_audio_start) noexcept {
    if (video_position < media::MediaTime::zero() ||
        interleave_lag < media::MediaTime::zero() ||
        committed_audio_end < media::MediaTime::zero() ||
        last_tick < media::MediaTime::zero() || video_position <= interleave_lag) {
        return std::nullopt;
    }
    media::MediaTime target = video_position - interleave_lag;
    if (pending_audio_start.has_value() && target > *pending_audio_start) {
        target = *pending_audio_start;
    }
    if (target <= committed_audio_end || target <= last_tick) {
        return std::nullopt;
    }
    return target;
}

bool AudioTimelineMapper::Reset(const media::AudioFormat& format) noexcept {
    format_ = format;
    segment_active_offset_ = media::MediaTime::zero();
    segment_qpc_anchor_ = media::MediaTime::zero();
    run_mapped_start_ = media::MediaTime::zero();
    run_source_qpc_start_ = media::MediaTime::zero();
    last_end_ = media::MediaTime::zero();
    run_frame_count_ = 0;
    next_device_position_ = 0;
    segment_active_ = false;
    run_active_ = false;
    run_qpc_precise_ = false;
    run_device_position_reliable_ = false;
    return format_.valid();
}

bool AudioTimelineMapper::BeginSegment(media::MediaTime active_offset,
                                       media::MediaTime qpc_anchor) noexcept {
    if (!format_.valid() || active_offset < media::MediaTime::zero() ||
        qpc_anchor < media::MediaTime::zero()) {
        return false;
    }
    // An accepted packet immediately before Pause can end a fraction beyond
    // the video clock sample taken just afterward. Never move the audio
    // timeline backward when a resumed segment is anchored.
    segment_active_offset_ = (std::max)(active_offset, last_end_);
    segment_qpc_anchor_ = qpc_anchor;
    segment_active_ = true;
    run_active_ = false;
    run_qpc_precise_ = false;
    run_device_position_reliable_ = false;
    run_frame_count_ = 0;
    return true;
}

AudioPacketMapResult AudioTimelineMapper::Map(
    const media::AudioPacket& packet, MappedAudioPacket& mapped) noexcept {
    mapped = {};
    if (!segment_active_ || !packet.consistent() ||
        !SameFormat(packet.format, format_) ||
        packet.timestamp < media::MediaTime::zero() || packet.frame_count == 0) {
        return AudioPacketMapResult::Invalid;
    }

    uint32_t first_frame = 0;
    const bool qpc_reliable =
        packet.timestamp_source !=
        media::AudioTimestampSource::CaptureQpcFallback;
    if (packet.timestamp < segment_qpc_anchor_) {
        const uint64_t frames_before_segment = TimeToFramesCeiling(
            segment_qpc_anchor_ - packet.timestamp, format_.sample_rate);
        if (frames_before_segment >= packet.frame_count) {
            return AudioPacketMapResult::Dropped;
        }
        first_frame = static_cast<uint32_t>(frames_before_segment);
    }

    const bool explicit_discontinuity =
        media::HasFlag(packet.flags, media::AudioPacketFlags::Discontinuity) ||
        media::HasFlag(packet.flags, media::AudioPacketFlags::DeviceChanged);
    bool new_run = !run_active_ || explicit_discontinuity;
    AudioTimelineDecision decision =
        !run_active_ ? AudioTimelineDecision::SegmentStart
                     : AudioTimelineDecision::Continuous;
    if (explicit_discontinuity) {
        decision = AudioTimelineDecision::ExplicitDiscontinuity;
    }
    int64_t qpc_error_100ns = 0;
    if (!new_run && run_device_position_reliable_ && qpc_reliable) {
        if (packet.device_position >
            (std::numeric_limits<uint64_t>::max)() - first_frame) {
            return AudioPacketMapResult::Invalid;
        }
        const uint64_t adjusted_device_position =
            packet.device_position + static_cast<uint64_t>(first_frame);
        new_run = adjusted_device_position != next_device_position_;
        if (new_run) {
            decision = AudioTimelineDecision::DevicePositionDiscontinuity;
        }
    }
    if (!new_run) {
        media::MediaTime run_elapsed{};
        media::MediaTime expected_qpc{};
        if (!FramesToTime(run_frame_count_, format_.sample_rate, run_elapsed) ||
            !AddTime(run_source_qpc_start_, run_elapsed, expected_qpc)) {
            return AudioPacketMapResult::Invalid;
        }
        media::MediaTime skipped{};
        if (!FramesToTime(first_frame, format_.sample_rate, skipped)) {
            return AudioPacketMapResult::Invalid;
        }
        media::MediaTime observed_qpc{};
        if (!AddTime(packet.timestamp, skipped, observed_qpc)) {
            return AudioPacketMapResult::Invalid;
        }
        qpc_error_100ns = (observed_qpc - expected_qpc).count();
        if (!qpc_reliable) {
            // Capture-QPC fallback is not stable enough to cadence every
            // packet, but its padding-adjusted first-frame observation still
            // preserves a real forward gap after source silence or endpoint
            // wake-up. Re-anchor that gap once; subsequent packets remain
            // sample-count-timed and ignore ordinary callback jitter.
            if (observed_qpc > expected_qpc &&
                observed_qpc - expected_qpc > kQpcDiscontinuityThreshold) {
                new_run = true;
                decision = AudioTimelineDecision::FallbackTimestampGap;
            }
        } else if (!run_qpc_precise_) {
            if (observed_qpc < run_elapsed) {
                return AudioPacketMapResult::Invalid;
            }
            // A capture-thread fallback is suitable for placing a new run but
            // deliberately excluded from packet-by-packet drift decisions.
            // The first precise clock pair calibrates the run without moving
            // PCM that has already been accepted on the sample timeline.
            run_source_qpc_start_ = observed_qpc - run_elapsed;
            run_qpc_precise_ = true;
            decision = AudioTimelineDecision::ClockCalibration;
        } else if (AbsoluteDifference(observed_qpc, expected_qpc) >
                   kQpcDiscontinuityThreshold) {
            new_run = true;
            decision = AudioTimelineDecision::QpcDiscontinuity;
        }
    }

    const bool mark_discontinuity =
        explicit_discontinuity || (new_run && run_active_);
    const bool mapped_ok =
        new_run ? StartRun(packet, first_frame, qpc_reliable,
                           mark_discontinuity, decision,
                           qpc_error_100ns, mapped)
                : MapContinuous(packet, first_frame, qpc_reliable, mapped);
    if (!mapped_ok) {
        return AudioPacketMapResult::Invalid;
    }
    if (mapped.frame_count == 0) {
        return AudioPacketMapResult::Dropped;
    }
    if (!new_run) {
        mapped.qpc_precise = run_qpc_precise_;
        mapped.decision = decision;
        mapped.qpc_error_100ns = qpc_error_100ns;
    }
    return AudioPacketMapResult::Mapped;
}

bool AudioTimelineMapper::StartRun(const media::AudioPacket& packet,
                                   uint32_t first_frame, bool qpc_reliable,
                                   bool explicit_discontinuity,
                                   AudioTimelineDecision decision,
                                   int64_t qpc_error_100ns,
                                   MappedAudioPacket& mapped) noexcept {
    media::MediaTime skipped_time{};
    if (!FramesToTime(first_frame, format_.sample_rate, skipped_time)) {
        return false;
    }

    media::MediaTime source_start{};
    if (!AddTime(packet.timestamp, skipped_time, source_start) ||
        (qpc_reliable &&
         packet.device_position >
             (std::numeric_limits<uint64_t>::max)() - first_frame)) {
        return false;
    }
    const media::MediaTime from_segment =
        source_start > segment_qpc_anchor_
            ? source_start - segment_qpc_anchor_
            : media::MediaTime::zero();
    media::MediaTime mapped_start{};
    if (!AddTime(segment_active_offset_, from_segment, mapped_start)) {
        return false;
    }

    if (mapped_start < last_end_) {
        const uint64_t overlap_frames = TimeToFramesCeiling(
            last_end_ - mapped_start, format_.sample_rate);
        const uint64_t remaining =
            static_cast<uint64_t>(packet.frame_count - first_frame);
        if (overlap_frames >= remaining) {
            mapped.frame_count = 0;
            return true;
        }
        first_frame += static_cast<uint32_t>(overlap_frames);
        if (!FramesToTime(first_frame, format_.sample_rate, skipped_time)) {
            return false;
        }
        if (!AddTime(packet.timestamp, skipped_time, source_start)) {
            return false;
        }
        mapped_start = last_end_;
    }

    run_mapped_start_ = mapped_start;
    run_source_qpc_start_ = source_start;
    run_frame_count_ = 0;
    next_device_position_ = 0;
    run_active_ = true;
    run_qpc_precise_ = qpc_reliable;
    run_device_position_reliable_ = false;

    const media::MediaTime previous_end = last_end_;
    if (!MapContinuous(packet, first_frame, qpc_reliable, mapped)) {
        return false;
    }
    mapped.discontinuity =
        explicit_discontinuity || mapped.timestamp > previous_end;
    mapped.qpc_precise = run_qpc_precise_;
    mapped.decision = decision;
    mapped.qpc_error_100ns = qpc_error_100ns;
    return true;
}

bool AudioTimelineMapper::MapContinuous(const media::AudioPacket& packet,
                                        uint32_t first_frame,
                                        bool device_position_reliable,
                                        MappedAudioPacket& mapped) noexcept {
    if (first_frame >= packet.frame_count) {
        mapped.frame_count = 0;
        return true;
    }
    const uint32_t frame_count = packet.frame_count - first_frame;
    if (run_frame_count_ >
        (std::numeric_limits<uint64_t>::max)() - frame_count) {
        return false;
    }
    media::MediaTime start_offset{};
    media::MediaTime end_offset{};
    if (!FramesToTime(run_frame_count_, format_.sample_rate, start_offset) ||
        !FramesToTime(run_frame_count_ + frame_count, format_.sample_rate,
                      end_offset)) {
        return false;
    }
    media::MediaTime timestamp{};
    if (!AddTime(run_mapped_start_, start_offset, timestamp)) {
        return false;
    }
    const media::MediaTime duration = end_offset - start_offset;
    if (duration <= media::MediaTime::zero()) {
        return false;
    }

    mapped.timestamp = timestamp;
    mapped.duration = duration;
    mapped.first_frame = first_frame;
    mapped.frame_count = frame_count;
    mapped.discontinuity = false;

    run_frame_count_ += frame_count;
    if (device_position_reliable) {
        if (packet.device_position >
            (std::numeric_limits<uint64_t>::max)() - packet.frame_count) {
            return false;
        }
        next_device_position_ = packet.device_position + packet.frame_count;
        run_device_position_reliable_ = true;
    } else if (run_device_position_reliable_) {
        // The provider could not validate this packet's WASAPI position pair.
        // Preserve sample-count continuity and advance the last trustworthy
        // device frontier by the accepted frames; never let a stale wake-up
        // position split a continuous PCM run.
        if (next_device_position_ >
            (std::numeric_limits<uint64_t>::max)() - frame_count) {
            return false;
        }
        next_device_position_ += frame_count;
    }
    if (!AddTime(timestamp, duration, last_end_)) {
        return false;
    }
    return true;
}

}  // namespace easnap::recording
