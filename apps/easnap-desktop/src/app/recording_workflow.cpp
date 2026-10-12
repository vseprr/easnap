#include "app/recording_workflow.h"

#include <algorithm>
#include <cwchar>
#include <limits>

namespace easnap::app {

bool IsSupportedRecordingAudioSource(media::AudioSource source) noexcept {
    return std::find(kSupportedRecordingAudioSources.begin(),
                     kSupportedRecordingAudioSources.end(),
                     source) != kSupportedRecordingAudioSources.end();
}

bool IsSupportedRecordingFormat(media::RecordingFormat format) noexcept {
    return std::find(kSupportedRecordingFormats.begin(),
                     kSupportedRecordingFormats.end(),
                     format) != kSupportedRecordingFormats.end();
}

bool IsValidGifExportConfiguration(
    const GifExportConfiguration& configuration) noexcept {
    return configuration.preset == GifExportPreset::HighQuality ||
           configuration.preset == GifExportPreset::LowSize;
}

RecordingCapabilities CapabilitiesForRecordingFormat(
    media::RecordingFormat format) noexcept {
    switch (format) {
        case media::RecordingFormat::Mp4:
            return RecordingCapabilities{true, true, false, false};
        case media::RecordingFormat::Gif:
            return RecordingCapabilities{false, true, true, true};
    }
    return {};
}

media::AudioSource EffectiveAudioSource(
    const RecordingConfiguration& configuration) noexcept {
    return CapabilitiesForRecordingFormat(configuration.format).supports_audio
               ? configuration.mp4_audio
               : media::AudioSource::None;
}

bool IsValidRecordingConfiguration(
    const RecordingConfiguration& configuration) noexcept {
    return IsSupportedRecordingFormat(configuration.format) &&
           IsSupportedRecordingAudioSource(configuration.mp4_audio) &&
           IsValidGifExportConfiguration(configuration.gif);
}

media::RecordingOptions MakeRecordingOptions(
    const RECT& region, const RecordingConfiguration& configuration) noexcept {
    media::RecordingOptions options;
    options.region = region;
    options.format = configuration.format;
    options.audio = EffectiveAudioSource(configuration);
    options.include_cursor = configuration.include_cursor;
    options.gif_width = 0;
    options.gif_height = 0;
    options.gif_fps = configuration.gif.preset == GifExportPreset::LowSize
                          ? 12u
                          : 15u;
    options.gif_palette_colors = 256;
    options.gif_dither = true;
    options.gif_low_size =
        configuration.gif.preset == GifExportPreset::LowSize;
    options.gif_sparse_transparency = options.gif_low_size;
    options.gif_distributed_cadence = options.gif_low_size;
    return options;
}

const wchar_t* RecordingFileExtension(media::RecordingFormat format) noexcept {
    return format == media::RecordingFormat::Gif ? L"gif" : L"mp4";
}

RecordingControlPolicy RecordingControlsForState(SessionState state) noexcept {
    RecordingControlPolicy policy;
    switch (state) {
        case SessionState::RecordingConfiguring:
            policy.can_start = true;
            policy.can_cancel = true;
            break;
        case SessionState::RecordingActive:
            policy.can_pause = true;
            policy.can_stop = true;
            break;
        case SessionState::RecordingPaused:
            policy.can_resume = true;
            policy.can_stop = true;
            break;
        default:
            break;
    }
    return policy;
}

bool RecordingRegionFitsH264(const RECT& region) noexcept {
    constexpr int64_t kMaximumMacroblocks = 36864;
    constexpr int64_t kMaximumSide = 8688;
    const int64_t width = static_cast<int64_t>(region.right) - region.left;
    const int64_t height = static_cast<int64_t>(region.bottom) - region.top;
    if (width <= 0 || height <= 0 || width > kMaximumSide || height > kMaximumSide) {
        return false;
    }
    return ((width + 15) / 16) * ((height + 15) / 16) <= kMaximumMacroblocks;
}

RECT MakeRecordingRegionEncoderCompatible(const RECT& region) noexcept {
    RECT result = region;
    const int64_t width = static_cast<int64_t>(result.right) - result.left;
    const int64_t height = static_cast<int64_t>(result.bottom) - result.top;
    if (width <= 0 || height <= 0) {
        return RECT{};
    }
    if ((width & 1) != 0) {
        --result.right;
    }
    if ((height & 1) != 0) {
        --result.bottom;
    }
    if (result.right <= result.left || result.bottom <= result.top) {
        return RECT{};
    }
    return result;
}

std::wstring FormatRecordingElapsed(uint64_t active_duration_100ns) {
    constexpr uint64_t kTicksPerSecond = 10'000'000;
    const uint64_t total_seconds = active_duration_100ns / kTicksPerSecond;
    const uint64_t hours = total_seconds / 3'600;
    const uint64_t minutes = (total_seconds / 60) % 60;
    const uint64_t seconds = total_seconds % 60;

    wchar_t text[32]{};
    static_cast<void>(::_snwprintf_s(
        text, _TRUNCATE, L"%02llu:%02llu:%02llu",
        static_cast<unsigned long long>(hours),
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds)));
    return text;
}

}  // namespace easnap::app
