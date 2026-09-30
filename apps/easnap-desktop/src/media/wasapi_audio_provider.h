// Easnap - production WASAPI audio capture provider.
#pragma once

#include <memory>

#include "media/media_interfaces.h"

namespace easnap::media {

namespace detail {

/// Clock observations captured immediately after IAudioCaptureClient::GetBuffer.
/// Absolute QPC values use WASAPI's 100 ns QPC representation.
struct AudioTimestampResolutionInput {
    int64_t qpc_epoch_100ns{0};
    uint64_t raw_wasapi_qpc_100ns{0};
    bool wasapi_timestamp_error{false};
    uint64_t device_position{0};
    uint32_t frame_count{0};
    uint32_t buffered_frame_count{0};
    uint32_t sample_rate{0};
    uint64_t audio_clock_position{0};
    uint64_t audio_clock_frequency{0};
    uint64_t audio_clock_qpc_100ns{0};
    uint64_t capture_qpc_100ns{0};
    /// Negotiated WASAPI buffer size in frames. GetCurrentPadding values
    /// larger than this are capped in the CaptureQpcFallback path to
    /// prevent endpoint-wakeup inflation from overshooting the fallback
    /// timestamp into the past.
    uint32_t maximum_buffer_frames{0};
};

struct AudioTimestampResolutionResult {
    MediaTime raw_wasapi_timestamp{0};
    MediaTime audio_clock_timestamp{0};
    MediaTime capture_fallback_timestamp{0};
    MediaTime resolved_timestamp{0};
    AudioTimestampSource source{AudioTimestampSource::CaptureQpcFallback};
    bool raw_wasapi_plausible{false};
    bool audio_clock_correlation_valid{false};
};

/// Selects a packet-first-frame timestamp without accepting flagged, stale,
/// pre-epoch, or future raw WASAPI clock values.
[[nodiscard]] AudioTimestampResolutionResult ResolveAudioPacketTimestamp(
    const AudioTimestampResolutionInput& input) noexcept;

/// Creates one native-format endpoint provider for composition.
std::unique_ptr<IAudioCaptureProvider> CreateWasapiEndpointCaptureProvider();

/// Deterministic composition seam for dual-source contract tests. Production
/// callers use CreateWasapiAudioCaptureProvider instead.
std::unique_ptr<IAudioCaptureProvider> CreateDualAudioCaptureProviderForTesting(
    std::unique_ptr<IAudioCaptureProvider> system,
    std::unique_ptr<IAudioCaptureProvider> microphone);

}  // namespace detail

/// Creates the production WASAPI audio capture provider.
///
/// Supports AudioSource::SystemAudio, AudioSource::Microphone, and a composed
/// AudioSource::Both path with independent endpoint clocks and one mixed stream.
///
/// Event-driven WASAPI capture runs on a dedicated MTA thread. Format is
/// negotiated from IAudioClient::GetMixFormat for each endpoint. Both uses the
/// Windows Audio Resampler DSP to convert each native stream into the canonical
/// mix format before bounded clock-aligned mixing.
/// Internal buffering is bounded to a 500 ms time budget.
///
/// The returned provider is idle until Start is called.
std::unique_ptr<IAudioCaptureProvider> CreateWasapiAudioCaptureProvider();

}  // namespace easnap::media
