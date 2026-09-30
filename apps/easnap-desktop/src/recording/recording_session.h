// Easnap - bounded audio/video recording session orchestration.
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "capture/recording_capture_worker.h"
#include "media/frame_queue.h"
#include "media/media_interfaces.h"
#include "media/temp_session.h"
#include "recording/audio_timeline.h"

namespace easnap::recording {

enum class RecordingSessionState {
    Ready,
    Starting,
    Recording,
    Paused,
    Stopping,
    Finalized,
    Failed,
    Aborted,
};

inline constexpr std::size_t kAudioSegmentDiagnosticCapacity = 4;
inline constexpr std::size_t kAudioPacketTimelineDiagnosticCapacity = 16;
inline constexpr std::size_t kSilenceWriteDiagnosticCapacity = 8;

struct AudioSegmentTimelineDiagnostic {
    uint64_t segment_index{0};
    int64_t active_offset_100ns{0};
    int64_t qpc_anchor_100ns{0};
};

struct AudioPacketTimelineDiagnostic {
    uint64_t packet_sequence{0};
    int64_t source_timestamp_100ns{0};
    uint64_t device_position{0};
    uint32_t source_frame_count{0};
    uint32_t source_flags{0};
    media::AudioTimestampSource timestamp_source{
        media::AudioTimestampSource::WasapiQpc};
    int64_t mapped_pts_100ns{0};
    int64_t mapped_duration_100ns{0};
    uint32_t first_frame{0};
    uint32_t mapped_frame_count{0};
    AudioTimelineDecision decision{AudioTimelineDecision::Continuous};
    int64_t qpc_error_100ns{0};
    int64_t committed_audio_end_100ns{0};
    int64_t committed_tick_100ns{0};
    int64_t video_end_when_mapped_100ns{0};
    int64_t video_end_when_written_100ns{0};
    int64_t audio_lead_when_written_100ns{0};
    uint64_t gate_wait_microseconds{0};
    bool qpc_precise{false};
    bool writer_accepted{false};
};

struct SilenceWriteDiagnostic {
    uint64_t tick_sequence{0};
    int64_t tick_pts_100ns{0};
    int64_t triggering_video_position_100ns{0};
    int64_t committed_audio_end_100ns{0};
    int64_t pending_audio_start_100ns{-1};
    uint64_t submission_microseconds{0};
    bool writer_accepted{false};
};

struct RecordingSessionDiagnostics {
    uint64_t capture_attempts{0};
    uint64_t acquired_frames{0};
    uint64_t no_new_frames{0};
    uint64_t reused_frames{0};
    uint64_t accepted_frames{0};
    uint64_t queue_full_rejections{0};
    uint64_t pacing_slots_missed{0};
    uint64_t capture_failures{0};
    capture::CaptureFailureDiagnostics capture_failure_diagnostics;
    uint64_t written_frames{0};
    uint64_t written_audio_packets{0};
    uint64_t written_audio_frames{0};
    uint64_t silence_writes{0};
    uint64_t failure_reused_frames{0};
    uint64_t active_duration_100ns{0};
    media::AudioCaptureDiagnostics audio_capture;
    capture::AggregateTiming capture_attempt_timing;
    capture::AggregateTiming acquisition_timing;
    capture::AggregateTiming region_copy_timing;
    capture::AggregateTiming queue_submission_timing;
    capture::AggregateTiming writer_submission_timing;
    capture::AggregateTiming audio_writer_submission_timing;
    capture::AggregateTiming silence_write_submission_timing;
    capture::AggregateTiming audio_backpressure_wait_timing;
    std::size_t queue_capacity{0};
    std::size_t peak_queue_depth{0};
    uint64_t bounded_frame_payload_bytes{0};
    int64_t audio_session_active_zero_100ns{0};
    int64_t maximum_audio_writer_lead_100ns{0};
    int64_t audio_stream_interleave_lag_100ns{0};
    int64_t first_audio_writer_input_pts_100ns{-1};
    int64_t first_audio_writer_input_duration_100ns{0};
    uint64_t maximum_audio_gate_wait_microseconds{0};
    int64_t maximum_audio_gate_wait_packet_pts_100ns{-1};
    int64_t maximum_audio_gate_wait_video_end_100ns{0};
    std::array<AudioSegmentTimelineDiagnostic,
               kAudioSegmentDiagnosticCapacity>
        audio_segments{};
    std::size_t audio_segment_diagnostic_count{0};
    std::array<AudioPacketTimelineDiagnostic,
               kAudioPacketTimelineDiagnosticCapacity>
        audio_packet_timeline{};
    std::size_t audio_packet_timeline_count{0};
    uint64_t audio_packet_timeline_omitted{0};
    std::array<SilenceWriteDiagnostic,
               kSilenceWriteDiagnosticCapacity>
        silence_write_timeline{};
    std::size_t silence_write_timeline_count{0};
    uint64_t silence_write_timeline_omitted{0};
    bool all_worker_threads_stopped{true};
};

/// Owns one bounded A/V recording pipeline above the capture and media modules.
/// The temporary store must outlive this object. Successfully finalized and
/// failed session data remains owned by that store for a later product decision.
class RecordingSession final {
public:
    ~RecordingSession();

    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    /// Transactionally creates storage, opens the writer, and starts both workers.
    [[nodiscard]] bool Start();
    [[nodiscard]] bool Pause();
    [[nodiscard]] bool Resume();
    /// Stops capture, drains accepted frames, finalizes, and retains the MP4.
    [[nodiscard]] bool Stop();
    /// Stops all workers and preserves any session-owned partial data.
    void Abort() noexcept;

    [[nodiscard]] RecordingSessionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] RecordingSessionDiagnostics diagnostics() const noexcept;
    /// Pollable failure signal for the UI. Per-frame capture misses remain
    /// diagnostics; only terminal worker/writer/audio failures return true.
    [[nodiscard]] bool has_runtime_failure() const noexcept;
    [[nodiscard]] std::optional<media::TemporarySession> temporary_session() const;
    [[nodiscard]] std::wstring finalized_path() const;
    [[nodiscard]] std::wstring last_error() const;

private:
    friend std::unique_ptr<RecordingSession> CreateVideoRecordingSession(
        media::ITemporarySessionStore&, media::RecordingOptions);

    RecordingSession(media::ITemporarySessionStore& store,
                     media::RecordingOptions options,
                     std::unique_ptr<capture::IRegionCaptureProvider> provider,
                     std::unique_ptr<media::IAudioCaptureProvider> audio_provider);

    void ConsumerMain() noexcept;
    void OnAudioPacket(const media::AudioPacket& packet) noexcept;
    [[nodiscard]] bool BeginAudioSegment() noexcept;
    [[nodiscard]] bool WriteSilenceUpTo(
        media::MediaTime video_position) noexcept;
    /// Fills [committed_audio_end_, target) with bounded PCM silence chunks.
    /// Caller must hold audio_callback_mutex_ and have ensured silence_buffer_
    /// is initialized and writer_ is valid. Returns false on writer failure.
    [[nodiscard]] bool FillSilenceGap(media::MediaTime target) noexcept;
    [[nodiscard]] bool FinalizeAudioTimeline(media::MediaTime endpoint) noexcept;
    void ResetAudioTimelineDiagnostics() noexcept;
    void TraceAudioSegment(media::MediaTime active_offset,
                           media::MediaTime qpc_anchor) noexcept;
    [[nodiscard]] std::optional<std::size_t> TraceMappedAudioPacket(
        const media::AudioPacket& packet,
        const MappedAudioPacket& mapped) noexcept;
    void CompleteAudioPacketTrace(std::optional<std::size_t> trace_index,
                                  media::MediaTime chunk_start,
                                  media::MediaTime chunk_end,
                                  uint64_t gate_wait_microseconds,
                                  bool writer_accepted) noexcept;
    void TraceSilenceWrite(media::MediaTime tick,
                              media::MediaTime video_position,
                              media::MediaTime committed_audio_end,
                              std::optional<media::MediaTime> pending_audio_start,
                              uint64_t submission_microseconds,
                              bool writer_accepted) noexcept;
    void SetAudioAccepting(bool accepting) noexcept;
    [[nodiscard]] bool AudioEnabled() const noexcept;
    void RecordFailure(std::wstring message) noexcept;
    void RollBackStartup(std::wstring message) noexcept;
    void StopWorkersAndJoin() noexcept;
    [[nodiscard]] uint64_t BoundedFramePayloadBytes() const noexcept;

    media::ITemporarySessionStore& store_;
    media::RecordingOptions options_;
    media::FrameQueue<media::VideoFrame> frame_queue_;
    std::unique_ptr<capture::RecordingCaptureWorker> capture_worker_;
    std::unique_ptr<media::IAudioCaptureProvider> audio_provider_;
    std::unique_ptr<media::IMediaWriter> writer_;

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex result_mutex_;
    std::optional<media::TemporarySession> temporary_session_;
    std::wstring finalized_path_;
    std::wstring last_error_;

    std::thread consumer_thread_;
    capture::SaturatingCounter written_frames_;
    capture::SaturatingCounter written_audio_packets_;
    capture::SaturatingCounter written_audio_frames_;
    capture::SaturatingCounter silence_writes_;
    capture::TimingAccumulator writer_submission_timing_;
    capture::TimingAccumulator audio_writer_submission_timing_;
    capture::TimingAccumulator silence_write_submission_timing_;
    capture::TimingAccumulator audio_backpressure_wait_timing_;
    mutable std::mutex audio_diagnostics_mutex_;
    std::array<AudioSegmentTimelineDiagnostic,
               kAudioSegmentDiagnosticCapacity>
        audio_segment_diagnostics_{};
    std::size_t audio_segment_diagnostic_count_{0};
    std::array<AudioPacketTimelineDiagnostic,
               kAudioPacketTimelineDiagnosticCapacity>
        audio_packet_timeline_diagnostics_{};
    std::size_t audio_packet_timeline_diagnostic_count_{0};
    uint64_t audio_packet_timeline_omitted_{0};
    uint64_t audio_packet_sequence_{0};
    std::array<SilenceWriteDiagnostic,
               kSilenceWriteDiagnosticCapacity>
        silence_write_diagnostics_{};
    std::size_t silence_write_diagnostic_count_{0};
    uint64_t silence_write_timeline_omitted_{0};
    uint64_t silence_write_sequence_{0};
    int64_t first_audio_writer_input_pts_100ns_{-1};
    int64_t first_audio_writer_input_duration_100ns_{0};
    uint64_t maximum_audio_gate_wait_microseconds_{0};
    int64_t maximum_audio_gate_wait_packet_pts_100ns_{-1};
    int64_t maximum_audio_gate_wait_video_end_100ns_{0};
    std::mutex audio_callback_mutex_;
    std::condition_variable audio_video_progress_;
    std::condition_variable audio_callback_drained_;
    media::AudioFormat audio_format_;
    AudioTimelineMapper audio_timeline_;
    // Writer-committed audio frontier. Tracks how far both real audio and
    // silence have been written to the writer.
    media::MediaTime committed_audio_end_{0};
    std::optional<media::MediaTime> pending_audio_start_;
    media::MediaTime silence_write_position_{0};
    std::vector<uint8_t> silence_buffer_;
    std::atomic<int64_t> last_video_writer_end_100ns_{0};
    std::atomic<bool> audio_accepting_{false};
    std::atomic<RecordingSessionState> state_{RecordingSessionState::Ready};
    std::atomic<bool> consumer_running_{false};
    std::atomic<bool> all_worker_threads_stopped_{true};
};

/// Creates a session wired to the production desktop, optional WASAPI audio,
/// and Media Foundation writer paths. Start remains explicit and transactional.
std::unique_ptr<RecordingSession> CreateVideoRecordingSession(
    media::ITemporarySessionStore& store, media::RecordingOptions options);

}  // namespace easnap::recording
