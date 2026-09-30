// Easnap - bounded audio/video recording session orchestration.

#include "recording/recording_session.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "capture/capture_provider.h"
#include "media/audio_queue.h"
#include "media/media_foundation_writer.h"
#include "platform/platform_utils.h"

namespace easnap::recording {
namespace {

constexpr std::size_t kMaximumDiagnosticCharacters = 512;
constexpr media::MediaTime kAudioStreamInterleaveLag{
    static_cast<int64_t>(media::kDefaultAudioQueueBudgetMs + 250) * 10'000};
constexpr media::MediaTime kMaximumAudioWriterLead{1'000'000};  // 100 ms

std::wstring BoundedDiagnostic(std::wstring message) {
    if (message.size() > kMaximumDiagnosticCharacters) {
        message.resize(kMaximumDiagnosticCharacters);
    }
    return message;
}

uint64_t SaturatingMultiply(uint64_t left, uint64_t right) noexcept {
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    return right != 0 && left > maximum / right ? maximum : left * right;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) noexcept {
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

uint64_t TimeToAudioFramesCeiling(media::MediaTime time,
                                  uint32_t sample_rate) noexcept {
    if (time <= media::MediaTime::zero() || sample_rate == 0) {
        return 0;
    }
    const uint64_t ticks = static_cast<uint64_t>(time.count());
    const uint64_t seconds = ticks / 10'000'000ULL;
    const uint64_t remainder = ticks % 10'000'000ULL;
    return seconds * sample_rate +
           (remainder * sample_rate + 9'999'999ULL) / 10'000'000ULL;
}

media::MediaTime AudioFramesToTime(uint64_t frames,
                                   uint32_t sample_rate) noexcept {
    const uint64_t seconds = frames / sample_rate;
    const uint64_t remainder = frames % sample_rate;
    return media::MediaTime{static_cast<int64_t>(
        seconds * 10'000'000ULL +
        (remainder * 10'000'000ULL) / sample_rate)};
}

bool SameAudioFormat(const media::AudioFormat& left,
                     const media::AudioFormat& right) noexcept {
    return left.sample_rate == right.sample_rate && left.channels == right.channels &&
           left.bits_per_sample == right.bits_per_sample &&
           left.is_float == right.is_float && left.channel_mask == right.channel_mask;
}

std::wstring AudioStartFailure(media::AudioCaptureResult result) {
    switch (result) {
        case media::AudioCaptureResult::SourceUnavailable:
            return L"The requested audio capture endpoint is unavailable";
        case media::AudioCaptureResult::InitializationFailed:
            return L"Initializing the requested WASAPI audio capture endpoint failed";
        case media::AudioCaptureResult::AlreadyActive:
            return L"The WASAPI audio capture provider was already active";
        case media::AudioCaptureResult::InvalidSource:
            return L"The requested audio source is not supported";
        case media::AudioCaptureResult::SystemSourceUnavailable:
            return L"System Audio is unavailable; Both requires both audio endpoints";
        case media::AudioCaptureResult::MicrophoneUnavailable:
            return L"Microphone is unavailable; Both requires both audio endpoints";
        case media::AudioCaptureResult::SystemInitializationFailed:
            return L"Initializing System Audio for Both failed";
        case media::AudioCaptureResult::MicrophoneInitializationFailed:
            return L"Initializing Microphone for Both failed";
        case media::AudioCaptureResult::Ok:
            break;
    }
    return L"Starting WASAPI audio capture failed unexpectedly";
}

}  // namespace

RecordingSession::RecordingSession(
    media::ITemporarySessionStore& store,
    media::RecordingOptions options,
    std::unique_ptr<capture::IRegionCaptureProvider> provider,
    std::unique_ptr<media::IAudioCaptureProvider> audio_provider)
    : store_(store),
      options_(options),
      frame_queue_(capture::kDefaultVideoFrameQueueCapacity),
      capture_worker_(std::make_unique<capture::RecordingCaptureWorker>(
          std::move(provider), options_, frame_queue_)),
      audio_provider_(std::move(audio_provider)) {}

RecordingSession::~RecordingSession() {
    Abort();
}

bool RecordingSession::Start() {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (state() != RecordingSessionState::Ready) {
        return false;
    }
    state_.store(RecordingSessionState::Starting, std::memory_order_release);
    written_frames_.Reset();
    written_audio_packets_.Reset();
    written_audio_frames_.Reset();
    silence_writes_.Reset();
    writer_submission_timing_.Reset();
    audio_writer_submission_timing_.Reset();
    silence_write_submission_timing_.Reset();
    audio_backpressure_wait_timing_.Reset();
    ResetAudioTimelineDiagnostics();
    audio_accepting_.store(false, std::memory_order_release);
    audio_format_ = {};
    silence_write_position_ = media::MediaTime::zero();
    committed_audio_end_ = media::MediaTime::zero();
    pending_audio_start_.reset();
    last_video_writer_end_100ns_.store(0, std::memory_order_release);

    try {
        temporary_session_ = store_.CreateSession();
        if (!temporary_session_.has_value() || !temporary_session_->valid()) {
            RollBackStartup(L"Creating the restricted temporary recording session failed");
            return false;
        }

        const std::wstring output_path = temporary_session_->recording_path();
        writer_ = media::CreateMediaFoundationWriter();
        if (writer_ == nullptr) {
            RollBackStartup(L"Creating the Media Foundation writer failed");
            return false;
        }

        if (AudioEnabled()) {
            if (audio_provider_ == nullptr) {
                RollBackStartup(L"Creating the WASAPI audio capture provider failed");
                return false;
            }
            all_worker_threads_stopped_.store(false, std::memory_order_release);
            const media::AudioCaptureResult audio_result = audio_provider_->Start(
                options_.audio, [this](const media::AudioPacket& packet) {
                    OnAudioPacket(packet);
                });
            if (audio_result != media::AudioCaptureResult::Ok) {
                RollBackStartup(AudioStartFailure(audio_result));
                return false;
            }
            audio_format_ = audio_provider_->negotiated_format();
            if (!audio_format_.valid()) {
                RollBackStartup(L"WASAPI returned an invalid negotiated audio format");
                return false;
            }
            if (!audio_timeline_.Reset(audio_format_)) {
                RollBackStartup(L"Initializing the sample-accurate audio timeline failed");
                return false;
            }
        }

        media::RecordingOptions working_options = options_;
        // Both product formats use the accepted H.264 MP4 capture artifact.
        // GIF keeps its final-format policy in options_ but never adds audio to
        // this internal working representation.
        working_options.format = media::RecordingFormat::Mp4;
        if (!writer_->Open(output_path, working_options,
                           AudioEnabled() ? &audio_format_ : nullptr)) {
            std::wstring message = writer_->last_error();
            if (message.empty()) {
                message = L"Opening the Media Foundation writer failed";
            }
            RollBackStartup(std::move(message));
            return false;
        }

        consumer_running_.store(true, std::memory_order_release);
        all_worker_threads_stopped_.store(false, std::memory_order_release);
        try {
            consumer_thread_ = std::thread(&RecordingSession::ConsumerMain, this);
        } catch (...) {
            consumer_running_.store(false, std::memory_order_release);
            RollBackStartup(L"Starting the bounded video consumer thread failed");
            return false;
        }

        if (!capture_worker_->Start()) {
            RollBackStartup(L"Starting the desktop capture worker failed");
            return false;
        }
        if (AudioEnabled() && !BeginAudioSegment()) {
            RollBackStartup(L"Anchoring the audio stream to recording active time failed");
            return false;
        }
    } catch (const std::bad_alloc&) {
        RollBackStartup(L"Allocating recording-session state failed");
        return false;
    } catch (...) {
        RollBackStartup(L"Starting the recording session failed unexpectedly");
        return false;
    }

    RecordingSessionState expected = RecordingSessionState::Starting;
    if (!state_.compare_exchange_strong(expected, RecordingSessionState::Recording,
                                        std::memory_order_acq_rel)) {
        StopWorkersAndJoin();
        if (writer_ != nullptr) {
            writer_->Abort();
            writer_.reset();
        }
        all_worker_threads_stopped_.store(true, std::memory_order_release);
        return false;
    }
    SetAudioAccepting(true);
    platform::LogMessage(L"Recording session started");
    return true;
}

bool RecordingSession::Pause() {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (state() != RecordingSessionState::Recording) {
        return false;
    }

    // Close the provider delivery boundary while video and session audio
    // acceptance are still active. This lets every PCM packet accepted before
    // Pause clear the A/V writer gate instead of being discarded in the
    // provider queue after acceptance is disabled.
    if (AudioEnabled() && audio_provider_ != nullptr) {
        audio_provider_->PauseDeliveryAndDrain();
    }
    SetAudioAccepting(false);
    if (!capture_worker_->Pause()) {
        SetAudioAccepting(true);
        if (AudioEnabled() && audio_provider_ != nullptr) {
            audio_provider_->ResumeDelivery();
        }
        return false;
    }
    state_.store(RecordingSessionState::Paused, std::memory_order_release);
    return true;
}

bool RecordingSession::Resume() {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (state() != RecordingSessionState::Paused || !capture_worker_->Resume()) {
        return false;
    }
    if (AudioEnabled() && !BeginAudioSegment()) {
        static_cast<void>(capture_worker_->Pause());
        RecordFailure(L"Re-anchoring audio after recording resume failed");
        return false;
    }
    SetAudioAccepting(true);
    if (AudioEnabled() && audio_provider_ != nullptr) {
        audio_provider_->ResumeDelivery();
    }
    state_.store(RecordingSessionState::Recording, std::memory_order_release);
    return true;
}

bool RecordingSession::Stop() {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    const RecordingSessionState initial_state = state();
    if (initial_state == RecordingSessionState::Finalized) {
        return true;
    }
    if (initial_state != RecordingSessionState::Recording &&
        initial_state != RecordingSessionState::Paused &&
        initial_state != RecordingSessionState::Failed) {
        return false;
    }
    if (initial_state != RecordingSessionState::Failed) {
        state_.store(RecordingSessionState::Stopping, std::memory_order_release);
    }

    StopWorkersAndJoin();
    if (capture_worker_->failed()) {
        RecordFailure(L"The desktop capture worker failed unexpectedly");
    }
    if (AudioEnabled() && audio_provider_ != nullptr) {
        const media::AudioCaptureDiagnostics audio = audio_provider_->diagnostics();
        if (audio.capture_failures != 0 || audio.device_invalidations != 0) {
            platform::LogMessage(
                L"[AudioCapture] Failure detected: hr=0x%08lX, capture_failures=%llu, "
                L"sys_fail=%llu, mic_fail=%llu, sys_resamp=%llu, mic_resamp=%llu, "
                L"q_overflow=%llu, mix_cap=%llu, mix_inv=%llu, late_disc=%llu, dev_inval=%llu",
                static_cast<unsigned long>(audio.last_failure_hresult),
                static_cast<unsigned long long>(audio.capture_failures),
                static_cast<unsigned long long>(audio.system_capture_failures),
                static_cast<unsigned long long>(audio.microphone_capture_failures),
                static_cast<unsigned long long>(audio.system_resampler_failures),
                static_cast<unsigned long long>(audio.microphone_resampler_failures),
                static_cast<unsigned long long>(audio.endpoint_queue_overflows),
                static_cast<unsigned long long>(audio.mixer_capacity_failures),
                static_cast<unsigned long long>(audio.mixer_invalid_packets),
                static_cast<unsigned long long>(audio.mixer_late_discards),
                static_cast<unsigned long long>(audio.device_invalidations));
            if (options_.audio == media::AudioSource::Both &&
                audio.system_capture_failures != 0 &&
                audio.microphone_capture_failures == 0) {
                RecordFailure(L"System Audio failed during Both recording");
            } else if (options_.audio == media::AudioSource::Both &&
                       audio.microphone_capture_failures != 0 &&
                       audio.system_capture_failures == 0) {
                RecordFailure(L"Microphone failed during Both recording");
            } else {
                RecordFailure(L"The audio capture or mixing worker failed unexpectedly");
            }
        }
    }

    if (state() != RecordingSessionState::Failed &&
        !FinalizeAudioTimeline(capture_worker_->active_duration())) {
        if (state() != RecordingSessionState::Failed) {
            RecordFailure(L"Completing the active-time audio timeline failed");
        }
    }

    if (state() == RecordingSessionState::Failed || writer_ == nullptr) {
        if (writer_ != nullptr) {
            writer_->Abort();
            writer_.reset();
        }
        all_worker_threads_stopped_.store(true, std::memory_order_release);
        return false;
    }

    std::wstring completed_path;
    const bool finalized = writer_->Finalize(completed_path);
    if (!finalized) {
        std::wstring message = writer_->last_error();
        if (message.empty()) {
            message = L"Finalizing the Media Foundation writer failed";
        }
        RecordFailure(std::move(message));
        writer_->Abort();
    } else if (!temporary_session_.has_value() ||
               completed_path != temporary_session_->recording_path()) {
        RecordFailure(L"The finalized recording path did not match its temporary session");
    } else {
        const std::scoped_lock result_lock(result_mutex_);
        finalized_path_ = std::move(completed_path);
    }
    writer_.reset();
    all_worker_threads_stopped_.store(true, std::memory_order_release);

    if (!finalized || state() == RecordingSessionState::Failed) {
        return false;
    }
    state_.store(RecordingSessionState::Finalized, std::memory_order_release);
    platform::LogMessage(L"Recording session finalized (%llu video, %llu audio, %llu queue-full)",
                         static_cast<unsigned long long>(capture_worker_->accepted_frames()),
                         static_cast<unsigned long long>(written_audio_packets_.value()),
                         static_cast<unsigned long long>(
                             capture_worker_->queue_full_rejections()));
    return true;
}

void RecordingSession::Abort() noexcept {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    const RecordingSessionState initial_state = state();
    if (initial_state == RecordingSessionState::Finalized ||
        initial_state == RecordingSessionState::Aborted) {
        return;
    }

    StopWorkersAndJoin();
    if (writer_ != nullptr) {
        writer_->Abort();
        writer_.reset();
    }
    all_worker_threads_stopped_.store(true, std::memory_order_release);
    if (initial_state != RecordingSessionState::Failed) {
        state_.store(RecordingSessionState::Aborted, std::memory_order_release);
    }
}

RecordingSessionDiagnostics RecordingSession::diagnostics() const noexcept {
    RecordingSessionDiagnostics result;
    result.capture_attempts = capture_worker_->capture_attempts();
    result.acquired_frames = capture_worker_->acquired_frames();
    result.no_new_frames = capture_worker_->no_new_frames();
    result.reused_frames = capture_worker_->reused_frames();
    result.failure_reused_frames = capture_worker_->failure_reused_frames();
    result.accepted_frames = capture_worker_->accepted_frames();
    result.queue_full_rejections = capture_worker_->queue_full_rejections();
    result.pacing_slots_missed = capture_worker_->pacing_slots_missed();
    result.capture_failures = capture_worker_->capture_failures();
    result.capture_failure_diagnostics =
        capture_worker_->capture_failure_diagnostics();
    result.written_frames = written_frames_.value();
    result.written_audio_packets = written_audio_packets_.value();
    result.written_audio_frames = written_audio_frames_.value();
    result.silence_writes = silence_writes_.value();
    if (audio_provider_ != nullptr) {
        result.audio_capture = audio_provider_->diagnostics();
    }
    const media::MediaTime active_duration = capture_worker_->active_duration();
    result.active_duration_100ns = active_duration.count() > 0
                                       ? static_cast<uint64_t>(active_duration.count())
                                       : 0;
    result.capture_attempt_timing = capture_worker_->capture_attempt_timing();
    result.acquisition_timing = capture_worker_->acquisition_timing();
    result.region_copy_timing = capture_worker_->region_copy_timing();
    result.queue_submission_timing = capture_worker_->queue_submission_timing();
    result.writer_submission_timing = writer_submission_timing_.snapshot();
    result.audio_writer_submission_timing = audio_writer_submission_timing_.snapshot();
    result.silence_write_submission_timing =
        silence_write_submission_timing_.snapshot();
    result.audio_backpressure_wait_timing =
        audio_backpressure_wait_timing_.snapshot();
    result.queue_capacity = frame_queue_.capacity();
    result.peak_queue_depth = frame_queue_.peak_size();
    result.bounded_frame_payload_bytes = BoundedFramePayloadBytes();
    result.maximum_audio_writer_lead_100ns =
        kMaximumAudioWriterLead.count();
    result.audio_stream_interleave_lag_100ns =
        kAudioStreamInterleaveLag.count();
    {
        const std::scoped_lock trace_lock(audio_diagnostics_mutex_);
        result.first_audio_writer_input_pts_100ns =
            first_audio_writer_input_pts_100ns_;
        result.first_audio_writer_input_duration_100ns =
            first_audio_writer_input_duration_100ns_;
        result.maximum_audio_gate_wait_microseconds =
            maximum_audio_gate_wait_microseconds_;
        result.maximum_audio_gate_wait_packet_pts_100ns =
            maximum_audio_gate_wait_packet_pts_100ns_;
        result.maximum_audio_gate_wait_video_end_100ns =
            maximum_audio_gate_wait_video_end_100ns_;
        result.audio_segments = audio_segment_diagnostics_;
        result.audio_segment_diagnostic_count =
            audio_segment_diagnostic_count_;
        result.audio_packet_timeline = audio_packet_timeline_diagnostics_;
        result.audio_packet_timeline_count =
            audio_packet_timeline_diagnostic_count_;
        result.audio_packet_timeline_omitted =
            audio_packet_timeline_omitted_;
        result.silence_write_timeline = silence_write_diagnostics_;
        result.silence_write_timeline_count =
            silence_write_diagnostic_count_;
        result.silence_write_timeline_omitted =
            silence_write_timeline_omitted_;
    }
    result.all_worker_threads_stopped =
        all_worker_threads_stopped_.load(std::memory_order_acquire);
    return result;
}

bool RecordingSession::has_runtime_failure() const noexcept {
    if (state() == RecordingSessionState::Failed || capture_worker_->failed() ||
        (writer_ != nullptr && writer_->has_error())) {
        return true;
    }
    if (audio_provider_ == nullptr) {
        return false;
    }
    const media::AudioCaptureDiagnostics audio = audio_provider_->diagnostics();
    return audio.capture_failures != 0 || audio.device_invalidations != 0;
}

std::optional<media::TemporarySession> RecordingSession::temporary_session() const {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    return temporary_session_;
}

std::wstring RecordingSession::finalized_path() const {
    const std::scoped_lock result_lock(result_mutex_);
    return finalized_path_;
}

std::wstring RecordingSession::last_error() const {
    const std::scoped_lock result_lock(result_mutex_);
    return last_error_;
}

void RecordingSession::ConsumerMain() noexcept {
    try {
        std::optional<media::VideoFrame> pending_frame;
        bool write_failed = false;

        const auto write_pending = [this, &pending_frame]() {
            if (!WriteSilenceUpTo(pending_frame->timestamp)) {
                return false;
            }
            const auto write_start = std::chrono::steady_clock::now();
            const bool written = writer_ != nullptr && writer_->WriteVideoFrame(*pending_frame);
            const auto elapsed = std::chrono::steady_clock::now() - write_start;
            const auto microseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            writer_submission_timing_.Add(
                microseconds > 0 ? static_cast<uint64_t>(microseconds) : 0);
            if (written) {
                written_frames_.Increment();
                const media::MediaTime frame_end =
                    pending_frame->timestamp + pending_frame->duration;
                {
                    // Publish the predicate under the same mutex used by the
                    // audio gate so a final video frame cannot race between
                    // predicate evaluation and condition-variable sleep.
                    const std::scoped_lock audio_lock(audio_callback_mutex_);
                    last_video_writer_end_100ns_.store(
                        frame_end.count(), std::memory_order_release);
                }
                audio_video_progress_.notify_all();
            }
            return written;
        };

        for (;;) {
            std::optional<media::VideoFrame> frame = frame_queue_.wait_pop();
            if (!frame.has_value()) {
                break;
            }
            if (pending_frame.has_value()) {
                pending_frame->duration = frame->timestamp - pending_frame->timestamp;
                if (pending_frame->duration <= media::MediaTime::zero() || !write_pending()) {
                    write_failed = true;
                    break;
                }
            }
            pending_frame = std::move(frame);
        }

        if (!write_failed && pending_frame.has_value()) {
            const media::MediaTime endpoint = capture_worker_->active_duration();
            pending_frame->duration = endpoint - pending_frame->timestamp;
            if (pending_frame->duration <= media::MediaTime::zero() || !write_pending()) {
                write_failed = true;
            }
        }

        if (write_failed) {
                std::wstring message =
                    writer_ != nullptr ? writer_->last_error() : std::wstring{};
                if (message.empty()) {
                    message = L"Writing a captured video frame failed";
                }
                RecordFailure(std::move(message));
                audio_accepting_.store(false, std::memory_order_release);
                frame_queue_.close();
                capture_worker_->Stop();
                if (writer_ != nullptr) {
                    writer_->Abort();
                }
        }
        if (capture_worker_->failed()) {
            RecordFailure(L"The desktop capture worker failed unexpectedly");
            if (writer_ != nullptr) {
                writer_->Abort();
            }
        }
    } catch (...) {
        RecordFailure(L"The bounded video consumer failed unexpectedly");
        audio_accepting_.store(false, std::memory_order_release);
        frame_queue_.close();
        capture_worker_->Stop();
        if (writer_ != nullptr) {
            writer_->Abort();
        }
    }
    {
        const std::scoped_lock audio_lock(audio_callback_mutex_);
        consumer_running_.store(false, std::memory_order_release);
    }
    audio_video_progress_.notify_all();
}

void RecordingSession::ResetAudioTimelineDiagnostics() noexcept {
    const std::scoped_lock trace_lock(audio_diagnostics_mutex_);
    audio_segment_diagnostics_ = {};
    audio_segment_diagnostic_count_ = 0;
    audio_packet_timeline_diagnostics_ = {};
    audio_packet_timeline_diagnostic_count_ = 0;
    audio_packet_timeline_omitted_ = 0;
    audio_packet_sequence_ = 0;
    silence_write_diagnostics_ = {};
    silence_write_diagnostic_count_ = 0;
    silence_write_timeline_omitted_ = 0;
    silence_write_sequence_ = 0;
    first_audio_writer_input_pts_100ns_ = -1;
    first_audio_writer_input_duration_100ns_ = 0;
    maximum_audio_gate_wait_microseconds_ = 0;
    maximum_audio_gate_wait_packet_pts_100ns_ = -1;
    maximum_audio_gate_wait_video_end_100ns_ = 0;
}

void RecordingSession::TraceAudioSegment(
    media::MediaTime active_offset, media::MediaTime qpc_anchor) noexcept {
    uint64_t segment_index = 0;
    {
        const std::scoped_lock trace_lock(audio_diagnostics_mutex_);
        segment_index = static_cast<uint64_t>(audio_segment_diagnostic_count_);
        if (audio_segment_diagnostic_count_ <
            audio_segment_diagnostics_.size()) {
            AudioSegmentTimelineDiagnostic& entry =
                audio_segment_diagnostics_[audio_segment_diagnostic_count_++];
            entry.segment_index = segment_index;
            entry.active_offset_100ns = active_offset.count();
            entry.qpc_anchor_100ns = qpc_anchor.count();
        }
    }
    platform::LogMessage(
        L"[RecordingAudioTimeline] segment=%llu active_zero=0 "
        L"active_offset_100ns=%lld qpc_anchor_100ns=%lld",
        static_cast<unsigned long long>(segment_index),
        static_cast<long long>(active_offset.count()),
        static_cast<long long>(qpc_anchor.count()));
}

std::optional<std::size_t> RecordingSession::TraceMappedAudioPacket(
    const media::AudioPacket& packet,
    const MappedAudioPacket& mapped) noexcept {
    const std::scoped_lock trace_lock(audio_diagnostics_mutex_);
    const uint64_t sequence = audio_packet_sequence_;
    if (audio_packet_sequence_ != (std::numeric_limits<uint64_t>::max)()) {
        ++audio_packet_sequence_;
    }
    if (audio_packet_timeline_diagnostic_count_ >=
        audio_packet_timeline_diagnostics_.size()) {
        if (audio_packet_timeline_omitted_ !=
            (std::numeric_limits<uint64_t>::max)()) {
            ++audio_packet_timeline_omitted_;
        }
        return std::nullopt;
    }

    const std::size_t index = audio_packet_timeline_diagnostic_count_++;
    AudioPacketTimelineDiagnostic& entry =
        audio_packet_timeline_diagnostics_[index];
    entry.packet_sequence = sequence;
    entry.source_timestamp_100ns = packet.timestamp.count();
    entry.device_position = packet.device_position;
    entry.source_frame_count = packet.frame_count;
    entry.source_flags = static_cast<uint32_t>(packet.flags);
    entry.timestamp_source = packet.timestamp_source;
    entry.mapped_pts_100ns = mapped.timestamp.count();
    entry.mapped_duration_100ns = mapped.duration.count();
    entry.first_frame = mapped.first_frame;
    entry.mapped_frame_count = mapped.frame_count;
    entry.decision = mapped.decision;
    entry.qpc_error_100ns = mapped.qpc_error_100ns;
    entry.committed_audio_end_100ns = committed_audio_end_.count();
    entry.committed_tick_100ns = silence_write_position_.count();
    entry.video_end_when_mapped_100ns =
        last_video_writer_end_100ns_.load(std::memory_order_acquire);
    entry.qpc_precise = mapped.qpc_precise;
    return index;
}

void RecordingSession::CompleteAudioPacketTrace(
    std::optional<std::size_t> trace_index, media::MediaTime chunk_start,
    media::MediaTime chunk_end, uint64_t gate_wait_microseconds,
    bool writer_accepted) noexcept {
    AudioPacketTimelineDiagnostic completed{};
    bool have_completed_entry = false;
    bool recorded_first_writer_input = false;
    {
        const std::scoped_lock trace_lock(audio_diagnostics_mutex_);
        const int64_t video_end =
            last_video_writer_end_100ns_.load(std::memory_order_acquire);
        if (writer_accepted && first_audio_writer_input_pts_100ns_ < 0 &&
            trace_index.has_value()) {
            const AudioPacketTimelineDiagnostic& first =
                audio_packet_timeline_diagnostics_[*trace_index];
            first_audio_writer_input_pts_100ns_ = first.mapped_pts_100ns;
            first_audio_writer_input_duration_100ns_ =
                first.mapped_duration_100ns;
            recorded_first_writer_input = true;
        }
        if (gate_wait_microseconds > maximum_audio_gate_wait_microseconds_) {
            maximum_audio_gate_wait_microseconds_ = gate_wait_microseconds;
            maximum_audio_gate_wait_video_end_100ns_ = video_end;
            maximum_audio_gate_wait_packet_pts_100ns_ =
                trace_index.has_value()
                    ? audio_packet_timeline_diagnostics_[*trace_index]
                          .mapped_pts_100ns
                    : chunk_start.count();
        }
        if (trace_index.has_value() &&
            *trace_index < audio_packet_timeline_diagnostic_count_) {
            AudioPacketTimelineDiagnostic& entry =
                audio_packet_timeline_diagnostics_[*trace_index];
            entry.video_end_when_written_100ns = video_end;
            entry.audio_lead_when_written_100ns = chunk_end.count() - video_end;
            entry.gate_wait_microseconds = gate_wait_microseconds;
            entry.writer_accepted = writer_accepted;
            completed = entry;
            have_completed_entry = true;
        }
    }
    if (recorded_first_writer_input) {
        platform::LogMessage(
            L"[RecordingAudioTimeline] first_audio_writer_input_pts_100ns=%lld "
            L"duration_100ns=%lld",
            static_cast<long long>(completed.mapped_pts_100ns),
            static_cast<long long>(completed.mapped_duration_100ns));
    }
    if (have_completed_entry) {
        platform::LogMessage(
            L"[RecordingAudioTimeline] packet=%llu source_pts_100ns=%lld "
            L"device_pos=%llu frames=%u flags=0x%X timestamp_source=%u "
            L"mapped_pts_100ns=%lld duration_100ns=%lld first_frame=%u "
            L"decision=%u qpc_error_100ns=%lld",
            static_cast<unsigned long long>(completed.packet_sequence),
            static_cast<long long>(completed.source_timestamp_100ns),
            static_cast<unsigned long long>(completed.device_position),
            completed.source_frame_count, completed.source_flags,
            static_cast<unsigned int>(completed.timestamp_source),
            static_cast<long long>(completed.mapped_pts_100ns),
            static_cast<long long>(completed.mapped_duration_100ns),
            completed.first_frame,
            static_cast<unsigned int>(completed.decision),
            static_cast<long long>(completed.qpc_error_100ns));
        platform::LogMessage(
            L"[RecordingAudioGate] packet=%llu tick_100ns=%lld "
            L"video_end_100ns=%lld lead_100ns=%lld gate_wait_us=%llu "
            L"writer_accepted=%u",
            static_cast<unsigned long long>(completed.packet_sequence),
            static_cast<long long>(completed.committed_tick_100ns),
            static_cast<long long>(completed.video_end_when_written_100ns),
            static_cast<long long>(completed.audio_lead_when_written_100ns),
            static_cast<unsigned long long>(completed.gate_wait_microseconds),
            completed.writer_accepted ? 1u : 0u);
    }
}

void RecordingSession::TraceSilenceWrite(
    media::MediaTime tick, media::MediaTime video_position,
    media::MediaTime committed_audio_end,
    std::optional<media::MediaTime> pending_audio_start,
    uint64_t submission_microseconds, bool writer_accepted) noexcept {
    uint64_t sequence = 0;
    {
        const std::scoped_lock trace_lock(audio_diagnostics_mutex_);
        sequence = silence_write_sequence_;
        if (silence_write_sequence_ !=
            (std::numeric_limits<uint64_t>::max)()) {
            ++silence_write_sequence_;
        }
        if (silence_write_diagnostic_count_ <
            silence_write_diagnostics_.size()) {
            SilenceWriteDiagnostic& entry =
                silence_write_diagnostics_[
                    silence_write_diagnostic_count_++];
            entry.tick_sequence = sequence;
            entry.tick_pts_100ns = tick.count();
            entry.triggering_video_position_100ns = video_position.count();
            entry.committed_audio_end_100ns = committed_audio_end.count();
            entry.pending_audio_start_100ns =
                pending_audio_start.has_value() ? pending_audio_start->count() :
                                                  -1;
            entry.submission_microseconds = submission_microseconds;
            entry.writer_accepted = writer_accepted;
        } else if (silence_write_timeline_omitted_ !=
                   (std::numeric_limits<uint64_t>::max)()) {
            ++silence_write_timeline_omitted_;
        }
    }
    if (sequence < kSilenceWriteDiagnosticCapacity) {
        platform::LogMessage(
            L"[RecordingAudioTimeline] tick=%llu pts_100ns=%lld "
            L"video_position_100ns=%lld committed_audio_end_100ns=%lld "
            L"pending_audio_start_100ns=%lld submission_us=%llu accepted=%u",
            static_cast<unsigned long long>(sequence),
            static_cast<long long>(tick.count()),
            static_cast<long long>(video_position.count()),
            static_cast<long long>(committed_audio_end.count()),
            static_cast<long long>(pending_audio_start.has_value()
                                       ? pending_audio_start->count()
                                       : -1),
            static_cast<unsigned long long>(submission_microseconds),
            writer_accepted ? 1u : 0u);
    }
}

bool RecordingSession::BeginAudioSegment() noexcept {
    if (!AudioEnabled() || audio_provider_ == nullptr) {
        return true;
    }
    const std::scoped_lock audio_lock(audio_callback_mutex_);
    const std::optional<media::MediaTime> qpc_anchor =
        audio_provider_->clock_position();
    const media::MediaTime active_offset = capture_worker_->active_duration();
    const bool began = qpc_anchor.has_value() &&
                       audio_timeline_.BeginSegment(active_offset,
                                                    *qpc_anchor);
    if (began) {
        TraceAudioSegment(active_offset, *qpc_anchor);
    }
    return began;
}

bool RecordingSession::WriteSilenceUpTo(
    media::MediaTime video_position) noexcept {
    if (!AudioEnabled()) {
        return true;
    }
    try {
        std::unique_lock audio_lock(audio_callback_mutex_);
        const std::optional<media::MediaTime> target = NextAudioStreamTick(
            video_position, kAudioStreamInterleaveLag, committed_audio_end_,
            silence_write_position_, pending_audio_start_);
        if (!target.has_value()) {
            return true;
        }
        if (writer_ == nullptr || !audio_format_.valid()) {
            return false;
        }

        // Allocate the reusable silence buffer on first use.
        constexpr uint32_t kFramesPerSilenceChunk = 1024;
        if (silence_buffer_.empty()) {
            const std::size_t byte_count =
                static_cast<std::size_t>(kFramesPerSilenceChunk) *
                audio_format_.block_align();
            silence_buffer_.assign(byte_count,
                                   audio_format_.bits_per_sample == 8 &&
                                           !audio_format_.is_float
                                       ? static_cast<uint8_t>(0x80)
                                       : static_cast<uint8_t>(0x00));
        }

        const media::MediaTime committed_before = committed_audio_end_;
        const std::optional<media::MediaTime> pending_before =
            pending_audio_start_;
        const auto write_start = std::chrono::steady_clock::now();

        const bool any_written = FillSilenceGap(*target);
        if (!any_written && committed_audio_end_ < *target) {
            // FillSilenceGap returned false → writer failure.
            return false;
        }

        const auto elapsed = std::chrono::steady_clock::now() - write_start;
        const auto microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        silence_write_submission_timing_.Add(
            microseconds > 0 ? static_cast<uint64_t>(microseconds) : 0);
        const uint64_t elapsed_microseconds =
            microseconds > 0 ? static_cast<uint64_t>(microseconds) : 0;
        TraceSilenceWrite(*target, video_position, committed_before,
                             pending_before, elapsed_microseconds, any_written);
        if (any_written) {
            silence_write_position_ = committed_audio_end_;
            silence_writes_.Increment();
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool RecordingSession::FillSilenceGap(media::MediaTime target) noexcept {
    // Preconditions: audio_callback_mutex_ is held, silence_buffer_ is
    // initialized, writer_ is valid.
    constexpr uint32_t kFramesPerSilenceChunk = 1024;
    if (committed_audio_end_ >= target) {
        return true;  // Nothing to fill.
    }
    bool any_written = false;
    while (committed_audio_end_ < target) {
        const int64_t remaining_100ns =
            target.count() - committed_audio_end_.count();
        const uint64_t remaining_frames = static_cast<uint64_t>(
            (static_cast<uint64_t>(remaining_100ns) *
             audio_format_.sample_rate) /
            10'000'000ULL);
        if (remaining_frames == 0) {
            // The remaining interval is smaller than one representable PCM
            // frame. Treat it as reconciled; the next real packet keeps its
            // QPC-derived timestamp and the writer remains monotonic.
            return true;
        }
        const uint32_t chunk_frames = static_cast<uint32_t>(
            (std::min)(static_cast<uint64_t>(kFramesPerSilenceChunk),
                       remaining_frames));
        const media::MediaTime chunk_duration{static_cast<int64_t>(
            (static_cast<uint64_t>(chunk_frames) * 10'000'000ULL) /
            audio_format_.sample_rate)};
        if (chunk_duration <= media::MediaTime::zero()) {
            return true;
        }

        media::MediaChunk chunk;
        chunk.data = silence_buffer_.data();
        chunk.size = static_cast<std::size_t>(chunk_frames) *
                     audio_format_.block_align();
        chunk.timestamp = committed_audio_end_;
        chunk.duration = chunk_duration;
        chunk.discontinuity =
            !any_written && committed_audio_end_ == media::MediaTime::zero();

        if (!writer_->WriteAudioPacket(chunk)) {
            return false;
        }
        committed_audio_end_ = chunk.timestamp + chunk.duration;
        any_written = true;
    }
    return any_written || committed_audio_end_ >= target;
}

void RecordingSession::OnAudioPacket(const media::AudioPacket& packet) noexcept {
    if (!audio_accepting_.load(std::memory_order_acquire)) {
        return;
    }

    bool write_failed = false;
    std::wstring write_failure_context;
    try {
        std::unique_lock audio_lock(audio_callback_mutex_);
        if (!audio_accepting_.load(std::memory_order_acquire)) {
            return;
        }
        if (!packet.consistent() || !SameAudioFormat(packet.format, audio_format_) ||
            packet.timestamp < media::MediaTime::zero()) {
            RecordFailure(L"Validating a captured WASAPI audio packet failed");
            audio_accepting_.store(false, std::memory_order_release);
            return;
        }

        MappedAudioPacket mapped;
        const AudioPacketMapResult map_result =
            audio_timeline_.Map(packet, mapped);
        if (map_result == AudioPacketMapResult::Dropped) {
            return;
        }
        if (map_result != AudioPacketMapResult::Mapped || mapped.frame_count == 0) {
            RecordFailure(L"Mapping captured PCM onto the active-time audio timeline failed");
            audio_accepting_.store(false, std::memory_order_release);
            return;
        }
        // The video-driven silence frontier can legitimately advance a few
        // frames beyond a newly re-anchored mixed packet after pause/resume.
        // Reconcile that overlap at whole PCM-frame boundaries instead of
        // submitting a non-monotonic sample or discarding the usable suffix.
        if (mapped.timestamp < committed_audio_end_) {
            const uint64_t overlap_frames = TimeToAudioFramesCeiling(
                committed_audio_end_ - mapped.timestamp,
                audio_format_.sample_rate);
            if (overlap_frames >= mapped.frame_count) {
                return;
            }
            mapped.first_frame += static_cast<uint32_t>(overlap_frames);
            mapped.frame_count -= static_cast<uint32_t>(overlap_frames);
            mapped.timestamp = committed_audio_end_;
            mapped.duration = AudioFramesToTime(mapped.frame_count,
                                                 audio_format_.sample_rate);
        }
        if (mapped.timestamp < silence_write_position_) {
            RecordFailure(L"Captured audio arrived behind an already committed audio gap");
            audio_accepting_.store(false, std::memory_order_release);
            return;
        }
        pending_audio_start_ = mapped.timestamp;
        struct PendingAudioGuard final {
            std::optional<media::MediaTime>& pending;
            std::condition_variable& drained;
            ~PendingAudioGuard() {
                pending.reset();
                drained.notify_all();
            }
        };
        [[maybe_unused]] PendingAudioGuard pending_audio_guard{
            pending_audio_start_, audio_callback_drained_};
        const std::optional<std::size_t> trace_index =
            TraceMappedAudioPacket(packet, mapped);

        const std::size_t block_align = packet.format.block_align();
        const std::size_t first_byte =
            static_cast<std::size_t>(mapped.first_frame) * block_align;
        media::MediaChunk chunk;
        chunk.data = packet.data.data() + first_byte;
        chunk.size = static_cast<std::size_t>(mapped.frame_count) * block_align;
        chunk.timestamp = mapped.timestamp;
        chunk.duration = mapped.duration;
        chunk.discontinuity = mapped.discontinuity;

        const media::MediaTime chunk_end = chunk.timestamp + chunk.duration;
        const auto wait_start = std::chrono::steady_clock::now();
        audio_video_progress_.wait(audio_lock, [this, chunk_end] {
            const int64_t video_end =
                last_video_writer_end_100ns_.load(std::memory_order_acquire);
            const int64_t maximum = (std::numeric_limits<int64_t>::max)();
            const int64_t allowed_audio_end =
                video_end > maximum - kMaximumAudioWriterLead.count()
                    ? maximum
                    : video_end + kMaximumAudioWriterLead.count();
            return chunk_end.count() <= allowed_audio_end ||
                   !consumer_running_.load(std::memory_order_acquire) ||
                   state() == RecordingSessionState::Failed;
        });
        const auto wait_elapsed =
            std::chrono::steady_clock::now() - wait_start;
        const auto wait_microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(wait_elapsed)
                .count();
        audio_backpressure_wait_timing_.Add(
            wait_microseconds > 0 ? static_cast<uint64_t>(wait_microseconds) :
                                    0);
        const uint64_t gate_wait_microseconds =
            wait_microseconds > 0 ? static_cast<uint64_t>(wait_microseconds) :
                                    0;
        if (!consumer_running_.load(std::memory_order_acquire) ||
            state() == RecordingSessionState::Failed) {
            write_failed = true;
            write_failure_context =
                L"Audio delivery outlived the recording video consumer";
        }

        const auto write_start = std::chrono::steady_clock::now();
        // Structural invariant: before any real audio at PTS X is submitted
        // to the writer, the encoded audio timeline must be continuous up to X.
        // WriteSilenceUpTo on the consumer thread advances silence only to
        // (video_pos - interleave_lag). Any remaining gap between that frontier
        // and the real audio's mapped timestamp must be filled here.
        bool written = false;
        if (!write_failed && writer_ != nullptr &&
            committed_audio_end_ < chunk.timestamp) {
            // Ensure the silence buffer is initialized.
            constexpr uint32_t kFramesPerSilenceChunk = 1024;
            if (silence_buffer_.empty()) {
                const std::size_t byte_count =
                    static_cast<std::size_t>(kFramesPerSilenceChunk) *
                    audio_format_.block_align();
                silence_buffer_.assign(
                    byte_count,
                    audio_format_.bits_per_sample == 8 && !audio_format_.is_float
                        ? static_cast<uint8_t>(0x80)
                        : static_cast<uint8_t>(0x00));
            }
            if (!FillSilenceGap(chunk.timestamp)) {
                write_failed = true;
                write_failure_context =
                    L"Filling the audio gap before captured PCM failed";
            }
        }
        if (!write_failed && writer_ != nullptr) {
            written = writer_->WriteAudioPacket(chunk);
        } else if (!write_failed) {
            write_failure_context =
                L"The audio writer was unavailable during packet delivery";
        }
        if (written) {
            const auto elapsed = std::chrono::steady_clock::now() - write_start;
            const auto microseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            audio_writer_submission_timing_.Add(
                microseconds > 0 ? static_cast<uint64_t>(microseconds) : 0);
            committed_audio_end_ = chunk_end;
            written_audio_packets_.Increment();
            written_audio_frames_.Add(mapped.frame_count);
        } else {
            write_failed = true;
            if (write_failure_context.empty()) {
                write_failure_context =
                    L"The AAC writer rejected a captured PCM packet";
            }
        }
        CompleteAudioPacketTrace(trace_index, chunk.timestamp, chunk_end,
                                 gate_wait_microseconds, written);
    } catch (...) {
        write_failed = true;
        write_failure_context =
            L"An exception interrupted captured PCM delivery";
    }

    if (write_failed) {
        std::wstring message = writer_ != nullptr ? writer_->last_error() : std::wstring{};
        if (message.empty()) {
            message = write_failure_context.empty()
                          ? L"Writing a captured audio packet failed"
                          : std::move(write_failure_context);
        }
        RecordFailure(std::move(message));
        audio_accepting_.store(false, std::memory_order_release);
        frame_queue_.close();
        capture_worker_->Stop();
    }
}

bool RecordingSession::FinalizeAudioTimeline(media::MediaTime endpoint) noexcept {
    if (!AudioEnabled()) {
        return true;
    }
    try {
        const std::scoped_lock audio_lock(audio_callback_mutex_);
        if (writer_ == nullptr || !audio_format_.valid() || endpoint <= media::MediaTime::zero()) {
            return false;
        }
        if (committed_audio_end_ >= endpoint && written_audio_packets_.value() != 0) {
            return true;
        }
        if (endpoint <= committed_audio_end_) {
            return written_audio_packets_.value() != 0;
        }

        // Ensure the reusable silence buffer is initialized.
        constexpr uint32_t kFramesPerSilenceChunk = 1024;
        if (silence_buffer_.empty()) {
            const std::size_t byte_count =
                static_cast<std::size_t>(kFramesPerSilenceChunk) *
                audio_format_.block_align();
            silence_buffer_.assign(byte_count,
                                   audio_format_.bits_per_sample == 8 &&
                                           !audio_format_.is_float
                                       ? static_cast<uint8_t>(0x80)
                                       : static_cast<uint8_t>(0x00));
        }

        const media::MediaTime fill_start = committed_audio_end_;

        // Same structural invariant as during recording: fill the remaining
        // gap from committed_audio_end_ to endpoint with bounded 1024-frame
        // PCM silence chunks.  With the continuous silence model,
        // committed_audio_end_ is typically only ~750 ms behind endpoint
        // (the interleave lag), so this writes approximately 33 bounded
        // chunks.  Memory use is O(1) — the reusable silence_buffer_ is
        // shared with WriteSilenceUpTo and OnAudioPacket.
        if (!FillSilenceGap(endpoint)) {
            std::wstring message = writer_->last_error();
            if (message.empty()) {
                message =
                    L"Writing trailing audio silence during finalization failed";
            }
            RecordFailure(std::move(message));
            return false;
        }

        {
            const std::scoped_lock trace_lock(audio_diagnostics_mutex_);
            if (first_audio_writer_input_pts_100ns_ < 0 &&
                committed_audio_end_ > fill_start) {
                first_audio_writer_input_pts_100ns_ = fill_start.count();
                first_audio_writer_input_duration_100ns_ =
                    (committed_audio_end_ - fill_start).count();
                platform::LogMessage(
                    L"[RecordingAudioTimeline] "
                    L"first_audio_writer_input_pts_100ns=%lld "
                    L"duration_100ns=%lld source=finalization_fill",
                    static_cast<long long>(fill_start.count()),
                    static_cast<long long>(
                        (committed_audio_end_ - fill_start).count()));
            }
        }
        return true;
    } catch (...) {
        RecordFailure(L"Completing the bounded active-time audio timeline failed");
        return false;
    }
}

void RecordingSession::SetAudioAccepting(bool accepting) noexcept {
    if (!AudioEnabled()) {
        return;
    }
    audio_accepting_.store(accepting, std::memory_order_release);
    if (!accepting) {
        // A callback may have mapped PCM and then released this mutex while it
        // waits for video to enter the 100-ms writer-lead window. Keep video
        // running and drain that already-mapped packet before Pause/Stop can
        // freeze the video frontier or begin another active-time segment.
        std::unique_lock audio_lock(audio_callback_mutex_);
        audio_callback_drained_.wait(audio_lock, [this] {
            return !pending_audio_start_.has_value();
        });
    }
}

bool RecordingSession::AudioEnabled() const noexcept {
    return options_.audio == media::AudioSource::SystemAudio ||
           options_.audio == media::AudioSource::Microphone ||
           options_.audio == media::AudioSource::Both;
}

void RecordingSession::RecordFailure(std::wstring message) noexcept {
    try {
        const std::scoped_lock result_lock(result_mutex_);
        if (last_error_.empty()) {
            last_error_ = BoundedDiagnostic(std::move(message));
        }
    } catch (...) {
        // Failure reporting must never prevent deterministic worker cleanup.
    }
    state_.store(RecordingSessionState::Failed, std::memory_order_release);
}

void RecordingSession::RollBackStartup(std::wstring message) noexcept {
    RecordFailure(std::move(message));
    StopWorkersAndJoin();
    if (writer_ != nullptr) {
        writer_->Abort();
        writer_.reset();
    }
    all_worker_threads_stopped_.store(true, std::memory_order_release);
}

void RecordingSession::StopWorkersAndJoin() noexcept {
    // Normal Stop drains provider-accepted PCM while the video frontier and
    // session callback remain live. Failed/abort paths also pass through this
    // idempotent boundary, where callbacks already gated off return promptly.
    if (AudioEnabled() && audio_provider_ != nullptr) {
        audio_provider_->PauseDeliveryAndDrain();
    }
    SetAudioAccepting(false);
    capture_worker_->Stop();
    frame_queue_.close();
    if (audio_provider_ != nullptr) {
        audio_provider_->Stop();
    }
    if (consumer_thread_.joinable()) {
        consumer_thread_.join();
    }
    consumer_running_.store(false, std::memory_order_release);
}

uint64_t RecordingSession::BoundedFramePayloadBytes() const noexcept {
    const int64_t width = static_cast<int64_t>(options_.region.right) - options_.region.left;
    const int64_t height = static_cast<int64_t>(options_.region.bottom) - options_.region.top;
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const uint64_t pixels = SaturatingMultiply(static_cast<uint64_t>(width),
                                               static_cast<uint64_t>(height));
    const uint64_t bgra_frames = static_cast<uint64_t>(frame_queue_.capacity()) + 2;
    const uint64_t bgra_bytes = SaturatingMultiply(SaturatingMultiply(pixels, 4), bgra_frames);
    const uint64_t nv12_bytes = SaturatingAdd(pixels, pixels / 2);
    return SaturatingAdd(bgra_bytes, nv12_bytes);
}

std::unique_ptr<RecordingSession> CreateVideoRecordingSession(
    media::ITemporarySessionStore& store, media::RecordingOptions options) {
    const bool supported_audio = options.audio == media::AudioSource::None ||
                                 options.audio == media::AudioSource::SystemAudio ||
                                 options.audio == media::AudioSource::Microphone ||
                                 options.audio == media::AudioSource::Both;
    const bool supported_format =
        options.format == media::RecordingFormat::Mp4 ||
        options.format == media::RecordingFormat::Gif;
    if (!supported_format || !supported_audio ||
        (options.format == media::RecordingFormat::Gif &&
         options.audio != media::AudioSource::None)) {
        return nullptr;
    }
    try {
        std::unique_ptr<capture::IRegionCaptureProvider> provider =
            capture::CreateRegionCaptureProvider();
        if (provider == nullptr) {
            return nullptr;
        }
        std::unique_ptr<media::IAudioCaptureProvider> audio_provider;
        if (options.audio != media::AudioSource::None) {
            audio_provider = media::CreateWasapiAudioCaptureProvider();
            if (audio_provider == nullptr) {
                return nullptr;
            }
        }
        return std::unique_ptr<RecordingSession>(
            new RecordingSession(store, options, std::move(provider),
                                 std::move(audio_provider)));
    } catch (...) {
        return nullptr;
    }
}

}  // namespace easnap::recording
