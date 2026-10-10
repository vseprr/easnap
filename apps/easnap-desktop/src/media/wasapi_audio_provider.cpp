// Easnap - production WASAPI audio capture provider implementation.
//
// Event-driven WASAPI capture for system-audio loopback and microphone.
// The capture thread owns COM initialization (MTA), device activation,
// IAudioClient setup, and the event-driven capture loop. All COM resources
// are released before the apartment is torn down.
//
// Clock source: validated QPC timestamps returned by GetBuffer, with an
// IAudioClock2 device-position correlation when available and capture-QPC minus
// current WASAPI padding as a bounded fallback. Continuous sample timing still
// uses the preserved device position and captured frame counts downstream.
//
// Bounded buffering: an internal AudioQueue with a 500 ms time budget holds
// packets until the consumer (PacketCallback) retrieves them. On overrun the
// capture continues but the next packet is flagged with Discontinuity.

#include "media/wasapi_audio_provider.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

#include "capture/recording_capture_worker.h"  // SaturatingCounter
#include "media/audio_delivery_barrier.h"
#include "media/audio_queue.h"
#include "platform/platform_utils.h"

namespace easnap::media {

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Converts WAVEFORMATEX / WAVEFORMATEXTENSIBLE to AudioFormat.
AudioFormat FormatFromWaveFormat(const WAVEFORMATEX& wfx) noexcept {
    AudioFormat fmt;
    fmt.sample_rate = wfx.nSamplesPerSec;
    fmt.channels = static_cast<uint16_t>(wfx.nChannels);
    fmt.bits_per_sample = static_cast<uint16_t>(wfx.wBitsPerSample);

    if (wfx.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        fmt.is_float = true;
    } else if (wfx.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx.cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&wfx);
        fmt.channel_mask = ext->dwChannelMask;
        fmt.bits_per_sample = static_cast<uint16_t>(ext->Samples.wValidBitsPerSample);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            fmt.is_float = true;
        }
    }

    return fmt;
}

/// WASAPI default buffer period in 100 ns units (10 ms).
constexpr int64_t kDefaultBufferPeriod100ns = 100'000;

/// Converts raw QueryPerformanceCounter ticks to the 100 ns clock domain used
/// by WASAPI's GetBuffer QPC position.
MediaTime QpcTicksToMediaTime(LARGE_INTEGER qpc,
                              LARGE_INTEGER frequency) noexcept {
    const int64_t seconds = qpc.QuadPart / frequency.QuadPart;
    const int64_t remainder = qpc.QuadPart % frequency.QuadPart;
    const int64_t hns = seconds * 10'000'000 + (remainder * 10'000'000) / frequency.QuadPart;
    return MediaTime{hns};
}

constexpr uint64_t kTimestampTolerance100ns = 1'000'000;  // 100 ms
constexpr uint64_t kMaximumClockCorrelationAge100ns = 50'000'000;  // 5 seconds

std::optional<uint64_t> ScaleUnsigned(uint64_t value, uint64_t numerator,
                                      uint64_t denominator) noexcept {
    if (denominator == 0) {
        return std::nullopt;
    }
    const uint64_t quotient = value / denominator;
    const uint64_t remainder = value % denominator;
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    if (numerator != 0 && quotient > maximum / numerator) {
        return std::nullopt;
    }
    const uint64_t integral = quotient * numerator;
    if (remainder != 0 && numerator > maximum / remainder) {
        return std::nullopt;
    }
    const uint64_t fractional = (remainder * numerator) / denominator;
    if (integral > maximum - fractional) {
        return std::nullopt;
    }
    return integral + fractional;
}

bool IsNotAfterWithTolerance(uint64_t value, uint64_t reference) noexcept {
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    const uint64_t upper_bound = reference > maximum - kTimestampTolerance100ns
                                     ? maximum
                                     : reference + kTimestampTolerance100ns;
    return value <= upper_bound;
}

MediaTime RelativeTimestamp(uint64_t absolute, int64_t epoch) noexcept {
    if (epoch <= 0 ||
        absolute > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
        return MediaTime{(std::numeric_limits<int64_t>::min)()};
    }
    return MediaTime{static_cast<int64_t>(absolute) - epoch};
}

uint64_t AbsoluteDifference(uint64_t lhs, uint64_t rhs) noexcept {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

detail::AudioTimestampResolutionResult ResolveAudioPacketTimestampImpl(
    const detail::AudioTimestampResolutionInput& input) noexcept {
    detail::AudioTimestampResolutionResult result;
    result.raw_wasapi_timestamp =
        RelativeTimestamp(input.raw_wasapi_qpc_100ns, input.qpc_epoch_100ns);

    uint64_t correlated_packet_qpc = 0;
    if (input.qpc_epoch_100ns > 0 && input.sample_rate > 0 &&
        input.audio_clock_frequency > 0 && input.audio_clock_qpc_100ns > 0 &&
        input.capture_qpc_100ns >= static_cast<uint64_t>(input.qpc_epoch_100ns) &&
        input.audio_clock_qpc_100ns >= static_cast<uint64_t>(input.qpc_epoch_100ns) &&
        IsNotAfterWithTolerance(input.audio_clock_qpc_100ns,
                                input.capture_qpc_100ns)) {
        if (input.audio_clock_position >= input.device_position) {
            const auto packet_age = ScaleUnsigned(
                input.audio_clock_position - input.device_position,
                10'000'000, input.sample_rate);
            if (packet_age.has_value() &&
                packet_age.value() <= kMaximumClockCorrelationAge100ns &&
                input.audio_clock_qpc_100ns >= packet_age.value()) {
                correlated_packet_qpc = input.audio_clock_qpc_100ns - packet_age.value();
                result.audio_clock_correlation_valid =
                    correlated_packet_qpc >= static_cast<uint64_t>(input.qpc_epoch_100ns) &&
                    IsNotAfterWithTolerance(correlated_packet_qpc,
                                            input.capture_qpc_100ns);
            }
        }
    }
    result.audio_clock_timestamp =
        RelativeTimestamp(correlated_packet_qpc, input.qpc_epoch_100ns);

    const uint32_t capped_buffered = input.maximum_buffer_frames > 0
        ? (std::min)(input.buffered_frame_count, input.maximum_buffer_frames)
        : input.buffered_frame_count;
    const uint32_t buffered_frames =
        (std::max)(input.frame_count, capped_buffered);
    const auto buffered_duration =
        ScaleUnsigned(buffered_frames, 10'000'000, input.sample_rate);
    uint64_t fallback_packet_qpc = input.capture_qpc_100ns;
    if (buffered_duration.has_value() &&
        fallback_packet_qpc >= buffered_duration.value()) {
        fallback_packet_qpc -= buffered_duration.value();
    }
    if (input.qpc_epoch_100ns > 0 &&
        fallback_packet_qpc < static_cast<uint64_t>(input.qpc_epoch_100ns)) {
        // A capture-thread observation cannot describe a frame before the
        // pre-Start epoch. Keep it explicit and place it at the observation
        // time instead of silently converting an invalid source to zero.
        fallback_packet_qpc = input.capture_qpc_100ns;
    }
    result.capture_fallback_timestamp =
        RelativeTimestamp(fallback_packet_qpc, input.qpc_epoch_100ns);

    result.raw_wasapi_plausible =
        !input.wasapi_timestamp_error && input.qpc_epoch_100ns > 0 &&
        input.raw_wasapi_qpc_100ns >= static_cast<uint64_t>(input.qpc_epoch_100ns) &&
        input.capture_qpc_100ns >= static_cast<uint64_t>(input.qpc_epoch_100ns) &&
        input.raw_wasapi_qpc_100ns <=
            static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) &&
        IsNotAfterWithTolerance(input.raw_wasapi_qpc_100ns,
                                input.capture_qpc_100ns);
    if (result.raw_wasapi_plausible && buffered_duration.has_value() &&
        input.capture_qpc_100ns >= input.raw_wasapi_qpc_100ns) {
        const uint64_t raw_age =
            input.capture_qpc_100ns - input.raw_wasapi_qpc_100ns;
        const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
        const uint64_t maximum_expected_age =
            buffered_duration.value() > maximum - kTimestampTolerance100ns
                ? maximum
                : buffered_duration.value() + kTimestampTolerance100ns;
        if (raw_age > maximum_expected_age) {
            result.raw_wasapi_plausible = false;
        }
    }
    if (result.raw_wasapi_plausible && result.audio_clock_correlation_valid &&
        AbsoluteDifference(input.raw_wasapi_qpc_100ns, correlated_packet_qpc) >
            kTimestampTolerance100ns) {
        result.raw_wasapi_plausible = false;
    }

    if (result.raw_wasapi_plausible) {
        result.resolved_timestamp = result.raw_wasapi_timestamp;
        result.source = AudioTimestampSource::WasapiQpc;
    } else if (result.audio_clock_correlation_valid) {
        result.resolved_timestamp = result.audio_clock_timestamp;
        result.source = AudioTimestampSource::AudioClockCorrelation;
    } else {
        result.resolved_timestamp = result.capture_fallback_timestamp;
        result.source = AudioTimestampSource::CaptureQpcFallback;
    }

    // QueryPerformanceCounter observations are expected to be post-epoch. If
    // the platform violates that invariant, retain an explicit fallback source
    // and a non-negative placement without accepting the invalid raw timestamp.
    if (result.resolved_timestamp < MediaTime{0}) {
        result.resolved_timestamp = MediaTime{0};
        result.source = AudioTimestampSource::CaptureQpcFallback;
    }
    return result;
}

// ---------------------------------------------------------------------------
// WasapiAudioProvider
// ---------------------------------------------------------------------------

class WasapiAudioProvider final : public IAudioCaptureProvider {
public:
    WasapiAudioProvider() = default;
    ~WasapiAudioProvider() override { Stop(); }

    WasapiAudioProvider(const WasapiAudioProvider&) = delete;
    WasapiAudioProvider& operator=(const WasapiAudioProvider&) = delete;

    AudioCaptureResult Start(AudioSource source, PacketCallback on_packet) override;
    void PauseDeliveryAndDrain() noexcept override;
    void ResumeDelivery() noexcept override;
    void Stop() override;
    [[nodiscard]] bool active() const override { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] bool IsSourceAvailable(AudioSource source) const override;
    [[nodiscard]] AudioFormat negotiated_format() const override;
    [[nodiscard]] std::optional<MediaTime> clock_position() const noexcept override;
    [[nodiscard]] AudioCaptureDiagnostics diagnostics() const override;

private:
    void CaptureThread(AudioSource source, PacketCallback on_packet);

    std::thread thread_;
    platform::UniqueHandle stop_event_;
    std::atomic<bool> active_{false};

    // Synchronization for Start: the capture thread signals this event once it
    // has either successfully initialized WASAPI or failed.
    platform::UniqueHandle init_done_event_;
    std::atomic<AudioCaptureResult> init_result_{AudioCaptureResult::Ok};

    mutable std::mutex format_mutex_;
    AudioFormat negotiated_format_{};
    std::atomic<int64_t> qpc_epoch_100ns_{0};
    std::atomic<int64_t> last_failure_hresult_{0};
    detail::AudioDeliveryBarrier delivery_barrier_;

    // Diagnostics — all atomic/saturating, safe to read from any thread.
    capture::SaturatingCounter packets_acquired_;
    capture::SaturatingCounter audio_frames_acquired_;
    capture::SaturatingCounter bytes_acquired_;
    capture::SaturatingCounter silent_packets_;
    capture::SaturatingCounter discontinuities_;
    capture::SaturatingCounter device_invalidations_;
    capture::SaturatingCounter buffer_overruns_;
    capture::SaturatingCounter capture_failures_;
    std::atomic<std::size_t> peak_queue_occupancy_{0};
    std::atomic<std::size_t> queue_capacity_{0};
    mutable std::mutex timestamp_diagnostics_mutex_;
    std::size_t timestamp_resolution_count_{0};
    std::array<AudioTimestampResolutionDiagnostic,
               kAudioTimestampDiagnosticCapacity> timestamp_resolutions_{};
};

// ---------------------------------------------------------------------------
// IsSourceAvailable — probes the real endpoint without starting capture
// ---------------------------------------------------------------------------

bool WasapiAudioProvider::IsSourceAvailable(AudioSource source) const {
    if (source == AudioSource::None || source == AudioSource::Both) {
        return false;
    }

    // Temporary COM scope for the probe.
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    bool available = false;
    {
        platform::ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                        CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) {
            platform::ComPtr<IMMDevice> device;
            if (source == AudioSource::SystemAudio) {
                hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            } else {
                hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            }
            if (SUCCEEDED(hr) && device != nullptr) {
                platform::ComPtr<IAudioClient> client;
                hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                      nullptr,
                                      reinterpret_cast<void**>(
                                          client.GetAddressOf()));
                WAVEFORMATEX* format = nullptr;
                if (SUCCEEDED(hr) && client) {
                    hr = client->GetMixFormat(&format);
                }
                DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
                if (source == AudioSource::SystemAudio) {
                    flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
                }
                if (SUCCEEDED(hr) && format != nullptr) {
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                            0, 0, format, nullptr);
                }
                if (format != nullptr) {
                    ::CoTaskMemFree(format);
                }
                available = SUCCEEDED(hr);
            }
        }
    }

    if (SUCCEEDED(com_hr)) {
        ::CoUninitialize();
    }

    return available;
}

// ---------------------------------------------------------------------------
// negotiated_format / diagnostics
// ---------------------------------------------------------------------------

AudioFormat WasapiAudioProvider::negotiated_format() const {
    const std::scoped_lock lock(format_mutex_);
    return negotiated_format_;
}

std::optional<MediaTime> WasapiAudioProvider::clock_position() const noexcept {
    const int64_t epoch = qpc_epoch_100ns_.load(std::memory_order_acquire);
    if (epoch <= 0) {
        return std::nullopt;
    }
    LARGE_INTEGER frequency{};
    LARGE_INTEGER now{};
    if (::QueryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart <= 0 ||
        ::QueryPerformanceCounter(&now) == FALSE) {
        return std::nullopt;
    }
    const MediaTime absolute = QpcTicksToMediaTime(now, frequency);
    if (absolute.count() < epoch) {
        return MediaTime::zero();
    }
    return MediaTime{absolute.count() - epoch};
}

AudioCaptureDiagnostics WasapiAudioProvider::diagnostics() const {
    AudioCaptureDiagnostics diag;
    diag.qpc_epoch_100ns = qpc_epoch_100ns_.load(std::memory_order_acquire);
    diag.last_failure_hresult =
        last_failure_hresult_.load(std::memory_order_acquire);
    diag.packets_acquired = packets_acquired_.value();
    diag.audio_frames_acquired = audio_frames_acquired_.value();
    diag.bytes_acquired = bytes_acquired_.value();
    diag.silent_packets = silent_packets_.value();
    diag.discontinuities = discontinuities_.value();
    diag.device_invalidations = device_invalidations_.value();
    diag.buffer_overruns = buffer_overruns_.value();
    diag.capture_failures = capture_failures_.value();
    const detail::AudioDeliverySnapshot delivery = delivery_barrier_.snapshot();
    diag.delivery_packets_accepted = delivery.accepted_sequence;
    diag.delivery_packets_completed = delivery.completed_sequence;
    diag.delivery_paused_discards = delivery.paused_discards;
    diag.queue_capacity = queue_capacity_.load(std::memory_order_relaxed);
    diag.peak_queue_occupancy = peak_queue_occupancy_.load(std::memory_order_relaxed);
    {
        const std::scoped_lock lock(timestamp_diagnostics_mutex_);
        diag.timestamp_resolution_count = timestamp_resolution_count_;
        diag.timestamp_resolutions = timestamp_resolutions_;
    }
    return diag;
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

AudioCaptureResult WasapiAudioProvider::Start(AudioSource source, PacketCallback on_packet) {
    if (active_.load(std::memory_order_acquire)) {
        return AudioCaptureResult::AlreadyActive;
    }

    if (source == AudioSource::None) {
        return AudioCaptureResult::InvalidSource;
    }
    if (source == AudioSource::Both) {
        // Two-source synchronized capture requires clock alignment and mixing.
        // Not implemented in this slice.
        return AudioCaptureResult::InvalidSource;
    }

    // Reset diagnostics for the new session.
    packets_acquired_.Reset();
    audio_frames_acquired_.Reset();
    bytes_acquired_.Reset();
    silent_packets_.Reset();
    discontinuities_.Reset();
    device_invalidations_.Reset();
    buffer_overruns_.Reset();
    capture_failures_.Reset();
    queue_capacity_.store(0, std::memory_order_relaxed);
    peak_queue_occupancy_.store(0, std::memory_order_relaxed);
    qpc_epoch_100ns_.store(0, std::memory_order_release);
    last_failure_hresult_.store(0, std::memory_order_release);
    delivery_barrier_.Reset();
    {
        const std::scoped_lock lock(timestamp_diagnostics_mutex_);
        timestamp_resolution_count_ = 0;
        timestamp_resolutions_ = {};
    }

    {
        const std::scoped_lock lock(format_mutex_);
        negotiated_format_ = {};
    }

    // Create a manual-reset event for Stop signalling.
    stop_event_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event_) {
        return AudioCaptureResult::InitializationFailed;
    }

    // Create init-done event so Start() can wait for the thread to finish
    // WASAPI initialization before returning.
    init_done_event_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!init_done_event_) {
        stop_event_.reset();
        return AudioCaptureResult::InitializationFailed;
    }
    init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_relaxed);

    thread_ = std::thread([this, source, cb = std::move(on_packet)]() mutable {
        CaptureThread(source, std::move(cb));
    });

    // Wait for the capture thread to complete WASAPI initialization.
    ::WaitForSingleObject(init_done_event_.get(), INFINITE);

    const AudioCaptureResult thread_result = init_result_.load(std::memory_order_acquire);
    if (thread_result != AudioCaptureResult::Ok) {
        // Initialization failed on the capture thread. Join it.
        if (thread_.joinable()) {
            thread_.join();
        }
        stop_event_.reset();
        init_done_event_.reset();
        return thread_result;
    }

    active_.store(true, std::memory_order_release);
    return AudioCaptureResult::Ok;
}

void WasapiAudioProvider::PauseDeliveryAndDrain() noexcept {
    delivery_barrier_.PauseAndDrain();
}

void WasapiAudioProvider::ResumeDelivery() noexcept {
    delivery_barrier_.Resume();
}

void WasapiAudioProvider::Stop() {
    if (!active_.load(std::memory_order_acquire)) {
        return;
    }

    // Signal the capture thread to exit.
    if (stop_event_) {
        ::SetEvent(stop_event_.get());
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    active_.store(false, std::memory_order_release);
    stop_event_.reset();
    init_done_event_.reset();
}

// ---------------------------------------------------------------------------
// CaptureThread — the real event-driven WASAPI capture loop
// ---------------------------------------------------------------------------

void WasapiAudioProvider::CaptureThread(AudioSource source, PacketCallback on_packet) {
    // Initialize COM as MTA on this thread.
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_hr)) {
        platform::LogMessage(L"[Audio] CoInitializeEx failed: 0x%08lX", com_hr);
        last_failure_hresult_.store(com_hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    // RAII: ensure CoUninitialize runs on every exit path, after all COM
    // objects are released.
    struct ComGuard {
        ~ComGuard() { ::CoUninitialize(); }
    } com_guard;

    // Activate the endpoint and negotiate format.
    platform::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        platform::LogMessage(L"[Audio] CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lX", hr);
        last_failure_hresult_.store(hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    platform::ComPtr<IMMDevice> device;
    if (source == AudioSource::SystemAudio) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    }
    if (FAILED(hr) || !device) {
        platform::LogMessage(L"[Audio] GetDefaultAudioEndpoint failed: 0x%08lX", hr);
        last_failure_hresult_.store(hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::SourceUnavailable, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    platform::ComPtr<IAudioClient> audio_client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                           nullptr, reinterpret_cast<void**>(audio_client.GetAddressOf()));
    if (FAILED(hr) || !audio_client) {
        platform::LogMessage(L"[Audio] IMMDevice::Activate failed: 0x%08lX", hr);
        last_failure_hresult_.store(hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    // Discover the native mix format.
    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client->GetMixFormat(&mix_format);
    if (FAILED(hr) || mix_format == nullptr) {
        platform::LogMessage(L"[Audio] GetMixFormat failed: 0x%08lX", hr);
        last_failure_hresult_.store(hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    const AudioFormat captured_format = FormatFromWaveFormat(*mix_format);

    // Store the negotiated format.
    {
        const std::scoped_lock lock(format_mutex_);
        negotiated_format_ = captured_format;
    }

    // Initialize in shared mode, event-driven.
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (source == AudioSource::SystemAudio) {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    // Use the default device period for the buffer.
    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME minimum_period = 0;
    audio_client->GetDevicePeriod(&default_period, &minimum_period);
    if (default_period == 0) {
        default_period = kDefaultBufferPeriod100ns;
    }

    hr = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        0,               // engine-selected shared-mode buffer duration
        0,               // periodicity (must be 0 for shared mode)
        mix_format,
        nullptr);        // session GUID

    ::CoTaskMemFree(mix_format);
    mix_format = nullptr;

    if (FAILED(hr)) {
        platform::LogMessage(L"[Audio] IAudioClient::Initialize failed: 0x%08lX", hr);
        last_failure_hresult_.store(hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    // Create the event for WASAPI buffer-ready notification.
    platform::UniqueHandle audio_event(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!audio_event) {
        platform::LogMessage(L"[Audio] Failed to create audio event");
        last_failure_hresult_.store(HRESULT_FROM_WIN32(::GetLastError()),
                                    std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    hr = audio_client->SetEventHandle(audio_event.get());
    if (FAILED(hr)) {
        platform::LogMessage(L"[Audio] SetEventHandle failed: 0x%08lX", hr);
        last_failure_hresult_.store(hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    // Get the capture client.
    platform::ComPtr<IAudioCaptureClient> capture_client;
    hr = audio_client->GetService(IID_PPV_ARGS(&capture_client));
    if (FAILED(hr)) {
        platform::LogMessage(L"[Audio] GetService(IAudioCaptureClient) failed: 0x%08lX", hr);
        last_failure_hresult_.store(hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    // IAudioClock is an independent high-precision observation of the same
    // client/device timeline. Capture can continue without it, but flagged or
    // implausible GetBuffer timestamps then use the explicit capture-QPC
    // fallback instead of being silently placed at zero.
    platform::ComPtr<IAudioClock> audio_clock;
    platform::ComPtr<IAudioClock2> audio_clock2;
    uint64_t audio_clock_frequency = 0;
    const HRESULT audio_clock_hr = audio_client->GetService(IID_PPV_ARGS(&audio_clock));
    if (SUCCEEDED(audio_clock_hr) && audio_clock) {
        if (FAILED(audio_clock->GetFrequency(&audio_clock_frequency)) ||
            audio_clock_frequency == 0) {
            audio_clock.Reset();
            audio_clock_frequency = 0;
        } else {
            (void)audio_client->GetService(IID_PPV_ARGS(&audio_clock2));
        }
    } else {
        audio_clock.Reset();
    }

    // Establish the shared QPC epoch before Start so the first device frame
    // cannot legitimately predate it. GetBuffer and IAudioClock QPC positions
    // are already expressed in 100 ns units in this same clock domain.
    LARGE_INTEGER qpc_frequency{};
    LARGE_INTEGER qpc_epoch{};
    if (::QueryPerformanceFrequency(&qpc_frequency) == FALSE ||
        qpc_frequency.QuadPart <= 0 ||
        ::QueryPerformanceCounter(&qpc_epoch) == FALSE) {
        platform::LogMessage(L"[Audio] QueryPerformanceCounter initialization failed");
        last_failure_hresult_.store(E_FAIL, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }
    const MediaTime qpc_epoch_100ns = QpcTicksToMediaTime(qpc_epoch, qpc_frequency);
    qpc_epoch_100ns_.store(qpc_epoch_100ns.count(), std::memory_order_release);

    // Start capture.
    hr = audio_client->Start();
    if (FAILED(hr)) {
        platform::LogMessage(L"[Audio] IAudioClient::Start failed: 0x%08lX", hr);
        last_failure_hresult_.store(hr, std::memory_order_release);
        capture_failures_.Increment();
        init_result_.store(AudioCaptureResult::InitializationFailed, std::memory_order_release);
        ::SetEvent(init_done_event_.get());
        return;
    }

    // Determine the WASAPI buffer period for queue capacity calculation.
    audio_client->GetDevicePeriod(&default_period, &minimum_period);
    const uint32_t period_ms = static_cast<uint32_t>(
        (default_period > 0 ? default_period : kDefaultBufferPeriod100ns) / 10'000);
    const uint32_t effective_period_ms = period_ms > 0 ? period_ms : 10;

    // Query the negotiated buffer size so the CaptureQpcFallback path can
    // cap inflated GetCurrentPadding values after endpoint wakeup.
    UINT32 negotiated_buffer_frames = 0;
    if (FAILED(audio_client->GetBufferSize(&negotiated_buffer_frames)) ||
        negotiated_buffer_frames == 0) {
        // Defensive: if GetBufferSize fails, fall back to one period.
        negotiated_buffer_frames = static_cast<UINT32>(
            (static_cast<uint64_t>(effective_period_ms) *
             captured_format.sample_rate + 999) / 1000);
    }

    // Create bounded audio queue.
    AudioQueue queue(kDefaultAudioQueueBudgetMs, effective_period_ms);
    queue_capacity_.store(queue.capacity(), std::memory_order_relaxed);

    // Initialization succeeded — signal the caller.
    init_result_.store(AudioCaptureResult::Ok, std::memory_order_release);
    ::SetEvent(init_done_event_.get());

    // Track whether the next packet needs a discontinuity flag due to overrun.
    bool overrun_pending = false;

    // Start a consumer thread that invokes the callback. This ensures the
    // callback does not block the WASAPI capture loop.
    std::thread consumer_thread([this, &queue, &on_packet]() {
        while (true) {
            auto packet = queue.wait_pop();
            if (!packet.has_value()) {
                break;  // Queue closed and drained.
            }
            if (on_packet) {
                on_packet(packet.value());
            }
            delivery_barrier_.CompleteOne();
        }
    });

    // Event-driven capture loop.
    HANDLE wait_handles[2] = {audio_event.get(), stop_event_.get()};

    while (true) {
        const DWORD wait_result = ::WaitForMultipleObjects(2, wait_handles, FALSE, 2000);

        if (wait_result == WAIT_OBJECT_0 + 1) {
            // Stop event signalled.
            break;
        }

        if (wait_result == WAIT_TIMEOUT) {
            // Timeout — check if stop was requested (defensive).
            if (::WaitForSingleObject(stop_event_.get(), 0) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        if (wait_result != WAIT_OBJECT_0) {
            // Unexpected wait failure.
            platform::LogMessage(L"[Audio] WaitForMultipleObjects failed: %lu", ::GetLastError());
            capture_failures_.Increment();
            break;
        }

        // Drain all available packets from the WASAPI buffer.
        bool device_lost = false;
        while (true) {
            UINT32 frames_available = 0;
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;
            UINT32 buffered_frames_available = 0;
            const bool has_padding =
                SUCCEEDED(audio_client->GetCurrentPadding(&buffered_frames_available));

            hr = capture_client->GetBuffer(&data, &frames_available, &flags,
                                            &device_position, &qpc_position);

            // A failed call reports no frames, so it must be classified before
            // the empty-buffer exit or endpoint loss would look like silence.
            if (FAILED(hr)) {
                platform::LogMessage(L"[Audio] GetBuffer failed: 0x%08lX", hr);
                last_failure_hresult_.store(hr, std::memory_order_release);
                capture_failures_.Increment();
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    device_invalidations_.Increment();
                    device_lost = true;
                }
                break;
            }
            if (hr == AUDCLNT_S_BUFFER_EMPTY || frames_available == 0) {
                break;
            }
            if (!has_padding || buffered_frames_available < frames_available) {
                buffered_frames_available = frames_available;
            }

            LARGE_INTEGER capture_qpc{};
            const bool has_capture_qpc =
                ::QueryPerformanceCounter(&capture_qpc) != FALSE;
            const MediaTime capture_qpc_time = has_capture_qpc
                                                   ? QpcTicksToMediaTime(
                                                         capture_qpc, qpc_frequency)
                                                   : MediaTime{0};
            const uint64_t capture_qpc_100ns =
                capture_qpc_time.count() > 0
                    ? static_cast<uint64_t>(capture_qpc_time.count())
                    : 0;

            uint64_t audio_clock_position = 0;
            uint64_t audio_clock_qpc_100ns = 0;
            if (audio_clock2 &&
                FAILED(audio_clock2->GetDevicePosition(&audio_clock_position,
                                                       &audio_clock_qpc_100ns))) {
                audio_clock_position = 0;
                audio_clock_qpc_100ns = 0;
            }

            // Build the audio packet with owned data.
            AudioPacket packet;
            packet.format = captured_format;
            packet.frame_count = frames_available;

            const size_t byte_count = static_cast<size_t>(frames_available) *
                                      captured_format.block_align();

            // Handle flags.
            AudioPacketFlags pkt_flags = AudioPacketFlags::None;

            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                pkt_flags |= AudioPacketFlags::Discontinuity;
                discontinuities_.Increment();
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pkt_flags |= AudioPacketFlags::Silent;
                silent_packets_.Increment();
                // For silent packets, create a zero-filled packet.
                packet.data.resize(byte_count, 0);
            } else {
                // Copy data from the WASAPI buffer into owned storage.
                packet.data.resize(byte_count);
                if (data != nullptr && byte_count > 0) {
                    std::memcpy(packet.data.data(), data, byte_count);
                }
            }

            if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
                pkt_flags |= AudioPacketFlags::TimestampError;
            }

            if (overrun_pending) {
                pkt_flags |= AudioPacketFlags::Discontinuity;
                overrun_pending = false;
            }

            packet.device_position = device_position;

            const detail::AudioTimestampResolutionInput timestamp_input{
                qpc_epoch_100ns.count(),
                qpc_position,
                (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0,
                device_position,
                frames_available,
                buffered_frames_available,
                captured_format.sample_rate,
                audio_clock_position,
                audio_clock_frequency,
                audio_clock_qpc_100ns,
                capture_qpc_100ns,
                negotiated_buffer_frames,
            };
            const auto timestamp_resolution =
                ResolveAudioPacketTimestampImpl(timestamp_input);
            packet.timestamp = timestamp_resolution.resolved_timestamp;
            packet.timestamp_source = timestamp_resolution.source;
            if (timestamp_resolution.source != AudioTimestampSource::WasapiQpc) {
                pkt_flags |= AudioPacketFlags::TimestampError;
            }
            packet.flags = pkt_flags;

            // Release the WASAPI buffer before any blocking operation. The
            // copied packet remains valid even if release reports endpoint
            // loss, so retain it and terminate capture deterministically after
            // diagnostics/enqueue below.
            const HRESULT release_hr = capture_client->ReleaseBuffer(frames_available);
            const bool release_failed = FAILED(release_hr);
            if (release_failed) {
                platform::LogMessage(L"[Audio] ReleaseBuffer failed: 0x%08lX", release_hr);
                capture_failures_.Increment();
                if (release_hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    device_invalidations_.Increment();
                }
                device_lost = true;
            }

            AudioTimestampResolutionDiagnostic timestamp_diagnostic;
            timestamp_diagnostic.packet_index = packets_acquired_.value();
            timestamp_diagnostic.frame_count = frames_available;
            timestamp_diagnostic.buffered_frame_count = buffered_frames_available;
            timestamp_diagnostic.device_position = device_position;
            timestamp_diagnostic.raw_wasapi_qpc_100ns = qpc_position;
            timestamp_diagnostic.audio_clock_position = audio_clock_position;
            timestamp_diagnostic.audio_clock_frequency = audio_clock_frequency;
            timestamp_diagnostic.audio_clock_qpc_100ns = audio_clock_qpc_100ns;
            timestamp_diagnostic.capture_qpc_100ns = capture_qpc_100ns;
            timestamp_diagnostic.raw_wasapi_timestamp =
                timestamp_resolution.raw_wasapi_timestamp;
            timestamp_diagnostic.audio_clock_timestamp =
                timestamp_resolution.audio_clock_timestamp;
            timestamp_diagnostic.capture_fallback_timestamp =
                timestamp_resolution.capture_fallback_timestamp;
            timestamp_diagnostic.resolved_timestamp =
                timestamp_resolution.resolved_timestamp;
            timestamp_diagnostic.source = timestamp_resolution.source;
            timestamp_diagnostic.packet_flags = packet.flags;
            timestamp_diagnostic.raw_wasapi_plausible =
                timestamp_resolution.raw_wasapi_plausible;
            timestamp_diagnostic.audio_clock_correlation_valid =
                timestamp_resolution.audio_clock_correlation_valid;

            bool retained_timestamp_diagnostic = false;
            {
                const std::scoped_lock lock(timestamp_diagnostics_mutex_);
                if (timestamp_resolution_count_ <
                    kAudioTimestampDiagnosticCapacity) {
                    timestamp_resolutions_[timestamp_resolution_count_] =
                        timestamp_diagnostic;
                    ++timestamp_resolution_count_;
                    retained_timestamp_diagnostic = true;
                }
            }
            if (retained_timestamp_diagnostic) {
                platform::LogMessage(
                    L"[AudioTimestamp] packet=%llu epoch=%lld frames=%u buffered=%u device=%llu "
                    L"raw_qpc=%llu clock_pos=%llu clock_freq=%llu",
                    static_cast<unsigned long long>(timestamp_diagnostic.packet_index),
                    static_cast<long long>(qpc_epoch_100ns.count()),
                    timestamp_diagnostic.frame_count,
                    timestamp_diagnostic.buffered_frame_count,
                    static_cast<unsigned long long>(timestamp_diagnostic.device_position),
                    static_cast<unsigned long long>(timestamp_diagnostic.raw_wasapi_qpc_100ns),
                    static_cast<unsigned long long>(timestamp_diagnostic.audio_clock_position),
                    static_cast<unsigned long long>(timestamp_diagnostic.audio_clock_frequency));
                platform::LogMessage(
                    L"[AudioTimestamp] packet=%llu clock_qpc=%llu capture_qpc=%llu "
                    L"raw_rel=%lld clock_rel=%lld fallback_rel=%lld resolved=%lld "
                    L"source=%u flags=0x%X raw_ok=%u clock_ok=%u",
                    static_cast<unsigned long long>(timestamp_diagnostic.packet_index),
                    static_cast<unsigned long long>(timestamp_diagnostic.audio_clock_qpc_100ns),
                    static_cast<unsigned long long>(timestamp_diagnostic.capture_qpc_100ns),
                    static_cast<long long>(timestamp_diagnostic.raw_wasapi_timestamp.count()),
                    static_cast<long long>(timestamp_diagnostic.audio_clock_timestamp.count()),
                    static_cast<long long>(timestamp_diagnostic.capture_fallback_timestamp.count()),
                    static_cast<long long>(timestamp_diagnostic.resolved_timestamp.count()),
                    static_cast<unsigned int>(timestamp_diagnostic.source),
                    static_cast<unsigned int>(timestamp_diagnostic.packet_flags),
                    timestamp_diagnostic.raw_wasapi_plausible ? 1u : 0u,
                    timestamp_diagnostic.audio_clock_correlation_valid ? 1u : 0u);
            }

            // Update diagnostics.
            packets_acquired_.Increment();
            audio_frames_acquired_.Add(frames_available);
            bytes_acquired_.Add(byte_count);

            // Make the delivery decision under the same synchronization as
            // the Pause boundary. Packets acquired while delivery is paused
            // are explicitly discarded while WASAPI continues to drain.
            AudioQueuePushResult push_result = AudioQueuePushResult::Closed;
            const detail::AudioDeliveryEnqueueResult delivery_result =
                delivery_barrier_.TryEnqueue(
                    [&queue, &packet, &push_result](bool discontinuity) {
                        if (discontinuity) {
                            packet.flags |= AudioPacketFlags::Discontinuity;
                        }
                        push_result = queue.try_push(std::move(packet));
                        return push_result == AudioQueuePushResult::Accepted;
                    });
            if (delivery_result == detail::AudioDeliveryEnqueueResult::Rejected &&
                push_result == AudioQueuePushResult::Overrun) {
                buffer_overruns_.Increment();
                overrun_pending = true;
            }

            // Track peak queue occupancy.
            const std::size_t current_peak = queue.peak_size();
            std::size_t stored_peak = peak_queue_occupancy_.load(std::memory_order_relaxed);
            while (current_peak > stored_peak) {
                if (peak_queue_occupancy_.compare_exchange_weak(
                        stored_peak, current_peak, std::memory_order_relaxed)) {
                    break;
                }
            }

            if (release_failed) {
                break;
            }
        }

        if (device_lost) {
            platform::LogMessage(L"[Audio] Device invalidated during capture");
            break;
        }
    }

    // Stop the WASAPI client.
    audio_client->Stop();

    // Close the queue to wake the consumer, then join it.
    queue.close();
    if (consumer_thread.joinable()) {
        consumer_thread.join();
    }

    // Final peak queue occupancy.
    const std::size_t final_peak = queue.peak_size();
    std::size_t stored_peak = peak_queue_occupancy_.load(std::memory_order_relaxed);
    while (final_peak > stored_peak) {
        if (peak_queue_occupancy_.compare_exchange_weak(
                stored_peak, final_peak, std::memory_order_relaxed)) {
            break;
        }
    }

    // COM objects (capture_client, audio_client, device, enumerator) are
    // released here by their destructors before com_guard calls CoUninitialize.
}

}  // namespace

detail::AudioTimestampResolutionResult detail::ResolveAudioPacketTimestamp(
    const AudioTimestampResolutionInput& input) noexcept {
    return ResolveAudioPacketTimestampImpl(input);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<IAudioCaptureProvider>
detail::CreateWasapiEndpointCaptureProvider() {
    return std::make_unique<WasapiAudioProvider>();
}

}  // namespace easnap::media
