// Easnap - media layer contracts (Phase 2/3 implementation boundaries).
//
// Nothing in this header allocates audio, encoder or GPU resources: the Idle
// state must stay passive. These interfaces exist now so the screenshot slice
// already compiles against the final boundaries and the recording work never has
// to reshape the application core.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "media/audio_format.h"
#include "media/media_types.h"
#include "platform/platform_utils.h"

namespace easnap::media {

/// Audio routing offered for MP4 recordings. GIF never exposes these options
/// because an animated GIF has no audio stream.
enum class AudioSource { None, SystemAudio, Microphone, Both };

enum class RecordingFormat { Mp4, Gif };

struct RecordingOptions {
    RECT region{};
    RecordingFormat format{RecordingFormat::Mp4};
    AudioSource audio{AudioSource::None};  ///< Microphone stays off unless requested.
    bool include_cursor{true};
    uint32_t target_frame_rate{30};
    uint32_t video_bitrate_bps{8'000'000};
    uint32_t audio_bitrate_bps{192'000};
    uint32_t gif_fps{15};  ///< Spec default; presets may lower/raise it.
    uint32_t gif_width{0};   ///< Zero preserves/derives the source dimension.
    uint32_t gif_height{0};
    uint16_t gif_palette_colors{256};
    bool gif_dither{true};
    bool gif_fixed_rgb332{false};  ///< Legacy explicit encoder policy.
    bool gif_low_size{false};  ///< Neutral fixed palette, restrained dither and temporal stability.
    bool gif_sparse_transparency{false};
    bool gif_distributed_cadence{false};
};

/// One immutable, reference-counted BGRA payload on the recording timeline.
/// Repeated cadence samples may share pixels without copying them; the bounded
/// frame queue still owns each sample's timestamp and duration.
struct VideoFrame {
    std::shared_ptr<const platform::BgraImage> image;
    MediaTime timestamp{0};
    MediaTime duration{0};
};

/// A single encoded or raw chunk handed to the writer. Buffers are always
/// bounded and recycled: no component may retain a full recording in RAM.
struct MediaChunk {
    const uint8_t* data{nullptr};
    size_t size{0};
    MediaTime timestamp{0};
    MediaTime duration{0};
    bool key_frame{false};
    bool discontinuity{false};
};

// ---------------------------------------------------------------------------
// Audio capture
// ---------------------------------------------------------------------------

/// Result of an audio capture Start attempt. Distinguishes device absence from
/// internal errors so the caller can present an appropriate message.
enum class AudioCaptureResult {
    Ok,                   ///< Capture started successfully.
    SourceUnavailable,    ///< The requested endpoint does not exist or is disabled.
    InitializationFailed, ///< Device exists but WASAPI initialization failed.
    AlreadyActive,        ///< Start called while already capturing.
    InvalidSource,        ///< AudioSource::None or unsupported source requested.
    SystemSourceUnavailable,
    MicrophoneUnavailable,
    SystemInitializationFailed,
    MicrophoneInitializationFailed,
};

/// Only the first few timestamp resolutions are retained. This is sufficient
/// to diagnose endpoint wake-up without unbounded per-packet logging.
inline constexpr std::size_t kAudioTimestampDiagnosticCapacity = 8;

/// Privacy-safe clock metadata for one of the first captured packets. No PCM
/// payload or endpoint identity is retained.
struct AudioTimestampResolutionDiagnostic {
    uint64_t packet_index{0};
    uint32_t frame_count{0};
    uint32_t buffered_frame_count{0};
    uint64_t device_position{0};
    uint64_t raw_wasapi_qpc_100ns{0};
    uint64_t audio_clock_position{0};
    uint64_t audio_clock_frequency{0};
    uint64_t audio_clock_qpc_100ns{0};
    uint64_t capture_qpc_100ns{0};
    MediaTime raw_wasapi_timestamp{0};
    MediaTime audio_clock_timestamp{0};
    MediaTime capture_fallback_timestamp{0};
    MediaTime resolved_timestamp{0};
    AudioTimestampSource source{AudioTimestampSource::CaptureQpcFallback};
    AudioPacketFlags packet_flags{AudioPacketFlags::None};
    bool raw_wasapi_plausible{false};
    bool audio_clock_correlation_valid{false};
};

/// Saturating diagnostic counters for audio capture operation.
struct AudioCaptureDiagnostics {
    int64_t qpc_epoch_100ns{0};
    int64_t last_failure_hresult{0};
    uint64_t packets_acquired{0};
    uint64_t audio_frames_acquired{0};
    uint64_t bytes_acquired{0};
    uint64_t silent_packets{0};
    uint64_t discontinuities{0};
    uint64_t device_invalidations{0};
    uint64_t buffer_overruns{0};
    uint64_t capture_failures{0};
    uint64_t system_capture_failures{0};
    uint64_t microphone_capture_failures{0};
    uint64_t system_resampler_failures{0};
    uint64_t microphone_resampler_failures{0};
    uint64_t endpoint_queue_overflows{0};
    uint64_t mixer_capacity_failures{0};
    uint64_t mixer_invalid_packets{0};
    uint64_t mixer_late_discards{0};
    uint64_t delivery_packets_accepted{0};
    uint64_t delivery_packets_completed{0};
    uint64_t delivery_paused_discards{0};
    std::size_t queue_capacity{0};
    std::size_t peak_queue_occupancy{0};
    std::size_t timestamp_resolution_count{0};
    std::array<AudioTimestampResolutionDiagnostic,
               kAudioTimestampDiagnosticCapacity> timestamp_resolutions{};
};

/// Microphone / system-audio capture (WASAPI).
///
/// Implementations must use fixed-maximum ring buffers and apply back-pressure
/// or explicit drop policies instead of growing queues.
class IAudioCaptureProvider {
public:
    using PacketCallback = std::function<void(const AudioPacket&)>;

    virtual ~IAudioCaptureProvider() = default;

    virtual AudioCaptureResult Start(AudioSource source, PacketCallback on_packet) = 0;
    /// Atomically disables new callback delivery and waits until every packet
    /// accepted before that boundary has completed its callback.
    virtual void PauseDeliveryAndDrain() noexcept = 0;
    /// Re-enables callback delivery after PauseDeliveryAndDrain. Capture keeps
    /// draining the endpoint while delivery is paused.
    virtual void ResumeDelivery() noexcept = 0;
    virtual void Stop() = 0;
    [[nodiscard]] virtual bool active() const = 0;
    /// True when the requested endpoint exists and the user granted access.
    [[nodiscard]] virtual bool IsSourceAvailable(AudioSource source) const = 0;
    /// The native format discovered during Start. Valid only while active().
    [[nodiscard]] virtual AudioFormat negotiated_format() const = 0;
    /// Current QPC position relative to the same epoch as AudioPacket::timestamp.
    /// Available after a successful Start and used to anchor active-time segments.
    [[nodiscard]] virtual std::optional<MediaTime> clock_position() const noexcept = 0;
    /// Snapshot of current diagnostic counters.
    [[nodiscard]] virtual AudioCaptureDiagnostics diagnostics() const = 0;
};

/// Incremental media persistence (Media Foundation sink writer in Phase 2).
///
/// The writer streams to a disk-backed session file; it never requires the full
/// media in memory and must keep partial output recoverable after failures.
class IMediaWriter {
public:
    virtual ~IMediaWriter() = default;

    virtual bool Open(const std::wstring& output_path, const RecordingOptions& options,
                      const AudioFormat* audio_format = nullptr) = 0;
    virtual bool WriteVideoFrame(const VideoFrame& frame) = 0;
    virtual bool WriteAudioPacket(const MediaChunk& packet) = 0;
    /// Flushes and closes the container. Returns the finished file path.
    virtual bool Finalize(std::wstring& out_path) = 0;
    /// Aborts the current file but keeps session data for user inspection.
    virtual void Abort() = 0;
    [[nodiscard]] virtual bool has_error() const = 0;
    /// Actionable context for the most recent failure, including its HRESULT.
    [[nodiscard]] virtual std::wstring last_error() const = 0;
};

/// Non-destructive trim export (Phase 2.9 for MP4, Phase 3 for GIF).
enum class TrimVideoPipeline {
    Unknown,
    D3d11DxgiNv12,
    SystemMemoryNv12,
    BoundedBgraFallback,
};

struct TrimExportDiagnostics final {
    TrimVideoPipeline video_pipeline{TrimVideoPipeline::Unknown};
    std::wstring adapter_name;
    uint64_t source_open_ms{0};
    uint64_t decode_process_ms{0};
    uint64_t encode_finalize_ms{0};
    uint64_t publication_ms{0};
    uint64_t total_ms{0};
};

enum class TrimExportFailureStage {
    None,
    SourceOpen,
    SourceValidation,
    MediaTypeNegotiation,
    VideoDecode,
    DecodedFrameAccess,
    SourceReset,
    PaletteQuantization,
    GifEncoding,
    StagingWrite,
    Publication,
    Unknown,
};

/// Actionable failure information retained across the asynchronous export
/// boundary. HRESULT is represented as a signed 32-bit value so this media
/// contract does not require Windows headers at every call site.
struct TrimExportFailure final {
    TrimExportFailureStage stage{TrimExportFailureStage::None};
    bool has_hresult{false};
    int32_t hresult{0};
    std::wstring detail;
};

class ITrimExporter {
public:
    /// Progress in [0..1]; returning false cancels the export.
    using ProgressCallback = std::function<bool(float)>;

    virtual ~ITrimExporter() = default;

    /// Streams the half-open interval [start, end) out of `source_path` into
    /// `destination_path`.
    /// Implementations must never decode the whole file into RAM.
    virtual bool Export(const std::wstring& source_path, const std::wstring& destination_path, MediaTime start,
                        MediaTime end, const RecordingOptions& options,
                        ProgressCallback progress,
                        bool replace_existing = false) = 0;

    /// Best-effort output size estimate shown before a GIF export.
    [[nodiscard]] virtual std::optional<uint64_t> EstimateOutputBytes(const std::wstring& source_path,
                                                                      MediaTime start, MediaTime end,
                                                                      const RecordingOptions& options) const = 0;
    [[nodiscard]] virtual TrimExportDiagnostics LastDiagnostics() const {
        return {};
    }
    [[nodiscard]] virtual TrimExportFailure LastFailure() const { return {}; }
};

/// Reserved extension point for v1.1 scrolling screenshots.
///
/// Intentionally unused by every v1 capture path: implementing it must not
/// require changes to the coordinator, overlay or editor.
class IScrollshotProvider {
public:
    virtual ~IScrollshotProvider() = default;

    /// Probes whether the window under `origin` exposes a scrollable surface.
    [[nodiscard]] virtual bool IsSupportedTarget(POINT origin) const = 0;
    /// Captures and stitches the scrollable content of the target region.
    virtual bool CaptureStitched(const RECT& region, platform::BgraImage& out) = 0;
};

// Phase 2/3 factories.
std::unique_ptr<IAudioCaptureProvider> CreateWasapiAudioCaptureProvider();

std::unique_ptr<ITrimExporter> CreateMp4TrimExporter();
std::unique_ptr<ITrimExporter> CreateGifExporter();

}  // namespace easnap::media
