// Easnap - viewport onto an image that can be larger than the screen.
//
// Pure geometry: which rectangle of the desktop shows the image, at what
// scale, and which image pixel sits at its top-left corner. The screenshot
// editor uses it to edit a stitched Scrollshot; a plain screenshot is the
// trivial case (scale 1, no scrolling, viewport equal to the image).
#pragma once

#include <windows.h>

#include <d2d1.h>

namespace easnap::editor {

/// One consistent state of the viewport.
struct ImageView {
    RECT viewport{};  ///< Desktop pixels that show the image.
    int image_width{0};
    int image_height{0};
    float zoom{1.0f};      ///< Desktop pixels per image pixel.
    float scroll_x{0.0f};  ///< Image pixel at the viewport's left edge.
    float scroll_y{0.0f};  ///< Image pixel at the viewport's top edge.

    /// Desktop position of image pixel (0, 0); whole pixels, so an unscaled
    /// image is shown without resampling.
    [[nodiscard]] D2D1_POINT_2F origin() const noexcept;
    [[nodiscard]] D2D1_POINT_2F ToImage(POINT desktop) const noexcept;
    [[nodiscard]] D2D1_POINT_2F ToDesktop(D2D1_POINT_2F image) const noexcept;
    /// Share of the image height above the viewport and inside it, for a scroll indicator.
    [[nodiscard]] float top_fraction() const noexcept;
    [[nodiscard]] float visible_fraction() const noexcept;
};

inline constexpr float kMinimumZoom = 0.1f;
inline constexpr float kMaximumZoom = 4.0f;

/// Largest scale, at most 1, at which the image's width fits `area`.
[[nodiscard]] float FitWidthZoom(int image_width, const RECT& area) noexcept;

/// Places the viewport inside `area` for the wanted scale and scroll position.
/// The viewport is as large as the scaled image, limited by `area`; it starts
/// at `preferred_left` when the area allows and is centred vertically when the
/// image is shorter than the area. Scale and scroll are clamped to what the
/// image offers.
[[nodiscard]] ImageView LayoutImageView(int image_width, int image_height, const RECT& area, LONG preferred_left,
                                        float zoom, float scroll_x, float scroll_y) noexcept;

/// The next step up (`direction` > 0) or down on the zoom ladder.
[[nodiscard]] float StepZoom(float zoom, int direction) noexcept;

/// Changes the scale so that the image point under `anchor` stays under it.
[[nodiscard]] ImageView ZoomImageView(const ImageView& view, const RECT& area, LONG preferred_left, float zoom,
                                      POINT anchor) noexcept;

}  // namespace easnap::editor
