// Easnap - presentation-independent animated GIF encoding.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "media/media_interfaces.h"

namespace easnap::media {

/// Palette mapping policy. Sharp avoids dithering for pixel/UI content;
/// Balanced uses stable canvas-anchored ordered dithering for gradients.
enum class GifQuantizationPolicy { Sharp, Balanced, Rgb332Ordered };

/// Palette construction remains an encoder detail behind the product preset.
enum class GifPalettePolicy { AdaptiveMedianCut, FixedRgb332, FixedWebSafe };

struct GifEncodeOptions final {
    /// Zero preserves the source dimension. If exactly one dimension is zero,
    /// it is derived from the other while preserving source aspect ratio.
    uint32_t output_width{0};
    uint32_t output_height{0};
    /// Explicit backend cadence cap. GIF cannot reliably present sub-centisecond
    /// frames, so valid values are 1..100.
    uint32_t maximum_frame_rate{15};
    uint16_t maximum_palette_colors{256};
    GifQuantizationPolicy quantization{GifQuantizationPolicy::Balanced};
    GifPalettePolicy palette{GifPalettePolicy::AdaptiveMedianCut};
    bool transparent_sparse_deltas{false};
    bool distributed_cadence{false};
    /// Per-channel temporal deadband for lossy working recordings (0 disables).
    /// Compare with the last accepted source pixel, never the previous input,
    /// so gradual changes accumulate and cannot disappear indefinitely.
    uint8_t temporal_tolerance{0};
    /// GIF loop count: zero is the Netscape convention for infinite looping.
    uint16_t loop_count{0};
};

enum class GifFrameReadResult { Frame, End, Error };

/// Rewindable, sequential canonical-video source. Implementations retain only
/// a fixed number of BGRA frames; Reset starts the next deterministic pass.
class IGifFrameSource {
public:
    virtual ~IGifFrameSource() = default;

    [[nodiscard]] virtual uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual uint32_t height() const noexcept = 0;
    [[nodiscard]] virtual MediaTime duration() const noexcept = 0;
    /// Optional per-sample cancellation hook for sources that may internally
    /// scan/decode before yielding the next interval frame.
    virtual void SetCancellationProbe(std::function<bool()> probe) {
        static_cast<void>(probe);
    }
    virtual bool Reset() = 0;
    virtual GifFrameReadResult Read(VideoFrame& frame) = 0;
    [[nodiscard]] virtual std::wstring last_error() const = 0;
    [[nodiscard]] virtual TrimExportFailure last_failure() const { return {}; }
};

struct GifEncodeDiagnostics final {
    uint64_t source_frames_read{0};
    uint64_t adapted_frames{0};
    uint64_t encoded_frames{0};
    uint64_t unchanged_frames_coalesced{0};
    uint64_t delta_frames{0};
    uint64_t full_frames{0};
    uint64_t palette_samples{0};
    uint64_t palette_entries{0};
    uint64_t image_data_bytes{0};
    uint64_t compressed_image_bytes{0};
    uint64_t image_rectangle_pixels{0};
    uint64_t delta_rectangle_pixels{0};
    uint64_t transparent_delta_frames{0};
    uint64_t opaque_delta_fallback_frames{0};
    uint64_t duration_only_frames{0};
    uint64_t intended_duration_100ns{0};
    uint64_t encoded_duration_centiseconds{0};
    /// Conservative encoder working-set estimate; excludes the source's bounded
    /// decode buffers and never depends on animation duration.
    uint64_t peak_working_bytes{0};
};

using GifProgressCallback = std::function<bool(float)>;

/// Encodes a canonical source in one fixed-palette pass or two adaptive-palette
/// passes through a unique sibling
/// staging file, then atomically publishes only a finalized GIF89a stream.
/// The source is never opened for writing by this layer.
bool ExportGif(IGifFrameSource& source, const std::wstring& destination_path,
               const GifEncodeOptions& options, GifProgressCallback progress,
               bool replace_existing, GifEncodeDiagnostics& diagnostics,
               std::wstring& error, TrimExportFailure* failure = nullptr);

}  // namespace easnap::media
