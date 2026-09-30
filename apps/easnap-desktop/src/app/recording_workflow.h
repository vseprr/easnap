// Easnap - deterministic policy for the user-facing recording workflow.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "app/session_transitions.h"
#include "media/media_interfaces.h"

namespace easnap::app {

/// Audio choices that the production recording session can currently create.
/// Includes AudioSource::Both, which routes to the composed dual-source WASAPI
/// provider and real-time mixer.
inline constexpr std::array<media::AudioSource, 4> kSupportedRecordingAudioSources{
    media::AudioSource::None,
    media::AudioSource::SystemAudio,
    media::AudioSource::Microphone,
    media::AudioSource::Both,
};

inline constexpr std::array<media::RecordingFormat, 2> kSupportedRecordingFormats{
    media::RecordingFormat::Mp4,
    media::RecordingFormat::Gif,
};

enum class GifExportPreset { HighQuality, LowSize };

/// Product-level GIF policy. Encoder details such as palette size, disposal and
/// LZW behavior deliberately stay behind this boundary.
struct GifExportConfiguration final {
    GifExportPreset preset{GifExportPreset::HighQuality};
};

struct RecordingCapabilities final {
    bool supports_audio{false};
    bool supports_cursor{true};
    bool requires_final_conversion{false};
    bool shows_gif_quality{false};
};

/// Presentation-independent state consumed by the current Win32 controls and
/// carried through the shared recording lifecycle.
struct RecordingConfiguration final {
    media::RecordingFormat format{media::RecordingFormat::Mp4};
    media::AudioSource mp4_audio{media::AudioSource::None};
    bool include_cursor{false};
    GifExportConfiguration gif{};
};

struct RecordingControlPolicy {
    bool can_start{false};
    bool can_cancel{false};
    bool can_pause{false};
    bool can_resume{false};
    bool can_stop{false};
};

[[nodiscard]] bool IsSupportedRecordingAudioSource(media::AudioSource source) noexcept;
[[nodiscard]] bool IsSupportedRecordingFormat(media::RecordingFormat format) noexcept;
[[nodiscard]] bool IsValidGifExportConfiguration(
    const GifExportConfiguration& configuration) noexcept;
[[nodiscard]] RecordingCapabilities CapabilitiesForRecordingFormat(
    media::RecordingFormat format) noexcept;
[[nodiscard]] media::AudioSource EffectiveAudioSource(
    const RecordingConfiguration& configuration) noexcept;
[[nodiscard]] bool IsValidRecordingConfiguration(
    const RecordingConfiguration& configuration) noexcept;
[[nodiscard]] media::RecordingOptions MakeRecordingOptions(
    const RECT& region, const RecordingConfiguration& configuration) noexcept;
[[nodiscard]] const wchar_t* RecordingFileExtension(
    media::RecordingFormat format) noexcept;
[[nodiscard]] RecordingControlPolicy RecordingControlsForState(SessionState state) noexcept;

/// H.264/NV12 requires even dimensions. Preserve the selected origin and trim
/// at most one pixel from the right/bottom edges.
[[nodiscard]] RECT MakeRecordingRegionEncoderCompatible(const RECT& region) noexcept;

/// Formats the shared active-time media clock as HH:MM:SS.
[[nodiscard]] std::wstring FormatRecordingElapsed(uint64_t active_duration_100ns);

}  // namespace easnap::app
