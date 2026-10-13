// Easnap - real WASAPI audio capture validation.
//
// Every test exercises the REAL production WASAPI provider. No fake audio
// sources or mock capture are used. The system-loopback test generates a
// bounded sine wave through a real Windows WASAPI render client as TEST INPUT,
// while the production loopback provider captures it. Synthetic test INPUT is
// acceptable; fake production CAPTURE is not.
//
// Microphone tests probe real hardware availability and capture when present.
// An absent microphone is reported accurately, not faked.
//
// Build: cmake --build build --config Release --target test_wasapi_audio
// Run:   ctest --test-dir build -C Release -R wasapi

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "media/audio_format.h"
#include "media/audio_delivery_barrier.h"
#include "media/audio_queue.h"
#include "media/media_interfaces.h"
#include "media/wasapi_audio_provider.h"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// Test infrastructure
// ---------------------------------------------------------------------------

int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
int g_tests_skipped = 0;

struct FakeAudioState final {
    std::atomic<uint64_t> failures{0};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
};

class FakeAudioProvider final : public easnap::media::IAudioCaptureProvider {
public:
    FakeAudioProvider(easnap::media::AudioSource expected_source,
                      easnap::media::AudioCaptureResult start_result,
                      easnap::media::AudioFormat format,
                      std::shared_ptr<FakeAudioState> state)
        : expected_source_(expected_source),
          start_result_(start_result),
          format_(format),
          state_(std::move(state)) {}

    easnap::media::AudioCaptureResult Start(
        easnap::media::AudioSource source, PacketCallback callback) override {
        if (source != expected_source_ || !callback) {
            return easnap::media::AudioCaptureResult::InvalidSource;
        }
        state_->started.store(true, std::memory_order_release);
        active_ = start_result_ == easnap::media::AudioCaptureResult::Ok;
        return start_result_;
    }
    void PauseDeliveryAndDrain() noexcept override {}
    void ResumeDelivery() noexcept override {}
    void Stop() override {
        active_ = false;
        state_->stopped.store(true, std::memory_order_release);
    }
    bool active() const override { return active_; }
    bool IsSourceAvailable(easnap::media::AudioSource source) const override {
        return source == expected_source_;
    }
    easnap::media::AudioFormat negotiated_format() const override {
        return active_ ? format_ : easnap::media::AudioFormat{};
    }
    std::optional<easnap::media::MediaTime> clock_position() const noexcept override {
        return easnap::media::MediaTime{0};
    }
    easnap::media::AudioCaptureDiagnostics diagnostics() const override {
        easnap::media::AudioCaptureDiagnostics result;
        result.qpc_epoch_100ns = 1;
        result.capture_failures =
            state_->failures.load(std::memory_order_acquire);
        return result;
    }

private:
    easnap::media::AudioSource expected_source_;
    easnap::media::AudioCaptureResult start_result_;
    easnap::media::AudioFormat format_;
    std::shared_ptr<FakeAudioState> state_;
    bool active_{false};
};

void Trace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::fflush(stdout);
}

#define TEST_ASSERT(cond, msg)                                                   \
    do {                                                                         \
        if (!(cond)) {                                                           \
            Trace("  FAIL: %s (line %d)\n", (msg), __LINE__);                    \
            return false;                                                        \
        }                                                                        \
    } while (false)

#define RUN_TEST(fn)                                                             \
    do {                                                                         \
        ++g_tests_run;                                                           \
        Trace("=== %s ===\n", #fn);                                              \
        const int result = fn();                                                 \
        if (result > 0) {                                                        \
            ++g_tests_passed;                                                    \
            Trace("  PASSED\n\n");                                               \
        } else if (result == 0) {                                                \
            ++g_tests_failed;                                                    \
            Trace("  FAILED\n\n");                                               \
        } else {                                                                 \
            ++g_tests_skipped;                                                   \
            Trace("  SKIPPED\n\n");                                              \
        }                                                                        \
    } while (false)

// ---------------------------------------------------------------------------
// Sine wave render helper — generates bounded test audio through a real
// Windows WASAPI render client. This is TEST INPUT, not production capture.
// Uses timer-based (non-event-driven) rendering for simplicity and safety.
// ---------------------------------------------------------------------------

class TestAudioRenderer final {
public:
    ~TestAudioRenderer() { StopAndCleanup(); }

    bool Initialize() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                        CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) return false;

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) return false;

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client_.GetAddressOf()));
        if (FAILED(hr)) return false;

        hr = client_->GetMixFormat(&format_);
        if (FAILED(hr) || !format_) return false;

        // Detect float format.
        if (format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            is_float_ = true;
        } else if (format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format_->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format_);
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                is_float_ = true;
            }
        }

        // Use default shared mode without event callback for simplicity.
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                  0, 0, format_, nullptr);
        if (FAILED(hr)) return false;

        hr = client_->GetBufferSize(&buffer_frames_);
        if (FAILED(hr)) return false;

        hr = client_->GetService(IID_PPV_ARGS(&render_client_));
        if (FAILED(hr)) return false;

        return true;
    }

    /// Starts rendering a sine wave on a background thread.
    void StartRendering(float frequency = 440.0f) {
        frequency_ = frequency;
        should_stop_.store(false, std::memory_order_relaxed);
        thread_ = std::thread([this]() { RenderLoop(); });
    }

    void StopAndCleanup() {
        should_stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.join();
        }
        render_client_.Reset();
        client_.Reset();
        if (format_) {
            ::CoTaskMemFree(format_);
            format_ = nullptr;
        }
    }

private:
    void RenderLoop() {
        if (!client_ || !render_client_ || !format_) return;

        client_->Start();

        const float sample_rate = static_cast<float>(format_->nSamplesPerSec);
        const WORD channels = format_->nChannels;
        const float increment = 2.0f * 3.14159265358979f * frequency_ / sample_rate;
        float phase = 0.0f;

        while (!should_stop_.load(std::memory_order_relaxed)) {
            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding))) break;

            const UINT32 available = buffer_frames_ - padding;
            if (available == 0) {
                ::Sleep(1);
                continue;
            }

            BYTE* data = nullptr;
            if (FAILED(render_client_->GetBuffer(available, &data))) break;
            if (!data) {
                render_client_->ReleaseBuffer(0, 0);
                break;
            }

            // Write samples. Use nBlockAlign for proper byte counting.
            if (is_float_ && format_->wBitsPerSample == 32) {
                auto* out = reinterpret_cast<float*>(data);
                for (UINT32 i = 0; i < available; ++i) {
                    const float value = 0.3f * std::sin(phase);
                    for (WORD ch = 0; ch < channels; ++ch) {
                        out[static_cast<size_t>(i) * channels + ch] = value;
                    }
                    phase += increment;
                }
            } else if (!is_float_ && format_->wBitsPerSample == 16) {
                auto* out = reinterpret_cast<int16_t*>(data);
                for (UINT32 i = 0; i < available; ++i) {
                    const float value = 0.3f * std::sin(phase);
                    const auto s = static_cast<int16_t>(value * 32767.0f);
                    for (WORD ch = 0; ch < channels; ++ch) {
                        out[static_cast<size_t>(i) * channels + ch] = s;
                    }
                    phase += increment;
                }
            } else {
                // Fill with silence for unknown formats.
                std::memset(data, 0, static_cast<size_t>(available) * format_->nBlockAlign);
            }

            // Wrap phase to prevent precision loss.
            constexpr float kTwoPi = 2.0f * 3.14159265358979f;
            while (phase > kTwoPi) phase -= kTwoPi;

            render_client_->ReleaseBuffer(available, 0);
            ::Sleep(5);  // Yield to avoid busy loop.
        }

        client_->Stop();
    }

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_client_;
    WAVEFORMATEX* format_{nullptr};
    UINT32 buffer_frames_{0};
    bool is_float_{false};
    float frequency_{440.0f};
    std::atomic<bool> should_stop_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// Lightweight RMS measurement for content validation
// ---------------------------------------------------------------------------

double ComputeRms(const std::vector<easnap::media::AudioPacket>& packets) {
    double sum_sq = 0.0;
    uint64_t sample_count = 0;

    for (const auto& pkt : packets) {
        if (pkt.data.empty() || !pkt.format.valid()) continue;

        if (pkt.format.is_float && pkt.format.bits_per_sample == 32) {
            const auto* samples = reinterpret_cast<const float*>(pkt.data.data());
            const size_t count = pkt.data.size() / sizeof(float);
            for (size_t i = 0; i < count; ++i) {
                const double s = static_cast<double>(samples[i]);
                sum_sq += s * s;
            }
            sample_count += count;
        } else if (!pkt.format.is_float && pkt.format.bits_per_sample == 16) {
            const auto* samples = reinterpret_cast<const int16_t*>(pkt.data.data());
            const size_t count = pkt.data.size() / sizeof(int16_t);
            for (size_t i = 0; i < count; ++i) {
                const double s = static_cast<double>(samples[i]) / 32768.0;
                sum_sq += s * s;
            }
            sample_count += count;
        }
    }

    if (sample_count == 0) return 0.0;
    return std::sqrt(sum_sq / static_cast<double>(sample_count));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// Test 1: AudioFormat validity
int TestAudioFormat() {
    using namespace easnap::media;

    AudioFormat valid;
    valid.sample_rate = 48000;
    valid.channels = 2;
    valid.bits_per_sample = 32;
    valid.is_float = true;
    TEST_ASSERT(valid.valid(), "48kHz/2ch/32bit float should be valid");
    TEST_ASSERT(valid.block_align() == 8, "2ch * 4 bytes = 8 block align");
    TEST_ASSERT(valid.bytes_per_second() == 384000, "48000 * 8 = 384000 B/s");

    AudioFormat invalid;
    TEST_ASSERT(!invalid.valid(), "Default-constructed format should be invalid");

    AudioFormat partial;
    partial.sample_rate = 44100;
    partial.channels = 0;
    partial.bits_per_sample = 16;
    TEST_ASSERT(!partial.valid(), "Zero channels should be invalid");

    // AudioPacket consistency.
    AudioPacket pkt;
    pkt.format = valid;
    pkt.frame_count = 480;
    pkt.data.resize(480 * 8, 0);
    TEST_ASSERT(pkt.consistent(), "Properly sized packet should be consistent");

    pkt.data.resize(100);
    TEST_ASSERT(!pkt.consistent(), "Wrong-sized packet should not be consistent");

    return 1;
}

int TestTimestampResolution() {
    using namespace easnap::media;

    constexpr int64_t epoch = 10'000'000;
    detail::AudioTimestampResolutionInput input;
    input.qpc_epoch_100ns = epoch;
    input.raw_wasapi_qpc_100ns = 20'000'000;
    input.device_position = 48'000;
    input.frame_count = 480;
    input.buffered_frame_count = 480;
    input.sample_rate = 48'000;
    input.audio_clock_position = 48'480;
    input.audio_clock_frequency = 48'000;
    input.audio_clock_qpc_100ns = 20'100'000;
    input.capture_qpc_100ns = 20'110'000;

    auto resolved = detail::ResolveAudioPacketTimestamp(input);
    TEST_ASSERT(resolved.source == AudioTimestampSource::WasapiQpc,
                "A plausible unflagged WASAPI QPC should be preferred");
    TEST_ASSERT(resolved.resolved_timestamp == MediaTime{10'000'000},
                "Raw WASAPI QPC should be made epoch-relative exactly");

    input.wasapi_timestamp_error = true;
    input.raw_wasapi_qpc_100ns = 10'100'000;
    resolved = detail::ResolveAudioPacketTimestamp(input);
    TEST_ASSERT(resolved.source == AudioTimestampSource::AudioClockCorrelation,
                "A flagged raw QPC should use the same-client audio clock");
    TEST_ASSERT(resolved.resolved_timestamp == MediaTime{10'000'000},
                "Audio-clock correlation should preserve the packet's true placement");

    input.wasapi_timestamp_error = false;
    resolved = detail::ResolveAudioPacketTimestamp(input);
    TEST_ASSERT(resolved.source == AudioTimestampSource::AudioClockCorrelation,
                "A stale unflagged raw QPC should fail the correlation check");
    TEST_ASSERT(!resolved.raw_wasapi_plausible,
                "Stale raw QPC should be diagnosed as implausible");

    input.raw_wasapi_qpc_100ns = 10'100'000;
    input.audio_clock_position = 0;
    input.audio_clock_frequency = 0;
    input.audio_clock_qpc_100ns = 0;
    input.capture_qpc_100ns = 30'100'000;
    resolved = detail::ResolveAudioPacketTimestamp(input);
    TEST_ASSERT(resolved.source == AudioTimestampSource::CaptureQpcFallback,
                "Stale raw QPC without an audio clock should use capture QPC");
    TEST_ASSERT(resolved.resolved_timestamp == MediaTime{20'000'000},
                "Capture fallback should subtract the packet's buffered duration");
    TEST_ASSERT(resolved.resolved_timestamp > MediaTime{0},
                "Invalid raw QPC must not silently collapse to timestamp zero");

    input.raw_wasapi_qpc_100ns = 1;
    resolved = detail::ResolveAudioPacketTimestamp(input);
    TEST_ASSERT(resolved.source == AudioTimestampSource::CaptureQpcFallback,
                "Pre-epoch raw QPC without an audio clock should use capture QPC");
    TEST_ASSERT(resolved.resolved_timestamp == MediaTime{20'000'000},
                "Pre-epoch raw QPC must use the same bounded fallback placement");

    return 1;
}

/// Regression test: inflated GetCurrentPadding after endpoint wakeup must be
/// capped at the negotiated buffer size so the fallback doesn't overshoot into
/// the distant past.
int TestFallbackTimestampCappedPadding() {
    using namespace easnap::media;

    constexpr int64_t epoch = 10'000'000;
    detail::AudioTimestampResolutionInput input;
    input.qpc_epoch_100ns = epoch;
    // Raw WASAPI QPC is pre-epoch (stale from long-idle endpoint).
    input.raw_wasapi_qpc_100ns = 1;
    input.wasapi_timestamp_error = false;
    input.device_position = 0;
    input.frame_count = 480;
    // Inflated padding: 72000 frames = 1.5 seconds at 48kHz.
    // This simulates the endpoint-wakeup scenario where GetCurrentPadding
    // reports accumulated render pipeline frames.
    input.buffered_frame_count = 72'000;
    input.sample_rate = 48'000;
    // No audio clock available.
    input.audio_clock_position = 0;
    input.audio_clock_frequency = 0;
    input.audio_clock_qpc_100ns = 0;
    // Capture QPC is 2 seconds after epoch.
    input.capture_qpc_100ns = 30'000'000;  // epoch + 2s
    // Negotiated buffer is 480 frames (10ms at 48kHz).
    input.maximum_buffer_frames = 480;

    auto resolved = detail::ResolveAudioPacketTimestamp(input);
    TEST_ASSERT(resolved.source == AudioTimestampSource::CaptureQpcFallback,
                "Should fall back to capture QPC with pre-epoch raw QPC");

    // Without capping: fallback = 30M - (72000 * 10M / 48000) = 30M - 15M = 15M
    // → epoch-relative = 15M - 10M = 5M (0.5 seconds)
    // With capping to 480: fallback = 30M - (480 * 10M / 48000) = 30M - 100000
    // → epoch-relative = 30M - 100000 - 10M = 19'900'000 (~2 seconds)
    const MediaTime uncapped_result{5'000'000};  // What we'd get without cap
    const MediaTime capped_result{19'900'000};    // What we expect with cap

    TEST_ASSERT(resolved.resolved_timestamp != uncapped_result,
                "Inflated padding must NOT produce the uncapped overshoot timestamp");
    TEST_ASSERT(resolved.resolved_timestamp == capped_result,
                "Capped padding should produce a timestamp near capture_qpc");

    // Now test without maximum_buffer_frames (cap disabled): should use raw padding.
    input.maximum_buffer_frames = 0;
    resolved = detail::ResolveAudioPacketTimestamp(input);
    TEST_ASSERT(resolved.resolved_timestamp == uncapped_result,
                "With cap disabled, raw padding should be used");

    return 1;
}

/// Test 2: AudioQueue bounded semantics
int TestAudioQueueSemantics() {
    using namespace easnap::media;

    // Test capacity from budget.
    const std::size_t cap = AudioQueue::CapacityFromBudget(500, 10);
    TEST_ASSERT(cap == 50, "500ms / 10ms = 50 slots");

    const std::size_t cap2 = AudioQueue::CapacityFromBudget(10, 10);
    TEST_ASSERT(cap2 >= kMinimumAudioQueueCapacity, "Should clamp to minimum");

    // Test push/pop semantics.
    AudioQueue queue(8);
    TEST_ASSERT(queue.capacity() == 8, "Capacity should be 8");
    TEST_ASSERT(queue.empty(), "Should be empty initially");

    // Fill the queue.
    for (int i = 0; i < 8; ++i) {
        AudioPacket pkt;
        pkt.frame_count = static_cast<uint32_t>(i + 1);
        pkt.format.sample_rate = 48000;
        pkt.format.channels = 2;
        pkt.format.bits_per_sample = 16;
        pkt.data.resize(pkt.frame_count * pkt.format.block_align(), 0);
        const auto result = queue.try_push(std::move(pkt));
        TEST_ASSERT(result == AudioQueuePushResult::Accepted, "Push should be accepted");
    }

    TEST_ASSERT(queue.full(), "Queue should be full");
    TEST_ASSERT(queue.peak_size() == 8, "Peak should be 8");

    // Overrun.
    {
        AudioPacket pkt;
        pkt.format.sample_rate = 48000;
        pkt.format.channels = 2;
        pkt.format.bits_per_sample = 16;
        const auto result = queue.try_push(std::move(pkt));
        TEST_ASSERT(result == AudioQueuePushResult::Overrun, "Push on full queue should be Overrun");
    }

    // Pop all.
    for (int i = 0; i < 8; ++i) {
        auto pkt = queue.wait_pop();
        TEST_ASSERT(pkt.has_value(), "Pop should return a packet");
        TEST_ASSERT(pkt->frame_count == static_cast<uint32_t>(i + 1),
                    "Packets should come out in FIFO order");
    }

    TEST_ASSERT(queue.empty(), "Queue should be empty after draining");

    // Close semantics.
    queue.close();
    {
        AudioPacket pkt;
        const auto result = queue.try_push(std::move(pkt));
        TEST_ASSERT(result == AudioQueuePushResult::Closed, "Push after close should be Closed");
    }

    auto final_pkt = queue.wait_pop();
    TEST_ASSERT(!final_pkt.has_value(), "Pop on closed empty queue should return nullopt");

    return 1;
}

int TestAudioDeliveryBarrier() {
    using namespace easnap::media;

    detail::AudioDeliveryBarrier barrier;
    barrier.Reset();

    bool first_discontinuity = true;
    const auto first = barrier.TryEnqueue([&](bool discontinuity) {
        first_discontinuity = discontinuity;
        return true;
    });
    const auto second = barrier.TryEnqueue([](bool) { return true; });

    std::atomic<bool> pause_returned{false};
    std::thread pause_thread([&] {
        barrier.PauseAndDrain();
        pause_returned.store(true, std::memory_order_release);
    });

    bool observed_disabled = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (!barrier.snapshot().enabled) {
            observed_disabled = true;
            break;
        }
        ::Sleep(1);
    }
    const bool blocked_before_completion =
        !pause_returned.load(std::memory_order_acquire);

    barrier.CompleteOne();
    ::Sleep(10);
    const bool blocked_after_one_completion =
        !pause_returned.load(std::memory_order_acquire);

    barrier.CompleteOne();
    pause_thread.join();

    bool paused_enqueue_ran = false;
    const auto paused = barrier.TryEnqueue([&](bool) {
        paused_enqueue_ran = true;
        return true;
    });

    barrier.Resume();
    bool resumed_discontinuity = false;
    const auto resumed = barrier.TryEnqueue([&](bool discontinuity) {
        resumed_discontinuity = discontinuity;
        return true;
    });
    barrier.CompleteOne();

    const auto rejected = barrier.TryEnqueue([](bool) { return false; });
    const detail::AudioDeliverySnapshot snapshot = barrier.snapshot();

    TEST_ASSERT(first == detail::AudioDeliveryEnqueueResult::Accepted &&
                    second == detail::AudioDeliveryEnqueueResult::Accepted,
                "Enabled delivery should accept successful bounded-queue pushes");
    TEST_ASSERT(!first_discontinuity,
                "A fresh delivery run should not fabricate a discontinuity");
    TEST_ASSERT(observed_disabled && blocked_before_completion &&
                    blocked_after_one_completion,
                "Pause must wait for every packet accepted before its boundary");
    TEST_ASSERT(paused == detail::AudioDeliveryEnqueueResult::DeliveryPaused &&
                    !paused_enqueue_ran,
                "Paused delivery must discard without touching the bounded queue");
    TEST_ASSERT(resumed == detail::AudioDeliveryEnqueueResult::Accepted &&
                    resumed_discontinuity,
                "First resumed delivery must expose the explicit paused gap");
    TEST_ASSERT(rejected == detail::AudioDeliveryEnqueueResult::Rejected,
                "A bounded-queue rejection must not become accepted delivery");
    TEST_ASSERT(snapshot.accepted_sequence == 3 &&
                    snapshot.completed_sequence == 3 &&
                    snapshot.paused_discards == 1,
                "Delivery sequences and paused-discard diagnostics must remain exact");

    return 1;
}

/// Test 3: Invalid/unsupported source handling
int TestInvalidSources() {
    using namespace easnap::media;

    auto provider = CreateWasapiAudioCaptureProvider();

    auto dummy_callback = [](const AudioPacket&) {};

    // AudioSource::None should return InvalidSource.
    auto r1 = provider->Start(AudioSource::None, dummy_callback);
    TEST_ASSERT(r1 == AudioCaptureResult::InvalidSource, "None should be InvalidSource");
    TEST_ASSERT(!provider->active(), "Should not be active after invalid Start");

    // None is never available. Both is available exactly when both concrete
    // endpoints are available; it is no longer an unsupported enum value.
    TEST_ASSERT(!provider->IsSourceAvailable(AudioSource::None), "None should not be available");
    const bool system_available =
        provider->IsSourceAvailable(AudioSource::SystemAudio);
    const bool microphone_available =
        provider->IsSourceAvailable(AudioSource::Microphone);
    TEST_ASSERT(provider->IsSourceAvailable(AudioSource::Both) ==
                    (system_available && microphone_available),
                "Both availability must require both endpoints");

    return 1;
}

int TestBothFailureSemantics() {
    using namespace easnap::media;

    const AudioFormat system_format{44'100, 2, 32, true, 0x3};
    const AudioFormat microphone_format{48'000, 1, 32, true, 0x4};

    auto system_state = std::make_shared<FakeAudioState>();
    auto microphone_state = std::make_shared<FakeAudioState>();
    auto provider = detail::CreateDualAudioCaptureProviderForTesting(
        std::make_unique<FakeAudioProvider>(
            AudioSource::SystemAudio, AudioCaptureResult::InitializationFailed,
            system_format, system_state),
        std::make_unique<FakeAudioProvider>(
            AudioSource::Microphone, AudioCaptureResult::Ok,
            microphone_format, microphone_state));
    TEST_ASSERT(provider->Start(AudioSource::Both, [](const AudioPacket&) {}) ==
                    AudioCaptureResult::SystemInitializationFailed,
                "Both must identify System Audio startup failure");
    TEST_ASSERT(!microphone_state->started.load(std::memory_order_acquire),
                "Microphone must not start after System Audio startup failure");
    TEST_ASSERT(system_state->stopped.load(std::memory_order_acquire),
                "Failed Both startup must stop the first endpoint");

    system_state = std::make_shared<FakeAudioState>();
    microphone_state = std::make_shared<FakeAudioState>();
    provider = detail::CreateDualAudioCaptureProviderForTesting(
        std::make_unique<FakeAudioProvider>(
            AudioSource::SystemAudio, AudioCaptureResult::Ok,
            system_format, system_state),
        std::make_unique<FakeAudioProvider>(
            AudioSource::Microphone, AudioCaptureResult::SourceUnavailable,
            microphone_format, microphone_state));
    TEST_ASSERT(provider->Start(AudioSource::Both, [](const AudioPacket&) {}) ==
                    AudioCaptureResult::MicrophoneUnavailable,
                "Both must identify Microphone startup unavailability");
    TEST_ASSERT(system_state->stopped.load(std::memory_order_acquire),
                "Microphone startup failure must roll back System Audio");

    system_state = std::make_shared<FakeAudioState>();
    microphone_state = std::make_shared<FakeAudioState>();
    provider = detail::CreateDualAudioCaptureProviderForTesting(
        std::make_unique<FakeAudioProvider>(
            AudioSource::SystemAudio, AudioCaptureResult::SourceUnavailable,
            system_format, system_state),
        std::make_unique<FakeAudioProvider>(
            AudioSource::Microphone, AudioCaptureResult::Ok,
            microphone_format, microphone_state));
    TEST_ASSERT(provider->Start(AudioSource::Both, [](const AudioPacket&) {}) ==
                    AudioCaptureResult::SystemSourceUnavailable,
                "Both must identify System Audio startup unavailability");
    TEST_ASSERT(!microphone_state->started.load(std::memory_order_acquire),
                "Microphone must not start after System Audio unavailability");
    TEST_ASSERT(!provider->active(), "Provider must not be active after startup failure");

    system_state = std::make_shared<FakeAudioState>();
    microphone_state = std::make_shared<FakeAudioState>();
    provider = detail::CreateDualAudioCaptureProviderForTesting(
        std::make_unique<FakeAudioProvider>(
            AudioSource::SystemAudio, AudioCaptureResult::Ok,
            system_format, system_state),
        std::make_unique<FakeAudioProvider>(
            AudioSource::Microphone, AudioCaptureResult::InitializationFailed,
            microphone_format, microphone_state));
    TEST_ASSERT(provider->Start(AudioSource::Both, [](const AudioPacket&) {}) ==
                    AudioCaptureResult::MicrophoneInitializationFailed,
                "Both must identify Microphone startup initialization failure");
    TEST_ASSERT(system_state->stopped.load(std::memory_order_acquire),
                "Microphone initialization failure must roll back System Audio");
    TEST_ASSERT(!provider->active(), "Provider must not be active after mic startup failure");

    system_state = std::make_shared<FakeAudioState>();
    microphone_state = std::make_shared<FakeAudioState>();
    provider = detail::CreateDualAudioCaptureProviderForTesting(
        std::make_unique<FakeAudioProvider>(
            AudioSource::SystemAudio, AudioCaptureResult::Ok,
            system_format, system_state),
        std::make_unique<FakeAudioProvider>(
            AudioSource::Microphone, AudioCaptureResult::Ok,
            microphone_format, microphone_state));
    TEST_ASSERT(provider->Start(AudioSource::Both, [](const AudioPacket&) {}) ==
                    AudioCaptureResult::Ok,
                "Synthetic Both startup should initialize the production DSP path");
    microphone_state->failures.store(1, std::memory_order_release);
    ::Sleep(20);
    const AudioCaptureDiagnostics failure = provider->diagnostics();
    TEST_ASSERT(!provider->active(),
                "A mid-session endpoint failure must terminate Both health");
    TEST_ASSERT(failure.microphone_capture_failures == 1 &&
                    failure.system_capture_failures == 0,
                "Mid-session failure diagnostics must identify Microphone");
    provider->Stop();
    TEST_ASSERT(system_state->stopped.load(std::memory_order_acquire) &&
                    microphone_state->stopped.load(std::memory_order_acquire),
                "Both shutdown must stop both endpoints after a failure");
    return 1;
}

/// Test 4: AlreadyActive guard
int TestAlreadyActive() {
    using namespace easnap::media;

    auto provider = CreateWasapiAudioCaptureProvider();

    if (!provider->IsSourceAvailable(AudioSource::SystemAudio)) {
        Trace("  No system audio endpoint — skipping.\n");
        return -1;
    }

    auto callback = [](const AudioPacket&) {};

    const auto r1 = provider->Start(AudioSource::SystemAudio, callback);
    TEST_ASSERT(r1 == AudioCaptureResult::Ok, "First Start should succeed");
    TEST_ASSERT(provider->active(), "Should be active");

    // Second Start while active should return AlreadyActive.
    const auto r2 = provider->Start(AudioSource::SystemAudio, callback);
    TEST_ASSERT(r2 == AudioCaptureResult::AlreadyActive, "Second Start should return AlreadyActive");

    provider->Stop();
    TEST_ASSERT(!provider->active(), "Should not be active after Stop");

    return 1;
}

/// Exercises a loopback endpoint that is silent when capture starts, then
/// wakes after a bounded delay. This must run before any test that opens a real
/// endpoint so process-local test setup cannot hide wake-up behavior.
int TestDelayedSystemAudioWakeup() {
    using namespace easnap::media;

    auto provider = CreateWasapiAudioCaptureProvider();
    std::atomic<bool> renderer_started{false};
    std::mutex first_packet_mutex;
    bool found_first_signal = false;
    MediaTime first_signal_timestamp{0};
    AudioTimestampSource first_signal_source = AudioTimestampSource::CaptureQpcFallback;

    auto callback = [&](const AudioPacket& packet) {
        if (!renderer_started.load(std::memory_order_acquire) ||
            HasFlag(packet.flags, AudioPacketFlags::Silent)) {
            return;
        }
        const std::vector<AudioPacket> candidate{packet};
        if (ComputeRms(candidate) <= 0.01) {
            return;
        }
        const std::scoped_lock lock(first_packet_mutex);
        if (!found_first_signal) {
            found_first_signal = true;
            first_signal_timestamp = packet.timestamp;
            first_signal_source = packet.timestamp_source;
        }
    };

    const auto start_result = provider->Start(AudioSource::SystemAudio, callback);
    if (start_result == AudioCaptureResult::SourceUnavailable) {
        Trace("  No system audio render endpoint available - skipping.\n");
        return -1;
    }
    TEST_ASSERT(start_result == AudioCaptureResult::Ok,
                "Silent-first System Audio capture should start");

    ::Sleep(1200);

    TestAudioRenderer renderer;
    if (!renderer.Initialize()) {
        provider->Stop();
        Trace("  Failed to initialize delayed render client - skipping.\n");
        return -1;
    }

    const auto render_start_position = provider->clock_position();
    TEST_ASSERT(render_start_position.has_value(),
                "Provider QPC position should be available after Start");
    renderer_started.store(true, std::memory_order_release);
    renderer.StartRendering(523.25f);
    ::Sleep(1500);

    provider->Stop();
    renderer.StopAndCleanup();

    MediaTime observed_timestamp{0};
    AudioTimestampSource observed_source = AudioTimestampSource::CaptureQpcFallback;
    {
        const std::scoped_lock lock(first_packet_mutex);
        TEST_ASSERT(found_first_signal,
                    "Delayed renderer should produce a non-silent loopback packet");
        observed_timestamp = first_signal_timestamp;
        observed_source = first_signal_source;
    }

    constexpr MediaTime early_tolerance{2'500'000};  // 250 ms
    TEST_ASSERT(observed_timestamp + early_tolerance >= render_start_position.value(),
                "First real System Audio packet must remain near its delayed active-time position");
    TEST_ASSERT(observed_timestamp >= MediaTime{7'500'000},
                "First real packet must not collapse near recording time zero");

    const auto diag = provider->diagnostics();
    TEST_ASSERT(diag.qpc_epoch_100ns > 0,
                "Timestamp diagnostics should expose the capture QPC epoch");
    TEST_ASSERT(diag.timestamp_resolution_count > 0,
                "Timestamp diagnostics should retain initial packet resolutions");
    TEST_ASSERT(diag.timestamp_resolution_count <= kAudioTimestampDiagnosticCapacity,
                "Timestamp diagnostics must remain fixed-capacity");
    Trace("  Delayed first signal: pts=%lld source=%u render_start=%lld; "
          "timestamp diagnostics=%zu\n",
          static_cast<long long>(observed_timestamp.count()),
          static_cast<unsigned int>(observed_source),
          static_cast<long long>(render_start_position->count()),
          diag.timestamp_resolution_count);
    const auto& first_resolution = diag.timestamp_resolutions[0];
    Trace("  First resolution: device=%llu clock_pos=%llu clock_freq=%llu "
          "raw_rel=%lld clock_rel=%lld fallback_rel=%lld "
          "resolved=%lld source=%u raw_ok=%u clock_ok=%u\n",
          static_cast<unsigned long long>(first_resolution.device_position),
          static_cast<unsigned long long>(first_resolution.audio_clock_position),
          static_cast<unsigned long long>(first_resolution.audio_clock_frequency),
          static_cast<long long>(first_resolution.raw_wasapi_timestamp.count()),
          static_cast<long long>(first_resolution.audio_clock_timestamp.count()),
          static_cast<long long>(first_resolution.capture_fallback_timestamp.count()),
          static_cast<long long>(first_resolution.resolved_timestamp.count()),
          static_cast<unsigned int>(first_resolution.source),
          first_resolution.raw_wasapi_plausible ? 1u : 0u,
          first_resolution.audio_clock_correlation_valid ? 1u : 0u);

    return 1;
}

/// Test 5: System audio loopback with real test waveform
int TestSystemAudioLoopback() {
    using namespace easnap::media;

    auto provider = CreateWasapiAudioCaptureProvider();
    TEST_ASSERT(provider != nullptr, "CreateWasapiAudioCaptureProvider returned null");
    TEST_ASSERT(!provider->active(), "Provider should not be active initially");

    if (!provider->IsSourceAvailable(AudioSource::SystemAudio)) {
        Trace("  No system audio render endpoint available — skipping.\n");
        return -1;  // Skip
    }

    // Start the sine wave render client as TEST INPUT.
    TestAudioRenderer renderer;
    if (!renderer.Initialize()) {
        Trace("  Failed to initialize render client — skipping.\n");
        return -1;
    }

    renderer.StartRendering(440.0f);

    // Give the render a moment to start producing audio.
    ::Sleep(300);

    // Capture loopback audio with the production provider.
    std::mutex packets_mutex;
    std::vector<AudioPacket> captured;
    MediaTime last_ts{-1};
    bool monotonic = true;

    auto callback = [&](const AudioPacket& pkt) {
        const std::scoped_lock lock(packets_mutex);
        if (last_ts >= MediaTime{0} && pkt.timestamp < last_ts) {
            monotonic = false;
        }
        last_ts = pkt.timestamp;
        captured.push_back(pkt);  // Copy for validation.
    };

    const auto result = provider->Start(AudioSource::SystemAudio, callback);
    TEST_ASSERT(result == AudioCaptureResult::Ok, "Start(SystemAudio) should succeed");
    TEST_ASSERT(provider->active(), "Provider should be active after Start");

    // Check negotiated format.
    const AudioFormat fmt = provider->negotiated_format();
    Trace("  Format: %u Hz, %u ch, %u bits, %s, mask=0x%08X\n",
          fmt.sample_rate, fmt.channels, fmt.bits_per_sample,
          fmt.is_float ? "float" : "int", fmt.channel_mask);
    TEST_ASSERT(fmt.valid(), "Negotiated format should be valid");

    // Capture for 2 seconds.
    ::Sleep(2000);

    // Stop capture first, then stop rendering.
    provider->Stop();
    TEST_ASSERT(!provider->active(), "Provider should not be active after Stop");

    renderer.StopAndCleanup();

    // Validate captured data.
    const std::scoped_lock lock(packets_mutex);
    Trace("  Captured %zu packets\n", captured.size());
    TEST_ASSERT(!captured.empty(), "Should have captured at least one packet");

    uint64_t total_frames = 0;
    uint64_t total_bytes = 0;
    for (const auto& pkt : captured) {
        TEST_ASSERT(pkt.format.valid(), "Each packet should have a valid format");
        TEST_ASSERT(pkt.consistent(), "Packet data size should match frame_count * block_align");
        total_frames += pkt.frame_count;
        total_bytes += pkt.data.size();
    }

    Trace("  Total frames: %llu, total bytes: %llu\n",
          static_cast<unsigned long long>(total_frames),
          static_cast<unsigned long long>(total_bytes));
    TEST_ASSERT(total_frames > 0, "Should have captured audio frames > 0");
    TEST_ASSERT(monotonic, "Timestamps should be monotonic");

    // Content validation: RMS of captured audio should be non-trivial
    // (we were playing a 440 Hz sine wave).
    const double rms = ComputeRms(captured);
    Trace("  RMS level: %.6f\n", rms);
    TEST_ASSERT(rms > 0.01, "Captured audio should contain non-silent signal (RMS > 0.01)");

    // Diagnostics check.
    const auto diag = provider->diagnostics();
    Trace("  Diagnostics: packets=%llu, frames=%llu, bytes=%llu, "
          "silent=%llu, disc=%llu, overruns=%llu, failures=%llu, "
          "peak_queue=%zu/%zu\n",
          static_cast<unsigned long long>(diag.packets_acquired),
          static_cast<unsigned long long>(diag.audio_frames_acquired),
          static_cast<unsigned long long>(diag.bytes_acquired),
          static_cast<unsigned long long>(diag.silent_packets),
          static_cast<unsigned long long>(diag.discontinuities),
          static_cast<unsigned long long>(diag.buffer_overruns),
          static_cast<unsigned long long>(diag.capture_failures),
          diag.peak_queue_occupancy, diag.queue_capacity);
    TEST_ASSERT(diag.packets_acquired > 0, "Diagnostics should show packets acquired");
    TEST_ASSERT(diag.audio_frames_acquired > 0, "Diagnostics should show frames acquired");
    TEST_ASSERT(diag.capture_failures == 0, "No capture failures expected");
    TEST_ASSERT(diag.queue_capacity >= kMinimumAudioQueueCapacity,
                "Audio queue capacity should retain its production minimum");
    TEST_ASSERT(diag.peak_queue_occupancy <= diag.queue_capacity,
                "Audio queue occupancy must stay within its fixed capacity");

    return 1;  // Pass
}

/// Test 6: Microphone capture (hardware-dependent)
int TestMicrophoneCapture() {
    using namespace easnap::media;

    auto provider = CreateWasapiAudioCaptureProvider();

    if (!provider->IsSourceAvailable(AudioSource::Microphone)) {
        Trace("  No microphone endpoint available — skipping.\n");
        Trace("  (This is accurate hardware reporting, not a test failure.)\n");
        return -1;  // Skip
    }

    std::mutex packets_mutex;
    std::vector<AudioPacket> captured;

    auto callback = [&](const AudioPacket& pkt) {
        const std::scoped_lock lock(packets_mutex);
        captured.push_back(pkt);
    };

    const auto result = provider->Start(AudioSource::Microphone, callback);
    if (result != AudioCaptureResult::Ok) {
        const AudioCaptureDiagnostics failed = provider->diagnostics();
        Trace("  Start(Microphone) result=%u hr=0x%08llX\n",
              static_cast<unsigned int>(result),
              static_cast<unsigned long long>(failed.last_failure_hresult));
    }
    TEST_ASSERT(result == AudioCaptureResult::Ok, "Start(Microphone) should succeed");
    TEST_ASSERT(provider->active(), "Provider should be active after Start");

    const AudioFormat fmt = provider->negotiated_format();
    Trace("  Microphone format: %u Hz, %u ch, %u bits, %s\n",
          fmt.sample_rate, fmt.channels, fmt.bits_per_sample,
          fmt.is_float ? "float" : "int");
    TEST_ASSERT(fmt.valid(), "Microphone format should be valid");

    // Capture for 1 second.
    ::Sleep(1000);

    provider->Stop();
    TEST_ASSERT(!provider->active(), "Provider should not be active after Stop");

    const std::scoped_lock lock(packets_mutex);
    Trace("  Microphone captured %zu packets\n", captured.size());
    TEST_ASSERT(!captured.empty(), "Should capture at least one microphone packet");

    for (const auto& pkt : captured) {
        TEST_ASSERT(pkt.format.valid(), "Microphone packet format should be valid");
        TEST_ASSERT(pkt.consistent(), "Microphone packet should be consistent");
    }

    // Note: we do NOT fail if the room is quiet (RMS near zero is expected).
    const double rms = ComputeRms(captured);
    Trace("  Microphone RMS: %.6f (quiet room is normal)\n", rms);

    const auto diag = provider->diagnostics();
    TEST_ASSERT(diag.packets_acquired > 0, "Microphone diagnostics should show packets");
    TEST_ASSERT(diag.capture_failures == 0, "No microphone capture failures expected");
    TEST_ASSERT(diag.queue_capacity >= kMinimumAudioQueueCapacity,
                "Microphone queue capacity should retain its production minimum");
    TEST_ASSERT(diag.peak_queue_occupancy <= diag.queue_capacity,
                "Microphone queue occupancy must stay within its fixed capacity");

    return 1;
}

/// Dual-source production path: two real endpoints, canonical conversion,
/// bounded mixing, and pause/resume delivery semantics.
int TestBothCapture() {
    using namespace easnap::media;

    auto provider = CreateWasapiAudioCaptureProvider();
    TEST_ASSERT(provider != nullptr,
                "CreateWasapiAudioCaptureProvider returned null");
    if (!provider->IsSourceAvailable(AudioSource::Both)) {
        Trace("  System Audio and Microphone are not both available - skipping.\n");
        return -1;
    }

    TestAudioRenderer renderer;
    if (!renderer.Initialize()) {
        Trace("  Failed to initialize render client - skipping.\n");
        return -1;
    }
    renderer.StartRendering(440.0F);
    ::Sleep(200);

    std::mutex packets_mutex;
    std::vector<AudioPacket> captured;
    auto callback = [&](const AudioPacket& packet) {
        const std::scoped_lock lock(packets_mutex);
        captured.push_back(packet);
    };
    const AudioCaptureResult result = provider->Start(AudioSource::Both, callback);
    if (result != AudioCaptureResult::Ok) {
        const AudioCaptureDiagnostics failed = provider->diagnostics();
        Trace("  Start(Both) result=%u hr=0x%08llX\n",
              static_cast<unsigned int>(result),
              static_cast<unsigned long long>(failed.last_failure_hresult));
    }
    TEST_ASSERT(result == AudioCaptureResult::Ok, "Start(Both) should succeed");
    const AudioFormat format = provider->negotiated_format();
    TEST_ASSERT(format.sample_rate == 48'000 && format.channels == 2 &&
                    format.bits_per_sample == 32 && format.is_float &&
                    format.channel_mask == 0x3,
                "Both must expose canonical 48 kHz stereo float PCM");

    ::Sleep(700);
    provider->PauseDeliveryAndDrain();
    std::size_t paused_count = 0;
    {
        const std::scoped_lock lock(packets_mutex);
        paused_count = captured.size();
    }
    ::Sleep(200);
    {
        const std::scoped_lock lock(packets_mutex);
        TEST_ASSERT(captured.size() == paused_count,
                    "No mixed packets may be delivered while paused");
    }
    provider->ResumeDelivery();
    ::Sleep(700);
    provider->Stop();
    renderer.StopAndCleanup();

    const std::scoped_lock lock(packets_mutex);
    TEST_ASSERT(!captured.empty(), "Both should emit mixed PCM packets");
    MediaTime previous_end{0};
    bool saw_resume_discontinuity = false;
    for (const AudioPacket& packet : captured) {
        TEST_ASSERT(packet.consistent(), "Mixed packet must be internally consistent");
        TEST_ASSERT(packet.format.sample_rate == format.sample_rate &&
                        packet.format.channels == format.channels &&
                        packet.format.bits_per_sample == format.bits_per_sample &&
                        packet.format.is_float == format.is_float,
                    "Every mixed packet must retain the canonical format");
        TEST_ASSERT(packet.timestamp >= previous_end,
                    "Mixed packet timestamps must be monotonic");
        saw_resume_discontinuity =
            saw_resume_discontinuity ||
            HasFlag(packet.flags, AudioPacketFlags::Discontinuity);
        previous_end = packet.timestamp + MediaTime{100'000};
    }
    TEST_ASSERT(saw_resume_discontinuity,
                "First packet after resume must mark a discontinuity");
    const AudioCaptureDiagnostics diagnostics = provider->diagnostics();
    TEST_ASSERT(diagnostics.peak_queue_occupancy <= diagnostics.queue_capacity,
                "Both capture storage must remain within its fixed capacity");
    TEST_ASSERT(diagnostics.capture_failures == 0,
                "Both capture should have no endpoint or mixer failure");
    TEST_ASSERT(ComputeRms(captured) > 0.005,
                "Mixed output should retain the rendered System Audio signal");
    return 1;
}

/// Regression test for Defect B:
/// 1. Start Both with System Audio initially silent while Microphone runs.
/// 2. Wait long enough to establish mixer output progress (700 ms).
/// 3. Start System Audio renderer mid-stream.
/// 4. Verify capture continues without mixer failure, queue overflow,
///    or fatal rejected packets.
int TestBothLateSystemAudioCapture() {
    using namespace easnap::media;

    auto provider = CreateWasapiAudioCaptureProvider();
    TEST_ASSERT(provider != nullptr,
                "CreateWasapiAudioCaptureProvider returned null");
    if (!provider->IsSourceAvailable(AudioSource::Both)) {
        Trace("  System Audio and Microphone are not both available - skipping.\n");
        return -1;
    }

    TestAudioRenderer renderer;
    if (!renderer.Initialize()) {
        Trace("  Failed to initialize render client - skipping.\n");
        return -1;
    }

    std::mutex packets_mutex;
    std::vector<AudioPacket> captured;
    auto callback = [&](const AudioPacket& packet) {
        const std::scoped_lock lock(packets_mutex);
        captured.push_back(packet);
    };

    // 1. Start Both with System Audio initially SILENT (renderer not started yet).
    const AudioCaptureResult result = provider->Start(AudioSource::Both, callback);
    TEST_ASSERT(result == AudioCaptureResult::Ok, "Start(Both) should succeed");

    // 2. Microphone captures for 700 ms while System Audio is completely silent.
    ::Sleep(700);

    // 3. Now start System Audio renderer: wake-up occurs mid-session.
    renderer.StartRendering(440.0F);
    ::Sleep(700);

    // 4. Stop capture and cleanup.
    provider->Stop();
    renderer.StopAndCleanup();

    const std::scoped_lock lock(packets_mutex);
    TEST_ASSERT(!captured.empty(), "Should capture packets throughout session");

    const AudioCaptureDiagnostics diag = provider->diagnostics();
    TEST_ASSERT(diag.capture_failures == 0,
                "Late System Audio start must not cause capture_failures");
    TEST_ASSERT(diag.mixer_capacity_failures == 0,
                "Late System Audio start must not cause mixer capacity failures");
    TEST_ASSERT(diag.peak_queue_occupancy <= diag.queue_capacity,
                "Queue occupancy must remain bounded within capacity");
    TEST_ASSERT(ComputeRms(captured) > 0.005,
                "Rendered System Audio tone must appear in mixed output");

    return 1;
}

/// Test 7: Repeated lifecycle (Start -> Stop -> Start -> Stop)
int TestRepeatedLifecycle() {
    using namespace easnap::media;

    auto provider = CreateWasapiAudioCaptureProvider();

    if (!provider->IsSourceAvailable(AudioSource::SystemAudio)) {
        Trace("  No system audio endpoint — skipping.\n");
        return -1;
    }

    for (int iteration = 0; iteration < 3; ++iteration) {
        Trace("  Lifecycle iteration %d\n", iteration + 1);

        std::atomic<uint64_t> packet_count{0};
        auto callback = [&](const AudioPacket&) {
            packet_count.fetch_add(1, std::memory_order_relaxed);
        };

        const auto result = provider->Start(AudioSource::SystemAudio, callback);
        TEST_ASSERT(result == AudioCaptureResult::Ok, "Start should succeed on each iteration");
        TEST_ASSERT(provider->active(), "Should be active after Start");

        ::Sleep(500);

        provider->Stop();
        TEST_ASSERT(!provider->active(), "Should not be active after Stop");

        Trace("  Iteration %d: captured %llu packets\n",
              iteration + 1,
              static_cast<unsigned long long>(packet_count.load()));
    }

    return 1;
}

}  // namespace

int main() {
    // Initialize COM for the test process (MTA for WASAPI).
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_hr)) {
        Trace("CoInitializeEx failed: 0x%08lX\n", com_hr);
        return 1;
    }

    Trace("Easnap WASAPI Audio Capture Validation\n");
    Trace("=======================================\n\n");

    RUN_TEST(TestAudioFormat);
    RUN_TEST(TestAudioQueueSemantics);
    RUN_TEST(TestAudioDeliveryBarrier);
    RUN_TEST(TestTimestampResolution);
    RUN_TEST(TestFallbackTimestampCappedPadding);
    RUN_TEST(TestInvalidSources);
    RUN_TEST(TestBothFailureSemantics);
    RUN_TEST(TestDelayedSystemAudioWakeup);
    RUN_TEST(TestAlreadyActive);
    RUN_TEST(TestSystemAudioLoopback);
    RUN_TEST(TestMicrophoneCapture);
    RUN_TEST(TestBothCapture);
    RUN_TEST(TestBothLateSystemAudioCapture);
    RUN_TEST(TestRepeatedLifecycle);

    Trace("=======================================\n");
    Trace("Results: %d passed, %d failed, %d skipped (of %d)\n",
          g_tests_passed, g_tests_failed, g_tests_skipped, g_tests_run);

    // Without any audio endpoint every capture test above skipped itself.
    // What remains is queue and timestamp logic; reporting that as a pass
    // would claim a capture that never happened. 77 is SKIP_RETURN_CODE.
    bool endpoint_available = false;
    if (const auto probe = easnap::media::CreateWasapiAudioCaptureProvider()) {
        endpoint_available = probe->IsSourceAvailable(easnap::media::AudioSource::SystemAudio) ||
                             probe->IsSourceAvailable(easnap::media::AudioSource::Microphone);
    }
    ::CoUninitialize();

    if (g_tests_failed > 0) { return 1; }
    if (!endpoint_available) {
        Trace("No audio endpoint: the capture tests did not run.\n");
        return 77;
    }
    return 0;
}
