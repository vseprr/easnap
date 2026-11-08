// Easnap - geometry of pinned images (ADR-038).
//
// Pure functions over desktop pixels: where a new pin goes, the zoom steps of
// the wheel, zooming around the pointer and keeping a pin on a work area. A
// pin's window is its image plus a border outside it.
#pragma once

#include <windows.h>

#include <vector>

namespace easnap::ui {

/// Zoom of a pinned image, relative to its captured pixels.
inline constexpr double kPinMinimumZoom = 0.1;
inline constexpr double kPinMaximumZoom = 4.0;
/// The border around the image, in DIPs.
inline constexpr float kPinBorder = 1.0f;
/// A wheel step never makes the shorter side of the image smaller than this,
/// in DIPs ...
inline constexpr float kPinMinimumSide = 24.0f;
/// ... nor its longer side larger than this, in pixels.
inline constexpr LONG kPinMaximumSide = 16384;
/// How much of a pin must remain on a work area after a display change, in
/// DIPs; a pin with less is moved back.
inline constexpr float kPinVisibleMinimum = 48.0f;

struct PinPlacement {
    RECT window{};  ///< Desktop pixels, border included.
    double zoom{1.0};
};

/// The border in pixels at `scale`: never less than one pixel.
[[nodiscard]] LONG PinBorderPixels(float scale) noexcept;

/// Where a new pin goes. An image the size of `source` (a screenshot) covers
/// its source exactly, at 100 %. Any other image is scaled down to fit
/// `work_area` and centred on it. Empty for an empty image.
[[nodiscard]] PinPlacement InitialPinPlacement(SIZE image, const RECT& source, const RECT& work_area,
                                               float scale) noexcept;

/// The zoom one wheel step from `zoom`: the next larger step for a positive
/// `direction`, the next smaller one for a negative one. Returns `zoom` when
/// there is no such step or the step would break a side limit.
[[nodiscard]] double NextPinZoom(double zoom, int direction, SIZE image, float scale) noexcept;

/// The window that shows the image at `new_zoom`, with the image point under
/// `anchor` (desktop pixels) left where it is.
[[nodiscard]] RECT ZoomPinAround(const RECT& window, double old_zoom, double new_zoom, SIZE image, POINT anchor,
                                 float scale) noexcept;

/// The zoom a window of this size shows, measured on its width.
[[nodiscard]] double PinZoomForWindow(const RECT& window, SIZE image, float scale) noexcept;

/// `window` as it is while at least kPinVisibleMinimum of it (or all of it,
/// when it is smaller) is on one of `work_areas`. Otherwise it is moved into
/// `nearest_work_area`, keeping its size; a pin larger than that area is
/// aligned with its top-left corner.
[[nodiscard]] RECT KeepPinOnScreen(const RECT& window, const std::vector<RECT>& work_areas,
                                   const RECT& nearest_work_area, float scale) noexcept;

}  // namespace easnap::ui
