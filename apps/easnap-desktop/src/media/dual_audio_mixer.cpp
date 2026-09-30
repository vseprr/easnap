// Easnap - bounded, clock-aligned two-source PCM mixer.

#include "media/dual_audio_mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace easnap::media::detail {
namespace {

constexpr uint64_t kTicksPerSecond = 10'000'000;

[[nodiscard]] double ReadSample(const uint8_t* data, uint16_t bits,
                                bool is_float) noexcept {
    if (is_float && bits == 32) {
        float value = 0.0F;
        std::memcpy(&value, data, sizeof(value));
        return std::isfinite(value) ? value : 0.0;
    }
    if (is_float && bits == 64) {
        double value = 0.0;
        std::memcpy(&value, data, sizeof(value));
        return std::isfinite(value) ? value : 0.0;
    }
    if (!is_float && bits == 8) {
        return (static_cast<double>(*data) - 128.0) / 128.0;
    }
    if (!is_float && bits == 16) {
        int16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value) / 32768.0;
    }
    if (!is_float && bits == 24) {
        int32_t value = static_cast<int32_t>(data[0]) |
                        (static_cast<int32_t>(data[1]) << 8) |
                        (static_cast<int32_t>(data[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<int32_t>(0xFF000000);
        }
        return static_cast<double>(value) / 8388608.0;
    }
    if (!is_float && bits == 32) {
        int32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value) / 2147483648.0;
    }
    return 0.0;
}

void WriteSample(uint8_t* data, uint16_t bits, bool is_float,
                 double value) noexcept {
    value = (std::clamp)(std::isfinite(value) ? value : 0.0, -1.0, 1.0);
    if (is_float && bits == 32) {
        const float output = static_cast<float>(value);
        std::memcpy(data, &output, sizeof(output));
    } else if (is_float && bits == 64) {
        std::memcpy(data, &value, sizeof(value));
    } else if (!is_float && bits == 8) {
        *data = static_cast<uint8_t>((std::clamp)(
            std::lround(value * 127.0 + 128.0), 0L, 255L));
    } else if (!is_float && bits == 16) {
        const int16_t output = static_cast<int16_t>((std::clamp)(
            std::lround(value * 32767.0), -32768L, 32767L));
        std::memcpy(data, &output, sizeof(output));
    } else if (!is_float && bits == 24) {
        const int32_t output = static_cast<int32_t>((std::clamp)(
            std::llround(value * 8388607.0), -8388608LL, 8388607LL));
        data[0] = static_cast<uint8_t>(output & 0xFF);
        data[1] = static_cast<uint8_t>((output >> 8) & 0xFF);
        data[2] = static_cast<uint8_t>((output >> 16) & 0xFF);
    } else if (!is_float && bits == 32) {
        const int64_t scaled = (std::clamp)(
            std::llround(value * 2147483647.0),
            static_cast<long long>((std::numeric_limits<int32_t>::min)()),
            static_cast<long long>((std::numeric_limits<int32_t>::max)()));
        const int32_t output = static_cast<int32_t>(scaled);
        std::memcpy(data, &output, sizeof(output));
    }
}

[[nodiscard]] uint64_t SaturatingAdd(uint64_t left, uint64_t right) noexcept {
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

}  // namespace

DualAudioMixer::DualAudioMixer(AudioFormat format, uint32_t capacity_frames,
                               uint32_t chunk_frames)
    : format_(format),
      capacity_frames_(capacity_frames),
      chunk_frames_(chunk_frames) {
    if (!valid()) {
        return;
    }
    const std::size_t sample_count =
        static_cast<std::size_t>(capacity_frames_) * format_.channels;
    system_.samples.resize(sample_count, 0.0F);
    system_.tags.resize(capacity_frames_, kEmptyTag);
    microphone_.samples.resize(sample_count, 0.0F);
    microphone_.tags.resize(capacity_frames_, kEmptyTag);
}

bool DualAudioMixer::valid() const noexcept {
    const bool supported_sample =
        (format_.is_float &&
         (format_.bits_per_sample == 32 || format_.bits_per_sample == 64)) ||
        (!format_.is_float &&
         (format_.bits_per_sample == 8 || format_.bits_per_sample == 16 ||
          format_.bits_per_sample == 24 || format_.bits_per_sample == 32));
    return format_.valid() && supported_sample && format_.channels > 0 &&
           format_.channels <= 8 && capacity_frames_ >= chunk_frames_ &&
           chunk_frames_ > 0;
}

void DualAudioMixer::Reset() noexcept {
    std::fill(system_.tags.begin(), system_.tags.end(), kEmptyTag);
    std::fill(microphone_.tags.begin(), microphone_.tags.end(), kEmptyTag);
    system_.expected_next_frame = 0;
    microphone_.expected_next_frame = 0;
    system_.latest_end_frame = 0;
    microphone_.latest_end_frame = 0;
    system_.run_active = false;
    microphone_.run_active = false;
    next_output_frame_ = 0;
    output_started_ = false;
    output_emitted_ = false;
    discontinuity_pending_ = false;
}

bool DualAudioMixer::SameFormat(const AudioFormat& format) const noexcept {
    return format.sample_rate == format_.sample_rate &&
           format.channels == format_.channels &&
           format.bits_per_sample == format_.bits_per_sample &&
           format.is_float == format_.is_float &&
           format.channel_mask == format_.channel_mask;
}

bool DualAudioMixer::TimeToFrame(MediaTime time, uint64_t& frame) const noexcept {
    if (time < MediaTime::zero() || format_.sample_rate == 0) {
        return false;
    }
    const uint64_t ticks = static_cast<uint64_t>(time.count());
    const uint64_t seconds = ticks / kTicksPerSecond;
    const uint64_t remainder = ticks % kTicksPerSecond;
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    if (seconds > maximum / format_.sample_rate) {
        return false;
    }
    const uint64_t whole = seconds * format_.sample_rate;
    const uint64_t partial =
        (remainder * format_.sample_rate + kTicksPerSecond / 2) /
        kTicksPerSecond;
    if (partial > maximum - whole) {
        return false;
    }
    frame = whole + partial;
    return true;
}

MediaTime DualAudioMixer::FrameToTime(uint64_t frame) const noexcept {
    const uint64_t seconds = frame / format_.sample_rate;
    const uint64_t remainder = frame % format_.sample_rate;
    const uint64_t ticks = seconds * kTicksPerSecond +
                           (remainder * kTicksPerSecond) / format_.sample_rate;
    if (ticks > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
        return MediaTime{(std::numeric_limits<int64_t>::max)()};
    }
    return MediaTime{static_cast<int64_t>(ticks)};
}

DualAudioMixer::SourceBuffer& DualAudioMixer::Buffer(
    DualAudioInput input) noexcept {
    return input == DualAudioInput::SystemAudio ? system_ : microphone_;
}

const DualAudioMixer::SourceBuffer& DualAudioMixer::Buffer(
    DualAudioInput input) const noexcept {
    return input == DualAudioInput::SystemAudio ? system_ : microphone_;
}

DualAudioMixerPushResult DualAudioMixer::Push(DualAudioInput input,
                                               const AudioPacket& packet) noexcept {
    if (!valid() || !packet.consistent() || packet.frame_count == 0 ||
        !SameFormat(packet.format)) {
        diagnostics_.invalid_packets =
            SaturatingAdd(diagnostics_.invalid_packets, 1);
        return DualAudioMixerPushResult::InvalidPacket;
    }

    uint64_t timestamp_frame = 0;
    if (!TimeToFrame(packet.timestamp, timestamp_frame)) {
        diagnostics_.invalid_packets =
            SaturatingAdd(diagnostics_.invalid_packets, 1);
        return DualAudioMixerPushResult::InvalidPacket;
    }

    SourceBuffer& source = Buffer(input);
    const bool discontinuity =
        HasFlag(packet.flags, AudioPacketFlags::Discontinuity) ||
        HasFlag(packet.flags, AudioPacketFlags::DeviceChanged);

    uint64_t start_frame = timestamp_frame;
    if (source.run_active && !discontinuity) {
        const uint64_t expected = source.expected_next_frame;
        const uint64_t diff = timestamp_frame >= expected
                                  ? timestamp_frame - expected
                                  : expected - timestamp_frame;
        if (diff <= kContinuityToleranceFrames) {
            // Normal packet scheduling / QPC jitter within an established run.
            // Preserve sample-count continuity: place consecutive packets without gaps or overwrites.
            start_frame = expected;
        } else {
            // Significant gap or clock jump: re-anchor to the observed timestamp.
            start_frame = timestamp_frame;
        }
    }
    source.run_active = true;

    if (!output_started_) {
        next_output_frame_ = (start_frame / chunk_frames_) * chunk_frames_;
        output_started_ = true;
    } else if (!output_emitted_ && start_frame < next_output_frame_) {
        next_output_frame_ = (start_frame / chunk_frames_) * chunk_frames_;
    }

    uint32_t first_frame = 0;
    if (start_frame < next_output_frame_) {
        const uint64_t late = next_output_frame_ - start_frame;
        if (late >= packet.frame_count) {
            // Packet ended entirely at or before the next output frame.
            // Obsolete late frames cannot be placed into the already-emitted timeline.
            diagnostics_.frames_dropped_late = SaturatingAdd(
                diagnostics_.frames_dropped_late, packet.frame_count);
            diagnostics_.packets_dropped_late =
                SaturatingAdd(diagnostics_.packets_dropped_late, 1);
            source.run_active = false;
            source.expected_next_frame = 0;
            return DualAudioMixerPushResult::LateDiscarded;
        }
        first_frame = static_cast<uint32_t>(late);
        diagnostics_.frames_dropped_late =
            SaturatingAdd(diagnostics_.frames_dropped_late, late);
        start_frame = next_output_frame_;
    }

    const uint64_t remaining_frames =
        static_cast<uint64_t>(packet.frame_count - first_frame);
    const uint64_t end_frame = SaturatingAdd(start_frame, remaining_frames);
    const uint64_t capacity_end =
        SaturatingAdd(next_output_frame_, capacity_frames_);

    if (start_frame >= capacity_end) {
        // Entire packet is beyond the bounded buffer capacity.
        diagnostics_.frames_dropped_overflow =
            SaturatingAdd(diagnostics_.frames_dropped_overflow, remaining_frames);
        diagnostics_.capacity_failures =
            SaturatingAdd(diagnostics_.capacity_failures, 1);
        return DualAudioMixerPushResult::CapacityExceeded;
    }

    const uint64_t accepted_end = (std::min)(end_frame, capacity_end);
    const bool truncated = accepted_end < end_frame;
    if (truncated) {
        diagnostics_.frames_dropped_overflow = SaturatingAdd(
            diagnostics_.frames_dropped_overflow, end_frame - accepted_end);
        diagnostics_.capacity_failures =
            SaturatingAdd(diagnostics_.capacity_failures, 1);
    }

    const std::size_t bytes_per_sample = format_.bits_per_sample / 8u;
    for (uint64_t frame = start_frame; frame < accepted_end; ++frame) {
        const std::size_t slot =
            static_cast<std::size_t>(frame % capacity_frames_);
        const std::size_t source_frame =
            static_cast<std::size_t>(first_frame) +
            static_cast<std::size_t>(frame - start_frame);
        for (uint16_t channel = 0; channel < format_.channels; ++channel) {
            const std::size_t byte_offset =
                (source_frame * format_.channels + channel) * bytes_per_sample;
            source.samples[slot * format_.channels + channel] =
                static_cast<float>(ReadSample(packet.data.data() + byte_offset,
                                              format_.bits_per_sample,
                                              format_.is_float));
        }
        source.tags[slot] = frame;
    }

    source.latest_end_frame = (std::max)(source.latest_end_frame, accepted_end);
    source.expected_next_frame = accepted_end;
    discontinuity_pending_ = discontinuity_pending_ || discontinuity;
    diagnostics_.packets_accepted =
        SaturatingAdd(diagnostics_.packets_accepted, 1);
    diagnostics_.peak_buffered_frames =
        (std::max)(diagnostics_.peak_buffered_frames, buffered_frames());

    return truncated ? DualAudioMixerPushResult::TruncatedOverflow
                     : DualAudioMixerPushResult::Success;
}

bool DualAudioMixer::PopUntil(MediaTime watermark,
                              AudioPacket& packet) noexcept {
    packet = {};
    uint64_t watermark_frame = 0;
    if (!output_started_ || !TimeToFrame(watermark, watermark_frame) ||
        watermark_frame < next_output_frame_ ||
        watermark_frame - next_output_frame_ < chunk_frames_) {
        return false;
    }

    packet.format = format_;
    packet.frame_count = chunk_frames_;
    packet.timestamp = FrameToTime(next_output_frame_);
    packet.device_position = next_output_frame_;
    packet.timestamp_source = AudioTimestampSource::WasapiQpc;
    packet.data.resize(static_cast<std::size_t>(chunk_frames_) *
                       format_.block_align());
    const std::size_t bytes_per_sample = format_.bits_per_sample / 8u;
    bool any_nonzero = false;

    for (uint32_t offset = 0; offset < chunk_frames_; ++offset) {
        const uint64_t frame = next_output_frame_ + offset;
        const std::size_t slot =
            static_cast<std::size_t>(frame % capacity_frames_);
        const bool has_system = system_.tags[slot] == frame;
        const bool has_microphone = microphone_.tags[slot] == frame;
        for (uint16_t channel = 0; channel < format_.channels; ++channel) {
            const std::size_t sample_index =
                slot * format_.channels + channel;
            const float system_sample =
                has_system ? system_.samples[sample_index] : 0.0F;
            const float microphone_sample =
                has_microphone ? microphone_.samples[sample_index] : 0.0F;
            float mixed = 0.5F * system_sample + 0.5F * microphone_sample;
            if (!std::isfinite(mixed)) {
                mixed = 0.0F;
            }
            mixed = (std::clamp)(mixed, -1.0F, 1.0F);
            const std::size_t byte_offset =
                (static_cast<std::size_t>(offset) * format_.channels + channel) *
                bytes_per_sample;
            WriteSample(packet.data.data() + byte_offset,
                        format_.bits_per_sample, format_.is_float, mixed);
            any_nonzero = any_nonzero || mixed != 0.0F;
        }
        if (has_system) {
            system_.tags[slot] = kEmptyTag;
        }
        if (has_microphone) {
            microphone_.tags[slot] = kEmptyTag;
        }
    }

    packet.flags = any_nonzero ? AudioPacketFlags::None
                               : AudioPacketFlags::Silent;
    if (discontinuity_pending_) {
        packet.flags |= AudioPacketFlags::Discontinuity;
        discontinuity_pending_ = false;
    }
    next_output_frame_ = SaturatingAdd(next_output_frame_, chunk_frames_);
    output_emitted_ = true;
    return true;
}

MediaTime DualAudioMixer::latest_buffered_end() const noexcept {
    return FrameToTime((std::max)(system_.latest_end_frame,
                                  microphone_.latest_end_frame));
}

std::size_t DualAudioMixer::buffered_frames() const noexcept {
    if (!output_started_) {
        return 0;
    }
    const uint64_t latest =
        (std::max)(system_.latest_end_frame, microphone_.latest_end_frame);
    const uint64_t buffered = latest > next_output_frame_
                                  ? latest - next_output_frame_
                                  : 0;
    return static_cast<std::size_t>(
        (std::min)(buffered, static_cast<uint64_t>(capacity_frames_)));
}

}  // namespace easnap::media::detail
