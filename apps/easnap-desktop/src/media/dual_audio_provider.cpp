// Easnap - production routing and dual-source WASAPI composition.

#include "media/wasapi_audio_provider.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "media/audio_delivery_barrier.h"
#include "media/audio_resampler.h"
#include "media/dual_audio_mixer.h"

namespace easnap::media {
namespace {

constexpr MediaTime kMixLookBehind{1'000'000};  // 100 ms
constexpr MediaTime kEndpointQueueBudget{5'000'000};  // 500 ms

[[nodiscard]] std::optional<int64_t> CurrentQpc100ns() noexcept {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (::QueryPerformanceFrequency(&frequency) == FALSE ||
        frequency.QuadPart <= 0 ||
        ::QueryPerformanceCounter(&counter) == FALSE) {
        return std::nullopt;
    }
    const int64_t seconds = counter.QuadPart / frequency.QuadPart;
    const int64_t remainder = counter.QuadPart % frequency.QuadPart;
    return seconds * 10'000'000 +
           (remainder * 10'000'000) / frequency.QuadPart;
}

[[nodiscard]] uint64_t SaturatingAdd(uint64_t left, uint64_t right) noexcept {
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

class AudioCaptureProvider final : public IAudioCaptureProvider {
public:
    AudioCaptureProvider() = default;
    AudioCaptureProvider(std::unique_ptr<IAudioCaptureProvider> system,
                         std::unique_ptr<IAudioCaptureProvider> microphone)
        : injected_system_(std::move(system)),
          injected_microphone_(std::move(microphone)) {}
    ~AudioCaptureProvider() override { Stop(); }

    AudioCaptureResult Start(AudioSource source, PacketCallback on_packet) override;
    void PauseDeliveryAndDrain() noexcept override;
    void ResumeDelivery() noexcept override;
    void Stop() override;
    [[nodiscard]] bool active() const override;
    [[nodiscard]] bool IsSourceAvailable(AudioSource source) const override;
    [[nodiscard]] AudioFormat negotiated_format() const override;
    [[nodiscard]] std::optional<MediaTime> clock_position() const noexcept override;
    [[nodiscard]] AudioCaptureDiagnostics diagnostics() const override;

private:
    struct EndpointQueue final {
        std::deque<AudioPacket> packets;
        uint64_t queued_frames{0};
        uint64_t capacity_frames{0};
        std::size_t peak_frames{0};
    };

    void OnEndpointPacket(detail::DualAudioInput input,
                          const AudioPacket& packet) noexcept;
    void MixerMain() noexcept;
    void Deliver(AudioPacket packet) noexcept;
    [[nodiscard]] bool EndpointFailed() const noexcept;
    void StopDual() noexcept;

    AudioSource source_{AudioSource::None};
    std::unique_ptr<IAudioCaptureProvider> single_;
    std::unique_ptr<IAudioCaptureProvider> system_;
    std::unique_ptr<IAudioCaptureProvider> microphone_;
    std::unique_ptr<IAudioCaptureProvider> injected_system_;
    std::unique_ptr<IAudioCaptureProvider> injected_microphone_;
    AudioFormat mix_format_{};
    std::unique_ptr<detail::DualAudioMixer> mixer_;
    detail::AudioDeliveryBarrier delivery_barrier_;
    PacketCallback on_packet_;
    std::thread mixer_thread_;
    mutable std::mutex mixer_mutex_;
    std::condition_variable mixer_changed_;
    std::condition_variable flush_completed_;
    std::condition_variable mixer_initialized_;
    EndpointQueue system_queue_;
    EndpointQueue microphone_queue_;
    int64_t qpc_epoch_100ns_{0};
    bool mixer_running_{false};
    bool mixing_enabled_{false};
    bool accepting_endpoint_packets_{false};
    bool flush_requested_{false};
    bool flush_done_{false};
    bool mixer_init_done_{false};
    bool mixer_init_ok_{false};
    std::atomic<long> mixer_failure_hresult_{0};
    std::atomic<uint64_t> system_resampler_failures_{0};
    std::atomic<uint64_t> microphone_resampler_failures_{0};
    std::atomic<uint64_t> endpoint_queue_overflows_{0};
    std::atomic<bool> dual_active_{false};
    std::atomic<bool> dual_failed_{false};
};

AudioCaptureResult AudioCaptureProvider::Start(AudioSource source,
                                                PacketCallback on_packet) {
    if (active()) {
        return AudioCaptureResult::AlreadyActive;
    }
    if (source == AudioSource::None || !on_packet) {
        return AudioCaptureResult::InvalidSource;
    }
    source_ = source;
    if (source != AudioSource::Both) {
        single_ = detail::CreateWasapiEndpointCaptureProvider();
        if (!single_) {
            return AudioCaptureResult::InitializationFailed;
        }
        const AudioCaptureResult result =
            single_->Start(source, std::move(on_packet));
        return result;
    }

    const std::optional<int64_t> epoch = CurrentQpc100ns();
    if (!epoch.has_value()) {
        source_ = AudioSource::None;
        return AudioCaptureResult::InitializationFailed;
    }
    qpc_epoch_100ns_ = *epoch;
    delivery_barrier_.Reset();
    dual_failed_.store(false, std::memory_order_release);
    mixer_failure_hresult_.store(0, std::memory_order_release);
    system_resampler_failures_.store(0, std::memory_order_release);
    microphone_resampler_failures_.store(0, std::memory_order_release);
    endpoint_queue_overflows_.store(0, std::memory_order_release);
    on_packet_ = std::move(on_packet);
    system_ = injected_system_ ? std::move(injected_system_)
                               : detail::CreateWasapiEndpointCaptureProvider();
    microphone_ = injected_microphone_
                      ? std::move(injected_microphone_)
                      : detail::CreateWasapiEndpointCaptureProvider();
    mixer_.reset();
    mix_format_ = AudioFormat{48'000, 2, 32, true, 0x3};
    if (!system_ || !microphone_) {
        StopDual();
        return AudioCaptureResult::InitializationFailed;
    }
    const AudioCaptureResult system_result = system_->Start(
        AudioSource::SystemAudio,
        [this](const AudioPacket& packet) {
            OnEndpointPacket(detail::DualAudioInput::SystemAudio, packet);
        });
    if (system_result != AudioCaptureResult::Ok) {
        StopDual();
        return system_result == AudioCaptureResult::SourceUnavailable
                   ? AudioCaptureResult::SystemSourceUnavailable
                   : AudioCaptureResult::SystemInitializationFailed;
    }
    const AudioCaptureResult microphone_result = microphone_->Start(
        AudioSource::Microphone,
        [this](const AudioPacket& packet) {
            OnEndpointPacket(detail::DualAudioInput::Microphone, packet);
        });
    if (microphone_result != AudioCaptureResult::Ok) {
        StopDual();
        return microphone_result == AudioCaptureResult::SourceUnavailable
                   ? AudioCaptureResult::MicrophoneUnavailable
                   : AudioCaptureResult::MicrophoneInitializationFailed;
    }

    try {
        mixer_ = std::make_unique<detail::DualAudioMixer>(mix_format_);
        if (!mixer_->valid()) {
            StopDual();
            return AudioCaptureResult::InitializationFailed;
        }
        {
            const std::scoped_lock lock(mixer_mutex_);
            system_queue_ = {};
            microphone_queue_ = {};
            system_queue_.capacity_frames =
                system_->negotiated_format().sample_rate / 2;
            microphone_queue_.capacity_frames =
                microphone_->negotiated_format().sample_rate / 2;
            mixer_running_ = true;
            mixing_enabled_ = false;
            accepting_endpoint_packets_ = true;
            flush_requested_ = false;
            flush_done_ = false;
            mixer_init_done_ = false;
            mixer_init_ok_ = false;
        }
        mixer_thread_ = std::thread(&AudioCaptureProvider::MixerMain, this);
        {
            std::unique_lock lock(mixer_mutex_);
            mixer_initialized_.wait(lock, [this] { return mixer_init_done_; });
            if (!mixer_init_ok_) {
                lock.unlock();
                StopDual();
                return AudioCaptureResult::InitializationFailed;
            }
            mixing_enabled_ = true;
        }
    } catch (...) {
        StopDual();
        return AudioCaptureResult::InitializationFailed;
    }

    dual_active_.store(true, std::memory_order_release);
    mixer_changed_.notify_all();
    return AudioCaptureResult::Ok;
}

bool AudioCaptureProvider::active() const {
    if (source_ == AudioSource::Both) {
        return dual_active_.load(std::memory_order_acquire) &&
               !dual_failed_.load(std::memory_order_acquire);
    }
    return single_ != nullptr && single_->active();
}

bool AudioCaptureProvider::IsSourceAvailable(AudioSource source) const {
    if (source == AudioSource::None) {
        return false;
    }
    auto probe = detail::CreateWasapiEndpointCaptureProvider();
    if (!probe) {
        return false;
    }
    if (source != AudioSource::Both) {
        return probe->IsSourceAvailable(source);
    }
    return probe->IsSourceAvailable(AudioSource::SystemAudio) &&
           probe->IsSourceAvailable(AudioSource::Microphone);
}

AudioFormat AudioCaptureProvider::negotiated_format() const {
    if (source_ == AudioSource::Both) {
        return dual_active_.load(std::memory_order_acquire) ? mix_format_
                                                            : AudioFormat{};
    }
    return single_ != nullptr ? single_->negotiated_format() : AudioFormat{};
}

std::optional<MediaTime> AudioCaptureProvider::clock_position() const noexcept {
    if (source_ != AudioSource::Both) {
        return single_ != nullptr ? single_->clock_position() : std::nullopt;
    }
    const std::optional<int64_t> now = CurrentQpc100ns();
    if (!now.has_value() || qpc_epoch_100ns_ <= 0 || *now < qpc_epoch_100ns_) {
        return std::nullopt;
    }
    return MediaTime{*now - qpc_epoch_100ns_};
}

void AudioCaptureProvider::OnEndpointPacket(
    detail::DualAudioInput input, const AudioPacket& packet) noexcept {
    IAudioCaptureProvider* endpoint =
        input == detail::DualAudioInput::SystemAudio ? system_.get()
                                                     : microphone_.get();
    if (endpoint == nullptr) {
        return;
    }
    const AudioCaptureDiagnostics endpoint_diagnostics = endpoint->diagnostics();
    if (endpoint_diagnostics.qpc_epoch_100ns <= 0 ||
        packet.timestamp < MediaTime::zero()) {
        dual_failed_.store(true, std::memory_order_release);
        mixer_changed_.notify_all();
        return;
    }
    AudioPacket normalized = packet;
    const int64_t relative = endpoint_diagnostics.qpc_epoch_100ns -
                             qpc_epoch_100ns_ + packet.timestamp.count();
    if (relative < 0) {
        dual_failed_.store(true, std::memory_order_release);
        mixer_changed_.notify_all();
        return;
    }
    normalized.timestamp = MediaTime{relative};
    {
        const std::scoped_lock lock(mixer_mutex_);
        if (!mixer_running_ || !accepting_endpoint_packets_) {
            return;
        }
        EndpointQueue& queue =
            input == detail::DualAudioInput::SystemAudio ? system_queue_
                                                         : microphone_queue_;
        if (normalized.frame_count > queue.capacity_frames -
                                         (std::min)(queue.queued_frames,
                                                    queue.capacity_frames)) {
            endpoint_queue_overflows_.fetch_add(1, std::memory_order_relaxed);
            dual_failed_.store(true, std::memory_order_release);
            mixer_failure_hresult_.store(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),
                                         std::memory_order_release);
            return;
        }
        queue.queued_frames += normalized.frame_count;
        queue.packets.push_back(std::move(normalized));
        queue.peak_frames = (std::max)(
            queue.peak_frames, static_cast<std::size_t>(queue.queued_frames));
    }
    mixer_changed_.notify_all();
}

bool AudioCaptureProvider::EndpointFailed() const noexcept {
    if (!system_ || !microphone_) {
        return false;
    }
    const AudioCaptureDiagnostics system = system_->diagnostics();
    const AudioCaptureDiagnostics microphone = microphone_->diagnostics();
    return system.capture_failures != 0 || system.device_invalidations != 0 ||
           microphone.capture_failures != 0 ||
           microphone.device_invalidations != 0;
}

void AudioCaptureProvider::Deliver(AudioPacket packet) noexcept {
    const detail::AudioDeliveryEnqueueResult accepted =
        delivery_barrier_.TryEnqueue([&packet](bool discontinuity) {
            if (discontinuity) {
                packet.flags |= AudioPacketFlags::Discontinuity;
            }
            return true;
        });
    if (accepted != detail::AudioDeliveryEnqueueResult::Accepted) {
        return;
    }
    try {
        if (on_packet_) {
            on_packet_(packet);
        }
    } catch (...) {
        dual_failed_.store(true, std::memory_order_release);
    }
    delivery_barrier_.CompleteOne();
}

void AudioCaptureProvider::MixerMain() noexcept {
    const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        detail::AudioResampler system_converter;
        detail::AudioResampler microphone_converter;
        bool initialized = SUCCEEDED(com_result) && system_ && microphone_ &&
                           system_converter.Initialize(
                               system_->negotiated_format(), mix_format_) &&
                           microphone_converter.Initialize(
                               microphone_->negotiated_format(), mix_format_);
        if (!initialized) {
            const long converter_error =
                system_converter.last_hresult() != 0
                    ? system_converter.last_hresult()
                    : microphone_converter.last_hresult();
            mixer_failure_hresult_.store(
                converter_error != 0 ? converter_error : com_result,
                std::memory_order_release);
        }
        {
            const std::scoped_lock lock(mixer_mutex_);
            mixer_init_ok_ = initialized;
            mixer_init_done_ = true;
            if (!initialized) {
                mixer_running_ = false;
            }
        }
        mixer_initialized_.notify_all();

        bool take_system = true;
        while (initialized) {
            // 1. Drain all available mixed chunks up to the look-behind watermark.
            // Executing this in every loop iteration guarantees that continuous input
            // arrivals can never starve mixed-output delivery or overflow the bounded buffer.
            std::vector<AudioPacket> mixed_chunks;
            {
                const std::scoped_lock lock(mixer_mutex_);
                if (!mixer_running_) {
                    break;
                }
                if (mixing_enabled_ && mixer_) {
                    const std::optional<int64_t> now = CurrentQpc100ns();
                    if (now.has_value() &&
                        *now >= qpc_epoch_100ns_ + kMixLookBehind.count()) {
                        const MediaTime watermark{
                            *now - qpc_epoch_100ns_ - kMixLookBehind.count()};
                        AudioPacket mixed;
                        while (mixer_->PopUntil(watermark, mixed)) {
                            mixed_chunks.push_back(std::move(mixed));
                        }
                    }
                }
            }
            for (auto& mixed : mixed_chunks) {
                Deliver(std::move(mixed));
            }

            // 2. Check for flush request (pause / drain).
            bool flush_now = false;
            {
                const std::scoped_lock lock(mixer_mutex_);
                if (!mixer_running_) {
                    break;
                }
                if (flush_requested_) {
                    flush_now = true;
                }
            }

            if (flush_now) {
                std::vector<AudioPacket> system_tail;
                std::vector<AudioPacket> microphone_tail;
                const bool drained = system_converter.Drain(system_tail) &&
                                     microphone_converter.Drain(microphone_tail);
                if (!drained) {
                    const HRESULT failure = FAILED(system_converter.last_hresult())
                                                ? system_converter.last_hresult()
                                                : microphone_converter.last_hresult();
                    dual_failed_.store(true, std::memory_order_release);
                    mixer_failure_hresult_.store(failure,
                                                 std::memory_order_release);
                }
                MediaTime target{0};
                {
                    const std::scoped_lock lock(mixer_mutex_);
                    for (const AudioPacket& packet : system_tail) {
                        if (mixer_) {
                            const auto push_res =
                                mixer_->Push(detail::DualAudioInput::SystemAudio, packet);
                            if (push_res == detail::DualAudioMixerPushResult::TruncatedOverflow ||
                                push_res == detail::DualAudioMixerPushResult::CapacityExceeded) {
                                dual_failed_.store(true, std::memory_order_release);
                            }
                        }
                    }
                    for (const AudioPacket& packet : microphone_tail) {
                        if (mixer_) {
                            const auto push_res =
                                mixer_->Push(detail::DualAudioInput::Microphone, packet);
                            if (push_res == detail::DualAudioMixerPushResult::TruncatedOverflow ||
                                push_res == detail::DualAudioMixerPushResult::CapacityExceeded) {
                                dual_failed_.store(true, std::memory_order_release);
                            }
                        }
                    }
                    if (mixer_) {
                        target = mixer_->latest_buffered_end();
                    }
                }
                while (drained) {
                    AudioPacket mixed;
                    {
                        const std::scoped_lock lock(mixer_mutex_);
                        if (!mixer_ || !mixer_->PopUntil(target, mixed)) {
                            break;
                        }
                    }
                    Deliver(std::move(mixed));
                }
                {
                    const std::scoped_lock lock(mixer_mutex_);
                    if (mixer_) {
                        mixer_->Reset();
                    }
                    flush_requested_ = false;
                    flush_done_ = true;
                }
                flush_completed_.notify_all();
                continue;
            }

            // 3. Dequeue one packet from available inputs (fair alternation).
            AudioPacket source_packet;
            detail::DualAudioInput input = detail::DualAudioInput::SystemAudio;
            bool have_source_packet = false;
            {
                const std::scoped_lock lock(mixer_mutex_);
                if (!mixer_running_) {
                    break;
                }
                EndpointQueue* first = take_system ? &system_queue_
                                                   : &microphone_queue_;
                EndpointQueue* second = take_system ? &microphone_queue_
                                                    : &system_queue_;
                if (!first->packets.empty() || !second->packets.empty()) {
                    EndpointQueue* selected = !first->packets.empty() ? first : second;
                    input = selected == &system_queue_
                                ? detail::DualAudioInput::SystemAudio
                                : detail::DualAudioInput::Microphone;
                    source_packet = std::move(selected->packets.front());
                    selected->packets.pop_front();
                    selected->queued_frames -= source_packet.frame_count;
                    have_source_packet = true;
                    take_system = input != detail::DualAudioInput::SystemAudio;
                }
            }

            if (have_source_packet) {
                std::vector<AudioPacket> converted;
                detail::AudioResampler& converter =
                    input == detail::DualAudioInput::SystemAudio
                        ? system_converter
                        : microphone_converter;
                if (!converter.Convert(source_packet, converted)) {
                    if (input == detail::DualAudioInput::SystemAudio) {
                        system_resampler_failures_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        microphone_resampler_failures_.fetch_add(1, std::memory_order_relaxed);
                    }
                    dual_failed_.store(true, std::memory_order_release);
                    mixer_failure_hresult_.store(converter.last_hresult(),
                                                 std::memory_order_release);
                } else {
                    const std::scoped_lock lock(mixer_mutex_);
                    for (const AudioPacket& packet : converted) {
                        if (!mixer_) {
                            break;
                        }
                        const auto push_result = mixer_->Push(input, packet);
                        if (push_result == detail::DualAudioMixerPushResult::TruncatedOverflow ||
                            push_result == detail::DualAudioMixerPushResult::CapacityExceeded) {
                            dual_failed_.store(true, std::memory_order_release);
                            mixer_failure_hresult_.store(
                                HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),
                                std::memory_order_release);
                            break;
                        }
                        if (push_result == detail::DualAudioMixerPushResult::InvalidPacket) {
                            dual_failed_.store(true, std::memory_order_release);
                            mixer_failure_hresult_.store(E_INVALIDARG,
                                                         std::memory_order_release);
                            break;
                        }
                        // LateDiscarded is recoverable timing reconciliation; do not fail!
                    }
                }
                if (EndpointFailed()) {
                    dual_failed_.store(true, std::memory_order_release);
                }
                continue;
            }

            if (EndpointFailed()) {
                dual_failed_.store(true, std::memory_order_release);
            }

            std::unique_lock lock(mixer_mutex_);
            mixer_changed_.wait_for(lock, std::chrono::milliseconds(5), [this] {
                return !mixer_running_ || flush_requested_ ||
                       !system_queue_.packets.empty() ||
                       !microphone_queue_.packets.empty();
            });
        }
    }
    if (SUCCEEDED(com_result)) {
        ::CoUninitialize();
    }
}

void AudioCaptureProvider::PauseDeliveryAndDrain() noexcept {
    if (source_ != AudioSource::Both) {
        if (single_) {
            single_->PauseDeliveryAndDrain();
        }
        return;
    }
    if (system_) {
        system_->PauseDeliveryAndDrain();
    }
    if (microphone_) {
        microphone_->PauseDeliveryAndDrain();
    }
    {
        std::unique_lock lock(mixer_mutex_);
        if (mixer_running_) {
            mixing_enabled_ = false;
            accepting_endpoint_packets_ = false;
            flush_done_ = false;
            flush_requested_ = true;
            mixer_changed_.notify_all();
            flush_completed_.wait(lock, [this] {
                return flush_done_ || !mixer_running_;
            });
        }
    }
    delivery_barrier_.PauseAndDrain();
}

void AudioCaptureProvider::ResumeDelivery() noexcept {
    if (source_ != AudioSource::Both) {
        if (single_) {
            single_->ResumeDelivery();
        }
        return;
    }
    delivery_barrier_.Resume();
    {
        const std::scoped_lock lock(mixer_mutex_);
        accepting_endpoint_packets_ = true;
        mixing_enabled_ = true;
    }
    if (system_) {
        system_->ResumeDelivery();
    }
    if (microphone_) {
        microphone_->ResumeDelivery();
    }
    mixer_changed_.notify_all();
}

void AudioCaptureProvider::StopDual() noexcept {
    {
        const std::scoped_lock lock(mixer_mutex_);
        mixer_running_ = false;
        mixing_enabled_ = false;
        accepting_endpoint_packets_ = false;
    }
    mixer_changed_.notify_all();
    flush_completed_.notify_all();
    if (mixer_thread_.joinable()) {
        mixer_thread_.join();
    }
    if (system_) {
        system_->Stop();
    }
    if (microphone_) {
        microphone_->Stop();
    }
    on_packet_ = {};
    dual_active_.store(false, std::memory_order_release);
}

void AudioCaptureProvider::Stop() {
    if (source_ != AudioSource::Both) {
        if (single_) {
            single_->Stop();
        }
        return;
    }
    PauseDeliveryAndDrain();
    StopDual();
}

AudioCaptureDiagnostics AudioCaptureProvider::diagnostics() const {
    if (source_ != AudioSource::Both) {
        return single_ != nullptr ? single_->diagnostics()
                                  : AudioCaptureDiagnostics{};
    }
    AudioCaptureDiagnostics result;
    result.qpc_epoch_100ns = qpc_epoch_100ns_;
    const AudioCaptureDiagnostics system =
        system_ != nullptr ? system_->diagnostics() : AudioCaptureDiagnostics{};
    const AudioCaptureDiagnostics microphone =
        microphone_ != nullptr ? microphone_->diagnostics()
                               : AudioCaptureDiagnostics{};
    const long mixer_hresult =
        mixer_failure_hresult_.load(std::memory_order_acquire);
    result.last_failure_hresult =
        system.last_failure_hresult != 0
            ? system.last_failure_hresult
            : (microphone.last_failure_hresult != 0
                   ? microphone.last_failure_hresult
                   : mixer_hresult);
    result.packets_acquired =
        SaturatingAdd(system.packets_acquired, microphone.packets_acquired);
    result.audio_frames_acquired = SaturatingAdd(
        system.audio_frames_acquired, microphone.audio_frames_acquired);
    result.bytes_acquired =
        SaturatingAdd(system.bytes_acquired, microphone.bytes_acquired);
    result.silent_packets =
        SaturatingAdd(system.silent_packets, microphone.silent_packets);
    result.discontinuities =
        SaturatingAdd(system.discontinuities, microphone.discontinuities);
    result.device_invalidations = SaturatingAdd(
        system.device_invalidations, microphone.device_invalidations);
    result.buffer_overruns =
        SaturatingAdd(system.buffer_overruns, microphone.buffer_overruns);
    result.capture_failures =
        SaturatingAdd(system.capture_failures, microphone.capture_failures);
    result.system_capture_failures = SaturatingAdd(
        system.capture_failures, system.device_invalidations);
    result.microphone_capture_failures = SaturatingAdd(
        microphone.capture_failures, microphone.device_invalidations);
    const detail::AudioDeliverySnapshot delivery = delivery_barrier_.snapshot();
    result.delivery_packets_accepted = delivery.accepted_sequence;
    result.delivery_packets_completed = delivery.completed_sequence;
    result.delivery_paused_discards = delivery.paused_discards;
    detail::DualAudioMixerDiagnostics mixer;
    uint64_t endpoint_queue_capacity = 0;
    std::size_t endpoint_queue_peak = 0;
    {
        const std::scoped_lock lock(mixer_mutex_);
        if (mixer_) {
            mixer = mixer_->diagnostics();
        }
        endpoint_queue_capacity =
            SaturatingAdd(system_queue_.capacity_frames,
                          microphone_queue_.capacity_frames);
        endpoint_queue_peak = system_queue_.peak_frames +
                              microphone_queue_.peak_frames;
    }
    result.system_resampler_failures =
        system_resampler_failures_.load(std::memory_order_relaxed);
    result.microphone_resampler_failures =
        microphone_resampler_failures_.load(std::memory_order_relaxed);
    result.endpoint_queue_overflows =
        endpoint_queue_overflows_.load(std::memory_order_relaxed);
    result.mixer_capacity_failures = mixer.capacity_failures;
    result.mixer_invalid_packets = mixer.invalid_packets;
    result.mixer_late_discards = mixer.packets_dropped_late;

    // Only genuine failures increment capture_failures (late packet discards are normal reconciliation).
    const uint64_t internal_mixer_failures =
        SaturatingAdd(result.system_resampler_failures,
                      SaturatingAdd(result.microphone_resampler_failures,
                                    SaturatingAdd(result.endpoint_queue_overflows,
                                                  SaturatingAdd(result.mixer_capacity_failures,
                                                                result.mixer_invalid_packets))));
    result.capture_failures =
        SaturatingAdd(result.capture_failures, internal_mixer_failures);
    if (mixer_hresult != 0 && result.capture_failures == 0) {
        result.capture_failures = 1;
    }
    result.queue_capacity = static_cast<std::size_t>(SaturatingAdd(
        endpoint_queue_capacity,
        detail::DualAudioMixer::kDefaultCapacityFrames));
    result.peak_queue_occupancy = endpoint_queue_peak +
                                  mixer.peak_buffered_frames;
    return result;
}

}  // namespace

std::unique_ptr<IAudioCaptureProvider> CreateWasapiAudioCaptureProvider() {
    return std::make_unique<AudioCaptureProvider>();
}

std::unique_ptr<IAudioCaptureProvider>
detail::CreateDualAudioCaptureProviderForTesting(
    std::unique_ptr<IAudioCaptureProvider> system,
    std::unique_ptr<IAudioCaptureProvider> microphone) {
    return std::make_unique<AudioCaptureProvider>(std::move(system),
                                                  std::move(microphone));
}

}  // namespace easnap::media
