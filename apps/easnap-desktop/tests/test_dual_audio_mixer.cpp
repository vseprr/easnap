#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "media/dual_audio_mixer.h"

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

using easnap::media::AudioFormat;
using easnap::media::AudioPacket;
using easnap::media::AudioPacketFlags;
using easnap::media::AudioTimestampSource;
using easnap::media::MediaTime;
using easnap::media::detail::DualAudioInput;
using easnap::media::detail::DualAudioMixer;
using easnap::media::detail::DualAudioMixerPushResult;

AudioPacket ConstantPacket(const AudioFormat& format, uint32_t frames,
                           int64_t timestamp, float value,
                           AudioTimestampSource timestamp_source =
                               AudioTimestampSource::WasapiQpc,
                           AudioPacketFlags flags = AudioPacketFlags::None) {
    AudioPacket packet;
    packet.format = format;
    packet.frame_count = frames;
    packet.timestamp = MediaTime{timestamp};
    packet.timestamp_source = timestamp_source;
    packet.flags = flags;
    std::vector<float> samples(
        static_cast<std::size_t>(frames) * format.channels, value);
    packet.data.resize(samples.size() * sizeof(float));
    std::memcpy(packet.data.data(), samples.data(), packet.data.size());
    return packet;
}

std::vector<float> Samples(const AudioPacket& packet) {
    std::vector<float> result(packet.data.size() / sizeof(float));
    std::memcpy(result.data(), packet.data.data(), packet.data.size());
    return result;
}

void TestCanonicalFormatAndConversionBoundary() {
    AudioFormat native_system{44'100, 2, 32, true, 0x3};
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    CHECK(format.sample_rate == 48'000);
    CHECK(format.channels == 2);
    CHECK(format.bits_per_sample == 32);
    CHECK(format.is_float);
    CHECK(format.channel_mask == 0x3);

    // Native 44.1 kHz mono and 48 kHz stereo endpoints both target this one
    // format through the Windows Audio Resampler DSP before reaching the mixer.
    AudioFormat native_microphone{44'100, 1, 16, false, 0x4};
    CHECK(native_microphone.valid());
    CHECK(native_system.valid());
    DualAudioMixer mixer(format);
    CHECK(mixer.valid());
    CHECK(mixer.Push(DualAudioInput::Microphone,
                     ConstantPacket(native_microphone, 441, 0, 0.5F)) ==
          DualAudioMixerPushResult::InvalidPacket);
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 480, 0, 0.5F)) ==
          DualAudioMixerPushResult::Success);
}

void TestStartAlignmentSilenceAndSimultaneousMix() {
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    DualAudioMixer mixer(format);
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 1'440, 0, 0.8F)) ==
          DualAudioMixerPushResult::Success);
    CHECK(mixer.Push(DualAudioInput::Microphone,
                     ConstantPacket(format, 480, 100'000, 0.4F)) ==
          DualAudioMixerPushResult::Success);

    AudioPacket first;
    CHECK(mixer.PopUntil(MediaTime{100'000}, first));
    const auto first_samples = Samples(first);
    CHECK(std::all_of(first_samples.begin(), first_samples.end(),
                      [](float value) { return std::abs(value - 0.4F) < 0.0001F; }));

    AudioPacket second;
    CHECK(mixer.PopUntil(MediaTime{200'000}, second));
    const auto second_samples = Samples(second);
    CHECK(std::all_of(second_samples.begin(), second_samples.end(),
                      [](float value) { return std::abs(value - 0.6F) < 0.0001F; }));

    AudioPacket third;
    CHECK(mixer.PopUntil(MediaTime{300'000}, third));
    const auto third_samples = Samples(third);
    CHECK(std::all_of(third_samples.begin(), third_samples.end(),
                      [](float value) { return std::abs(value - 0.4F) < 0.0001F; }));
}

void TestTemporarySilenceAndHeadroom() {
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    DualAudioMixer mixer(format);
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 480, 0, 1.0F)) ==
          DualAudioMixerPushResult::Success);
    CHECK(mixer.Push(DualAudioInput::Microphone,
                     ConstantPacket(format, 480, 0, 1.0F)) ==
          DualAudioMixerPushResult::Success);
    AudioPacket mixed;
    CHECK(mixer.PopUntil(MediaTime{100'000}, mixed));
    const auto samples = Samples(mixed);
    CHECK(std::all_of(samples.begin(), samples.end(),
                      [](float value) { return value == 1.0F; }));

    // Neither endpoint contributes the next interval: the mixer emits an
    // explicit silent packet instead of stalling or borrowing prior samples.
    AudioPacket silent;
    CHECK(mixer.PopUntil(MediaTime{200'000}, silent));
    CHECK(easnap::media::HasFlag(silent.flags, AudioPacketFlags::Silent));
    const auto silence = Samples(silent);
    CHECK(std::all_of(silence.begin(), silence.end(),
                      [](float value) { return value == 0.0F; }));
}

void TestIndependentCadenceAndClockDrift() {
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    DualAudioMixer mixer(format);

    // Different packet cadences and a one-frame QPC drift are placed from
    // their own timestamps rather than arrival order or a shared device clock.
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 960, 0, 0.6F)) ==
          DualAudioMixerPushResult::Success);
    CHECK(mixer.Push(DualAudioInput::Microphone,
                     ConstantPacket(format, 240, 0, 0.2F)) ==
          DualAudioMixerPushResult::Success);
    CHECK(mixer.Push(DualAudioInput::Microphone,
                     ConstantPacket(format, 240, 50'208, 0.2F)) ==
          DualAudioMixerPushResult::Success);
    CHECK(mixer.Push(DualAudioInput::Microphone,
                     ConstantPacket(format, 480, 100'208, 0.2F)) ==
          DualAudioMixerPushResult::Success);

    AudioPacket packet;
    CHECK(mixer.PopUntil(MediaTime{100'000}, packet));
    CHECK(mixer.PopUntil(MediaTime{200'000}, packet));
    CHECK(packet.timestamp == MediaTime{100'000});
    CHECK(packet.frame_count == 480);
    CHECK(mixer.diagnostics().frames_dropped_late == 0);
}

void TestPauseResetAndFallbackCadence() {
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    DualAudioMixer mixer(format);
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 480, 0, 0.4F,
                                    AudioTimestampSource::CaptureQpcFallback)) ==
          DualAudioMixerPushResult::Success);
    // Callback jitter must not create a gap while fallback cadence is active.
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 480, 130'000, 0.4F,
                                    AudioTimestampSource::CaptureQpcFallback)) ==
          DualAudioMixerPushResult::Success);
    AudioPacket packet;
    CHECK(mixer.PopUntil(MediaTime{200'000}, packet));
    CHECK(mixer.PopUntil(MediaTime{200'000}, packet));

    mixer.Reset();
    CHECK(mixer.buffered_frames() == 0);
    CHECK(mixer.Push(DualAudioInput::Microphone,
                     ConstantPacket(format, 480, 50'000'000, 0.2F,
                                    AudioTimestampSource::WasapiQpc,
                                    AudioPacketFlags::Discontinuity)) ==
          DualAudioMixerPushResult::Success);
    CHECK(mixer.PopUntil(MediaTime{50'100'000}, packet));
    CHECK(packet.timestamp == MediaTime{50'000'000});
    CHECK(easnap::media::HasFlag(packet.flags,
                                 AudioPacketFlags::Discontinuity));
}

void TestBoundedCapacity() {
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    constexpr uint32_t capacity = 960;
    DualAudioMixer mixer(format, capacity, 480);
    const DualAudioMixerPushResult result = mixer.Push(
        DualAudioInput::SystemAudio,
        ConstantPacket(format, capacity * 2, 0, 0.25F));
    CHECK(result == DualAudioMixerPushResult::TruncatedOverflow);
    CHECK(mixer.buffered_frames() <= capacity);
    CHECK(mixer.diagnostics().peak_buffered_frames <= capacity);
    CHECK(mixer.diagnostics().frames_dropped_overflow == capacity);
    CHECK(mixer.diagnostics().capacity_failures == 1);
}

void TestMicrophoneContinuityUnderPacketJitter() {
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    DualAudioMixer mixer(format, 24'000, 480);

    // Push 30 consecutive microphone packets of 480 frames (10 ms) with artificial
    // ±1.5 ms QPC timestamp jitter at packet boundaries.
    // In a continuous run, the mixer must lock onto sample-count continuity
    // so NO frames are skipped, NO silence gaps are injected, and NO clicks occur.
    for (uint32_t index = 0; index < 30; ++index) {
        // Deterministic pseudo-jitter between -15,000 and +15,000 hundred-nanoseconds (-1.5 to +1.5 ms)
        const int64_t jitter = (static_cast<int64_t>(index % 5) - 2) * 7'500;
        const int64_t nominal_time = static_cast<int64_t>(index) * 100'000;
        const int64_t jittered_time = (std::max)(int64_t{0}, nominal_time + jitter);
        const auto push_result = mixer.Push(
            DualAudioInput::Microphone,
            ConstantPacket(format, 480, jittered_time, 0.75F));
        CHECK(push_result == DualAudioMixerPushResult::Success);

        // Periodically pop chunks so mixer buffer stays bounded
        if (index >= 10) {
            const MediaTime watermark{static_cast<int64_t>(index - 5) * 100'000};
            AudioPacket chunk;
            while (mixer.PopUntil(watermark, chunk)) {
                CHECK(chunk.frame_count == 480);
                const auto samples = Samples(chunk);
                // The mixed value of 0.75F microphone with silent system audio is 0.375F.
                // Every single sample in the continuous run must equal 0.375F:
                // absolutely ZERO 0.0F silence gaps or corrupted samples!
                for (float s : samples) {
                    CHECK(std::abs(s - 0.375F) < 0.0001F);
                }
            }
        }
    }
    CHECK(mixer.diagnostics().frames_dropped_late == 0);
    CHECK(mixer.diagnostics().capacity_failures == 0);
    CHECK(mixer.diagnostics().invalid_packets == 0);
}

void TestLateSystemAudioJoiningActiveMicrophone() {
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    DualAudioMixer mixer(format, 24'000, 480);

    // 1. Start with Microphone active from time 0: push 10 packets (100 ms).
    for (uint32_t i = 0; i < 10; ++i) {
        CHECK(mixer.Push(DualAudioInput::Microphone,
                         ConstantPacket(format, 480, i * 100'000, 0.4F)) ==
              DualAudioMixerPushResult::Success);
    }

    // 2. Pop all mixed chunks up to 100 ms (System Audio was silent for this duration).
    AudioPacket chunk;
    uint32_t popped_chunks = 0;
    while (mixer.PopUntil(MediaTime{1'000'000}, chunk)) {
        ++popped_chunks;
        CHECK(chunk.frame_count == 480);
        // Half of microphone 0.4F is 0.2F
        const auto samples = Samples(chunk);
        for (float s : samples) {
            CHECK(std::abs(s - 0.2F) < 0.0001F);
        }
    }
    CHECK(popped_chunks == 10);

    // 3. Now System Audio wakes up late with an obsolete packet covering 50 ms - 60 ms
    // (which has already elapsed and been emitted as mixed silence).
    const auto late_result = mixer.Push(
        DualAudioInput::SystemAudio,
        ConstantPacket(format, 480, 500'000, 0.8F));
    CHECK(late_result == DualAudioMixerPushResult::LateDiscarded);
    CHECK(mixer.diagnostics().packets_dropped_late == 1);
    CHECK(mixer.diagnostics().frames_dropped_late == 480);
    CHECK(mixer.diagnostics().capacity_failures == 0);
    CHECK(mixer.diagnostics().invalid_packets == 0);

    // 4. Now System Audio sends a current packet at 100 ms (matching current timeline).
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 480, 1'000'000, 0.8F)) ==
          DualAudioMixerPushResult::Success);
    CHECK(mixer.Push(DualAudioInput::Microphone,
                     ConstantPacket(format, 480, 1'000'000, 0.4F)) ==
          DualAudioMixerPushResult::Success);

    // 5. Pop and verify both sources are now cleanly mixed: 0.5 * 0.8 + 0.5 * 0.4 = 0.6.
    CHECK(mixer.PopUntil(MediaTime{1'100'000}, chunk));
    const auto mixed_samples = Samples(chunk);
    for (float s : mixed_samples) {
        CHECK(std::abs(s - 0.6F) < 0.0001F);
    }
}

void TestSourceSilentActiveSilentActiveTransitions() {
    const AudioFormat format{48'000, 2, 32, true, 0x3};
    DualAudioMixer mixer(format, 24'000, 480);

    // System Audio active: packet 0 at t=0 (480 frames = 10 ms)
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 480, 0, 0.6F)) ==
          DualAudioMixerPushResult::Success);
    AudioPacket chunk;
    CHECK(mixer.PopUntil(MediaTime{100'000}, chunk));
    const auto initial_samples = Samples(chunk);
    for (float s : initial_samples) {
        CHECK(std::abs(s - 0.3F) < 0.0001F);
    }

    // System Audio goes silent for 200 ms (gap of 2,000,000 hundred-nanoseconds).
    // Now packet 1 arrives at t = 200 ms: exceeds 50 ms continuity tolerance, so it re-anchors.
    CHECK(mixer.Push(DualAudioInput::SystemAudio,
                     ConstantPacket(format, 480, 2'000'000, 0.6F)) ==
          DualAudioMixerPushResult::Success);

    // Pop the silence gap up to 200 ms
    AudioPacket silent_chunk;
    uint32_t silent_count = 0;
    while (mixer.PopUntil(MediaTime{2'000'000}, silent_chunk)) {
        ++silent_count;
        CHECK(easnap::media::HasFlag(silent_chunk.flags, AudioPacketFlags::Silent));
    }
    CHECK(silent_count == 19);

    // Pop at 200 ms: must have the re-anchored packet
    CHECK(mixer.PopUntil(MediaTime{2'100'000}, chunk));
    const auto samples = Samples(chunk);
    for (float s : samples) {
        CHECK(std::abs(s - 0.3F) < 0.0001F);
    }
}

}  // namespace

int main() {
    TestCanonicalFormatAndConversionBoundary();
    TestStartAlignmentSilenceAndSimultaneousMix();
    TestTemporarySilenceAndHeadroom();
    TestIndependentCadenceAndClockDrift();
    TestPauseResetAndFallbackCadence();
    TestBoundedCapacity();
    TestMicrophoneContinuityUnderPacketJitter();
    TestLateSystemAudioJoiningActiveMicrophone();
    TestSourceSilentActiveSilentActiveTransitions();
    std::printf("dual_audio_mixer: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
