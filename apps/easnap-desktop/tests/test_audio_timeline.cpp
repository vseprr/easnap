#include <cstdint>
#include <cstdio>
#include <limits>

#include "recording/audio_timeline.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expression);
    }
}

#define CHECK(expression) Check((expression), #expression, __FILE__, __LINE__)

easnap::media::AudioFormat TestFormat() {
    easnap::media::AudioFormat format;
    format.sample_rate = 48'000;
    format.channels = 2;
    format.bits_per_sample = 32;
    format.is_float = true;
    format.channel_mask = 3;
    return format;
}

easnap::media::AudioPacket Packet(
    const easnap::media::AudioFormat& format, uint32_t frames,
    int64_t qpc_100ns, uint64_t device_position,
    easnap::media::AudioPacketFlags flags =
        easnap::media::AudioPacketFlags::None,
    easnap::media::AudioTimestampSource timestamp_source =
        easnap::media::AudioTimestampSource::WasapiQpc) {
    easnap::media::AudioPacket packet;
    packet.format = format;
    packet.frame_count = frames;
    packet.timestamp = easnap::media::MediaTime{qpc_100ns};
    packet.device_position = device_position;
    packet.flags = flags;
    packet.timestamp_source = timestamp_source;
    packet.data.resize(static_cast<std::size_t>(frames) * format.block_align());
    return packet;
}

void TestDelayedFallbackRunStaysAtTrueActiveTime() {
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime::zero(),
                                easnap::media::MediaTime{10'000'000}));

    constexpr uint32_t kFramesPerPacket = 480;
    constexpr uint32_t kFallbackPacketCount = 150;
    constexpr uint64_t kFirstDevicePosition = 12'345;
    constexpr easnap::media::MediaTime kPreviouslyCommittedSilenceTick{
        22'500'000};
    easnap::media::MediaTime expected_start{30'000'000};
    easnap::recording::MappedAudioPacket mapped;
    for (uint32_t index = 0; index < kFallbackPacketCount; ++index) {
        const int64_t callback_jitter =
            index == 0 ? 0 : (index % 2 == 0 ? 20'000 : -10'000);
        // Endpoint wake-up can report a stale, constant, or backward device
        // position together with an invalid timestamp pair. Fallback packets
        // must remain sample-count-contiguous until a precise pair arrives.
        const uint64_t unreliable_device_position =
            index % 3 == 0 ? kFirstDevicePosition
                           : kFirstDevicePosition - (index % 2);
        auto packet = Packet(
            format, kFramesPerPacket,
            40'000'000 + static_cast<int64_t>(index) * 100'000 +
                callback_jitter,
            unreliable_device_position,
            easnap::media::AudioPacketFlags::TimestampError,
            easnap::media::AudioTimestampSource::CaptureQpcFallback);
        CHECK(timeline.Map(packet, mapped) ==
              easnap::recording::AudioPacketMapResult::Mapped);
        CHECK(mapped.timestamp == expected_start);
        CHECK(mapped.duration == easnap::media::MediaTime{100'000});
        CHECK(mapped.timestamp >= kPreviouslyCommittedSilenceTick);
        CHECK(index != 0 || mapped.discontinuity);
        CHECK(index == 0 || !mapped.discontinuity);
        expected_start += mapped.duration;
    }

    // The first precise packet after the 1.5-second fallback run calibrates
    // QPC drift detection but must not move, overlap, or create a hole in PCM.
    auto precise = Packet(
        format, kFramesPerPacket, 55'000'000,
        900'000);
    CHECK(timeline.Map(precise, mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == expected_start);
    CHECK(mapped.decision ==
          easnap::recording::AudioTimelineDecision::ClockCalibration);
    CHECK(mapped.qpc_precise);
    CHECK(!mapped.discontinuity);
    expected_start += mapped.duration;

    precise = Packet(
        format, kFramesPerPacket, 55'100'000,
        900'000 + kFramesPerPacket);
    CHECK(timeline.Map(precise, mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == expected_start);
    CHECK(mapped.decision ==
          easnap::recording::AudioTimelineDecision::Continuous);
    CHECK(!mapped.discontinuity);
    expected_start += mapped.duration;

    // A later timestamp-error packet must neither replace the established
    // device frontier with its bogus position nor inject callback jitter.
    auto fallback_between_precise = Packet(
        format, kFramesPerPacket, 55'230'000, 0,
        easnap::media::AudioPacketFlags::TimestampError,
        easnap::media::AudioTimestampSource::CaptureQpcFallback);
    CHECK(timeline.Map(fallback_between_precise, mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == expected_start);
    CHECK(mapped.decision ==
          easnap::recording::AudioTimelineDecision::Continuous);
    CHECK(!mapped.discontinuity);
    expected_start += mapped.duration;

    precise = Packet(format, kFramesPerPacket, 55'300'000,
                     900'000 + 3 * kFramesPerPacket);
    CHECK(timeline.Map(precise, mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == expected_start);
    CHECK(mapped.decision ==
          easnap::recording::AudioTimelineDecision::Continuous);
    CHECK(!mapped.discontinuity);
}

void TestResetClearsWakeupRunState() {
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    easnap::recording::MappedAudioPacket mapped;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime::zero(),
                                easnap::media::MediaTime{10'000'000}));
    CHECK(timeline.Map(Packet(format, 480, 40'000'000, 900'000), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);

    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime::zero(),
                                easnap::media::MediaTime{80'000'000}));
    CHECK(timeline.Map(Packet(format, 480, 80'000'000, 0), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == easnap::media::MediaTime::zero());
    CHECK(mapped.decision ==
          easnap::recording::AudioTimelineDecision::SegmentStart);
    CHECK(timeline.last_end() == easnap::media::MediaTime{100'000});
}

void TestFallbackWakeAfterSparseTickStartsAtResolvedTime() {
    using easnap::media::AudioPacketFlags;
    using easnap::media::AudioTimestampSource;
    using easnap::media::MediaTime;
    using easnap::recording::AudioPacketMapResult;
    using easnap::recording::AudioTimelineDecision;

    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    easnap::recording::MappedAudioPacket mapped;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(MediaTime::zero(), MediaTime{10'000'000}));

    CHECK(timeline.Map(Packet(format, 480, 10'000'000, 0), mapped) ==
          AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == MediaTime::zero());

    const auto silence_tick = easnap::recording::NextAudioStreamTick(
        MediaTime{30'000'000}, MediaTime{7'500'000}, timeline.last_end(),
        MediaTime::zero(), std::nullopt);
    CHECK(silence_tick.has_value());
    CHECK(silence_tick.value_or(MediaTime{-1}) == MediaTime{22'500'000});

    auto fallback = Packet(format, 480, 40'000'000, 0,
                           AudioPacketFlags::TimestampError,
                           AudioTimestampSource::CaptureQpcFallback);
    CHECK(timeline.Map(fallback, mapped) == AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == MediaTime{30'000'000});
    CHECK(mapped.timestamp >= silence_tick.value_or(MediaTime{-1}));
    CHECK(mapped.decision == AudioTimelineDecision::FallbackTimestampGap);
    CHECK(mapped.discontinuity);

    fallback = Packet(format, 480, 40'120'000, 0,
                      AudioPacketFlags::TimestampError,
                      AudioTimestampSource::CaptureQpcFallback);
    CHECK(timeline.Map(fallback, mapped) == AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == MediaTime{30'100'000});
    CHECK(mapped.decision == AudioTimelineDecision::Continuous);
    CHECK(!mapped.discontinuity);

    CHECK(timeline.Map(Packet(format, 480, 40'200'000, 75'000), mapped) ==
          AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == MediaTime{30'200'000});
    CHECK(mapped.decision == AudioTimelineDecision::ClockCalibration);
    CHECK(!mapped.discontinuity);
}

void TestSparseTickUsesCommittedNotMappedFrontier() {
    using easnap::media::MediaTime;
    const auto first_tick = easnap::recording::NextAudioStreamTick(
        MediaTime{20'000'000}, MediaTime{7'500'000}, MediaTime::zero(),
        MediaTime::zero(), MediaTime{30'000'000});
    CHECK(first_tick.has_value());
    CHECK(first_tick.value_or(MediaTime{-1}) == MediaTime{12'500'000});

    const auto capped_at_pending = easnap::recording::NextAudioStreamTick(
        MediaTime{40'000'000}, MediaTime{7'500'000}, MediaTime::zero(),
        first_tick.value_or(MediaTime::zero()), MediaTime{30'000'000});
    CHECK(capped_at_pending.has_value());
    CHECK(capped_at_pending.value_or(MediaTime{-1}) ==
          MediaTime{30'000'000});

    const auto advances_from_committed = easnap::recording::NextAudioStreamTick(
        MediaTime{40'000'000}, MediaTime{7'500'000}, MediaTime{30'100'000},
        MediaTime{30'000'000}, std::nullopt);
    CHECK(advances_from_committed.has_value());
    CHECK(advances_from_committed.value_or(MediaTime{-1}) ==
          MediaTime{32'500'000});

    const auto already_committed = easnap::recording::NextAudioStreamTick(
        MediaTime{40'000'000}, MediaTime{7'500'000}, MediaTime{32'500'000},
        MediaTime{30'000'000}, std::nullopt);
    CHECK(!already_committed.has_value());
}

void TestDevicePositionResetReanchorsAtResolvedQpc() {
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    easnap::recording::MappedAudioPacket mapped;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime::zero(),
                                easnap::media::MediaTime{10'000'000}));
    CHECK(timeline.Map(Packet(format, 480, 10'000'000, 50'000), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);

    CHECK(timeline.Map(Packet(format, 480, 20'000'000, 0), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == easnap::media::MediaTime{10'000'000});
    CHECK(mapped.decision ==
          easnap::recording::AudioTimelineDecision::DevicePositionDiscontinuity);
    CHECK(mapped.discontinuity);

    CHECK(timeline.Map(Packet(format, 480, 20'100'000, 480), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == easnap::media::MediaTime{10'100'000});
    CHECK(mapped.decision ==
          easnap::recording::AudioTimelineDecision::Continuous);
    CHECK(!mapped.discontinuity);
}

void TestLateFirstPacketPreservesInitialSilence() {
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime{10'000'000},
                                easnap::media::MediaTime{50'000'000}));

    easnap::recording::MappedAudioPacket mapped;
    const auto result = timeline.Map(
        Packet(format, 480, 70'000'000, 1000), mapped);
    CHECK(result == easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == easnap::media::MediaTime{30'000'000});
    CHECK(mapped.duration == easnap::media::MediaTime{100'000});
    CHECK(mapped.discontinuity);
}

void TestContinuousPacketsIgnoreQpcJitter() {
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime::zero(),
                                easnap::media::MediaTime{10'000'000}));

    constexpr uint32_t frames_per_packet = 1024;
    constexpr uint32_t packet_count = 1000;
    easnap::media::MediaTime previous_end{};
    uint64_t device_position = 5000;
    for (uint32_t index = 0; index < packet_count; ++index) {
        const int64_t ideal_qpc =
            10'000'000 + static_cast<int64_t>(
                             (static_cast<uint64_t>(index) * frames_per_packet *
                              10'000'000ULL) /
                             format.sample_rate);
        const int64_t callback_jitter = index == 0 ? 0 : (index % 2 == 0 ? 30'000 : -20'000);
        easnap::recording::MappedAudioPacket mapped;
        const auto result = timeline.Map(
            Packet(format, frames_per_packet, ideal_qpc + callback_jitter,
                   device_position),
            mapped);
        CHECK(result == easnap::recording::AudioPacketMapResult::Mapped);
        CHECK(mapped.timestamp == previous_end);
        CHECK(!mapped.discontinuity);
        previous_end = mapped.timestamp + mapped.duration;
        device_position += frames_per_packet;
    }
    const int64_t expected_end = static_cast<int64_t>(
        (static_cast<uint64_t>(frames_per_packet) * packet_count *
         10'000'000ULL) /
        format.sample_rate);
    CHECK(previous_end == easnap::media::MediaTime{expected_end});
    CHECK(timeline.last_end() == previous_end);
}

void TestPauseResumeExcludesPausedWallTime() {
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime::zero(),
                                easnap::media::MediaTime{10'000'000}));
    easnap::recording::MappedAudioPacket mapped;
    CHECK(timeline.Map(Packet(format, 480, 10'000'000, 0), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);

    CHECK(timeline.BeginSegment(easnap::media::MediaTime{20'000'000},
                                easnap::media::MediaTime{80'000'000}));
    CHECK(timeline.Map(Packet(format, 480, 85'000'000, 336'000), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == easnap::media::MediaTime{25'000'000});
    CHECK(mapped.discontinuity);
}

void TestPacketCrossingSegmentAnchorIsTrimmed() {
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime::zero(),
                                easnap::media::MediaTime{50'000}));
    easnap::recording::MappedAudioPacket mapped;
    CHECK(timeline.Map(Packet(format, 480, 0, 100), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.first_frame == 240);
    CHECK(mapped.frame_count == 240);
    CHECK(mapped.timestamp == easnap::media::MediaTime::zero());
    CHECK(mapped.duration == easnap::media::MediaTime{50'000});
}

void TestDeviceGapStartsExplicitRun() {
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(easnap::media::MediaTime::zero(),
                                easnap::media::MediaTime{10'000'000}));
    easnap::recording::MappedAudioPacket mapped;
    CHECK(timeline.Map(Packet(format, 480, 10'000'000, 0), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(timeline.Map(Packet(format, 480, 20'000'000, 10'000), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == easnap::media::MediaTime{10'000'000});
    CHECK(mapped.discontinuity);
}

// Two sequential sessions must not leak state.
void TestSequentialRecordingSessions() {
    using MT = easnap::media::MediaTime;
    const auto format = TestFormat();
    easnap::recording::AudioTimelineMapper timeline;

    // Session 1
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(MT::zero(), MT{10'000'000}));
    easnap::recording::MappedAudioPacket mapped;
    CHECK(timeline.Map(Packet(format, 480, 10'000'000, 0), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    CHECK(mapped.timestamp == MT::zero());
    const auto session1_end = timeline.last_end();

    // Session 2: full reset, new segment
    CHECK(timeline.Reset(format));
    CHECK(timeline.BeginSegment(MT::zero(), MT{50'000'000}));
    CHECK(timeline.Map(Packet(format, 480, 50'000'000, 0), mapped) ==
          easnap::recording::AudioPacketMapResult::Mapped);
    // Must start at 0 again, not carry over session1_end.
    CHECK(mapped.timestamp == MT::zero());
    CHECK(timeline.last_end() < session1_end + mapped.duration ||
          timeline.last_end() == mapped.duration);
}

void TestAudioWriterGateLimitSaturates() {
    using easnap::media::MediaTime;
    using easnap::recording::AudioWriterGateLimit;
    constexpr int64_t kMaximum = (std::numeric_limits<int64_t>::max)();
    constexpr int64_t kMinimum = (std::numeric_limits<int64_t>::min)();
    constexpr MediaTime kLead{1'000'000};

    CHECK(AudioWriterGateLimit(MediaTime::zero(), kLead) == MediaTime{1'000'000});
    CHECK(AudioWriterGateLimit(MediaTime{-5}, kLead) == MediaTime{999'995});
    CHECK(AudioWriterGateLimit(MediaTime{kMaximum - 1'000'000}, kLead) ==
          MediaTime{kMaximum});
    CHECK(AudioWriterGateLimit(MediaTime{kMaximum - 1'000'000 + 1}, kLead) ==
          MediaTime{kMaximum});
    CHECK(AudioWriterGateLimit(MediaTime{kMaximum}, kLead) == MediaTime{kMaximum});

    // Without a lead, audio may reach exactly the written video.
    CHECK(AudioWriterGateLimit(MediaTime{12'345}, MediaTime::zero()) ==
          MediaTime{12'345});
    CHECK(AudioWriterGateLimit(MediaTime{kMaximum}, MediaTime::zero()) ==
          MediaTime{kMaximum});
    CHECK(AudioWriterGateLimit(MediaTime{kMinimum}, MediaTime::zero()) ==
          MediaTime{kMinimum});

    // The session never passes a negative lead; it saturates at the bottom.
    CHECK(AudioWriterGateLimit(MediaTime{5'000'000}, MediaTime{-1'000'000}) ==
          MediaTime{4'000'000});
    CHECK(AudioWriterGateLimit(MediaTime{kMinimum + 1}, MediaTime{-1'000'000}) ==
          MediaTime{kMinimum});
}

easnap::recording::MappedAudioPacket OverlapPacket(int64_t timestamp,
                                                   uint32_t first_frame,
                                                   uint32_t frame_count,
                                                   int64_t duration) {
    easnap::recording::MappedAudioPacket mapped;
    mapped.timestamp = easnap::media::MediaTime{timestamp};
    mapped.duration = easnap::media::MediaTime{duration};
    mapped.first_frame = first_frame;
    mapped.frame_count = frame_count;
    mapped.discontinuity = true;
    mapped.qpc_precise = true;
    mapped.decision = easnap::recording::AudioTimelineDecision::ClockCalibration;
    mapped.qpc_error_100ns = -1'234;
    return mapped;
}

bool SameMappedPacket(const easnap::recording::MappedAudioPacket& left,
                      const easnap::recording::MappedAudioPacket& right) {
    return left.timestamp == right.timestamp &&
           left.duration == right.duration &&
           left.first_frame == right.first_frame &&
           left.frame_count == right.frame_count &&
           left.discontinuity == right.discontinuity &&
           left.qpc_precise == right.qpc_precise &&
           left.decision == right.decision &&
           left.qpc_error_100ns == right.qpc_error_100ns;
}

void TestTrimAudioOverlap() {
    using easnap::media::MediaTime;
    using easnap::recording::AudioPacketMapResult;
    using easnap::recording::TrimAudioOverlap;
    constexpr uint32_t k48kHz = 48'000;
    constexpr uint32_t k44kHz = 44'100;

    // Audio that starts at or after the written end is left alone.
    {
        auto mapped = OverlapPacket(10'000'000, 120, 960, 200'000);
        const auto original = mapped;
        CHECK(TrimAudioOverlap(mapped, MediaTime{10'000'000}, k48kHz) ==
              AudioPacketMapResult::Mapped);
        CHECK(SameMappedPacket(mapped, original));
        CHECK(TrimAudioOverlap(mapped, MediaTime{9'000'000}, k48kHz) ==
              AudioPacketMapResult::Mapped);
        CHECK(SameMappedPacket(mapped, original));
    }

    // 10 ms of overlap is exactly 480 frames at 48 kHz; only the frame window
    // and its timing change.
    {
        auto mapped = OverlapPacket(10'000'000, 120, 960, 200'000);
        CHECK(TrimAudioOverlap(mapped, MediaTime{10'100'000}, k48kHz) ==
              AudioPacketMapResult::Mapped);
        CHECK(mapped.first_frame == 600);
        CHECK(mapped.frame_count == 480);
        CHECK(mapped.timestamp == MediaTime{10'100'000});
        CHECK(mapped.duration == MediaTime{100'000});
        CHECK(mapped.discontinuity);
        CHECK(mapped.qpc_precise);
        CHECK(mapped.decision ==
              easnap::recording::AudioTimelineDecision::ClockCalibration);
        CHECK(mapped.qpc_error_100ns == -1'234);
    }

    // One tick of overlap costs a whole frame, and the rest starts at the
    // written end rather than on the 48 kHz frame grid.
    {
        auto mapped = OverlapPacket(10'000'000, 120, 960, 200'000);
        CHECK(TrimAudioOverlap(mapped, MediaTime{10'000'001}, k48kHz) ==
              AudioPacketMapResult::Mapped);
        CHECK(mapped.first_frame == 121);
        CHECK(mapped.frame_count == 959);
        CHECK(mapped.timestamp == MediaTime{10'000'001});
        CHECK(mapped.duration == MediaTime{199'791});
    }

    // An overlap of every frame, or more, drops the packet unchanged.
    {
        auto mapped = OverlapPacket(10'000'000, 120, 480, 100'000);
        const auto original = mapped;
        CHECK(TrimAudioOverlap(mapped, MediaTime{10'100'000}, k48kHz) ==
              AudioPacketMapResult::Dropped);
        CHECK(SameMappedPacket(mapped, original));
        CHECK(TrimAudioOverlap(mapped, MediaTime{10'200'000}, k48kHz) ==
              AudioPacketMapResult::Dropped);
        CHECK(SameMappedPacket(mapped, original));
    }

    // 99,791 ticks round up to 479 frames, which keeps the last frame.
    {
        auto mapped = OverlapPacket(10'000'000, 120, 480, 100'000);
        CHECK(TrimAudioOverlap(mapped, MediaTime{10'099'791}, k48kHz) ==
              AudioPacketMapResult::Mapped);
        CHECK(mapped.first_frame == 599);
        CHECK(mapped.frame_count == 1);
        CHECK(mapped.timestamp == MediaTime{10'099'791});
        CHECK(mapped.duration == MediaTime{208});
    }

    // A 44.1 kHz frame lasts 226.757 ticks: 227 ticks need two frames and
    // 100,001 ticks need 442.
    {
        auto mapped = OverlapPacket(10'000'000, 0, 1'000, 226'757);
        CHECK(TrimAudioOverlap(mapped, MediaTime{10'000'227}, k44kHz) ==
              AudioPacketMapResult::Mapped);
        CHECK(mapped.first_frame == 2);
        CHECK(mapped.frame_count == 998);
        CHECK(mapped.timestamp == MediaTime{10'000'227});
        CHECK(mapped.duration == MediaTime{226'303});

        auto longer = OverlapPacket(10'000'000, 0, 1'000, 226'757);
        CHECK(TrimAudioOverlap(longer, MediaTime{10'100'001}, k44kHz) ==
              AudioPacketMapResult::Mapped);
        CHECK(longer.first_frame == 442);
        CHECK(longer.frame_count == 558);
        CHECK(longer.timestamp == MediaTime{10'100'001});
        CHECK(longer.duration == MediaTime{126'530});
    }

    // A packet placed before zero is trimmed against a written end of zero.
    {
        auto mapped = OverlapPacket(-100'000, 0, 960, 200'000);
        CHECK(TrimAudioOverlap(mapped, MediaTime::zero(), k48kHz) ==
              AudioPacketMapResult::Mapped);
        CHECK(mapped.first_frame == 480);
        CHECK(mapped.frame_count == 480);
        CHECK(mapped.timestamp == MediaTime::zero());
        CHECK(mapped.duration == MediaTime{100'000});
    }

    // Without a sample rate, with a frame index past uint32_t or with an
    // overlap wider than MediaTime, the packet is invalid and unchanged.
    {
        auto mapped = OverlapPacket(10'000'000, 120, 960, 200'000);
        const auto original = mapped;
        CHECK(TrimAudioOverlap(mapped, MediaTime{10'100'000}, 0) ==
              AudioPacketMapResult::Invalid);
        CHECK(SameMappedPacket(mapped, original));

        auto unindexable = OverlapPacket(
            10'000'000, (std::numeric_limits<uint32_t>::max)() - 100, 960,
            200'000);
        const auto unindexable_original = unindexable;
        CHECK(TrimAudioOverlap(unindexable, MediaTime{10'100'000}, k48kHz) ==
              AudioPacketMapResult::Invalid);
        CHECK(SameMappedPacket(unindexable, unindexable_original));

        auto distant = OverlapPacket((std::numeric_limits<int64_t>::min)(), 0,
                                     960, 200'000);
        const auto distant_original = distant;
        CHECK(TrimAudioOverlap(distant, MediaTime::zero(), k48kHz) ==
              AudioPacketMapResult::Invalid);
        CHECK(SameMappedPacket(distant, distant_original));
    }
}

}  // namespace

int main() {
    TestLateFirstPacketPreservesInitialSilence();
    TestDelayedFallbackRunStaysAtTrueActiveTime();
    TestContinuousPacketsIgnoreQpcJitter();
    TestPauseResumeExcludesPausedWallTime();
    TestPacketCrossingSegmentAnchorIsTrimmed();
    TestDeviceGapStartsExplicitRun();
    TestResetClearsWakeupRunState();
    TestFallbackWakeAfterSparseTickStartsAtResolvedTime();
    TestSparseTickUsesCommittedNotMappedFrontier();
    TestDevicePositionResetReanchorsAtResolvedQpc();
    TestSequentialRecordingSessions();
    TestAudioWriterGateLimitSaturates();
    TestTrimAudioOverlap();
    if (g_fail == 0) {
        std::printf("PASSED  %d/%d tests\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
