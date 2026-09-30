// Easnap - deterministic DPI-aware trim-window layout geometry.
#pragma once

namespace easnap::ui {

inline constexpr int kTrimMinClientWidthDip = 480;
inline constexpr int kTrimMinClientHeightDip = 400;
inline constexpr int kTrimChromeHeightDip = 40;

struct TrimLayoutRect final {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] int right() const noexcept { return x + width; }
    [[nodiscard]] int bottom() const noexcept { return y + height; }
};

struct TrimWindowLayout final {
    TrimLayoutRect title;
    TrimLayoutRect time_label;
    TrimLayoutRect video;
    TrimLayoutRect range_label;
    TrimLayoutRect timeline;
    TrimLayoutRect help;
    TrimLayoutRect status;
    TrimLayoutRect progress;
    TrimLayoutRect play_pause;
    TrimLayoutRect set_in;
    TrimLayoutRect set_out;
    TrimLayoutRect save_or_cancel;
    TrimLayoutRect copy;
    TrimLayoutRect discard;
    // Timeline-local geometry shared by layout and painting.
    int timeline_label_height{0};
    int timeline_track_center{0};
};

struct TrimTextMetrics final {
    int line_height{0};
    int time_height{0};
    int range_height{0};
};

/// Calculates a bottom-anchored layout. Save and Cancel intentionally share
/// one slot because they are mutually exclusive, keeping every button inside
/// the client width at the minimum supported size.
[[nodiscard]] TrimWindowLayout CalculateTrimWindowLayout(
    int client_width, int client_height, float dpi_scale,
    int status_height = 0, bool show_progress = false,
    TrimTextMetrics text = {}) noexcept;

/// Fits without stretching; surplus width/height is consumed before the image
/// itself shrinks. Width/height are source pixels, not UI dimensions.
[[nodiscard]] TrimLayoutRect FitTrimVideo(TrimLayoutRect area, int source_width,
                                         int source_height) noexcept;
[[nodiscard]] TrimLayoutRect InitialTrimWindowSize(int work_width, int work_height,
    float dpi_scale, int source_width, int source_height) noexcept;

[[nodiscard]] bool IsTrimWindowLayoutContained(
    const TrimWindowLayout& layout, int client_width,
    int client_height) noexcept;

}  // namespace easnap::ui
