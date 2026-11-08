// Easnap - geometry of pinned images (ADR-038).
#include "ui/pin_layout.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace easnap::ui {
namespace {

/// The wheel steps through these zooms; 1.0 is always one of them, so a pin
/// can always return to its captured size.
constexpr std::array<double, 18> kZoomSteps{0.10, 0.15, 0.20, 0.25, 0.33, 0.50, 0.67, 0.75, 0.90,
                                            1.00, 1.10, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00, 4.00};
static_assert(kZoomSteps.front() == kPinMinimumZoom && kZoomSteps.back() == kPinMaximumZoom);
constexpr double kZoomTolerance = 1e-6;

LONG Width(const RECT& r) noexcept { return r.right - r.left; }
LONG Height(const RECT& r) noexcept { return r.bottom - r.top; }
LONG Scaled(float dip, float scale) noexcept { return static_cast<LONG>(std::lround(dip * scale)); }
LONG ImagePixels(LONG side, double zoom) noexcept {
    return std::max<LONG>(1, static_cast<LONG>(std::lround(static_cast<double>(side) * zoom)));
}

RECT Overlap(const RECT& a, const RECT& b) noexcept {
    RECT r{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
    if (r.right < r.left) { r.right = r.left; }
    if (r.bottom < r.top) { r.bottom = r.top; }
    return r;
}

/// The start of a span of `size` placed at `start` and moved inside [low, high).
/// A span longer than the room starts at `low`.
LONG Contain(LONG start, LONG size, LONG low, LONG high) noexcept {
    if (size >= high - low) { return low; }
    return std::clamp(start, low, high - size);
}

}  // namespace

LONG PinBorderPixels(float scale) noexcept { return std::max<LONG>(1, Scaled(kPinBorder, scale)); }

PinPlacement InitialPinPlacement(SIZE image, const RECT& source, const RECT& work_area, float scale) noexcept {
    if (image.cx <= 0 || image.cy <= 0) { return {}; }
    const LONG border = PinBorderPixels(scale);
    if (image.cx == Width(source) && image.cy == Height(source)) {
        return {{source.left - border, source.top - border, source.right + border, source.bottom + border}, 1.0};
    }
    const LONG room_width = std::max<LONG>(1, Width(work_area) - 2 * border);
    const LONG room_height = std::max<LONG>(1, Height(work_area) - 2 * border);
    const double zoom = std::min({1.0, static_cast<double>(room_width) / image.cx, static_cast<double>(room_height) / image.cy});
    const LONG width = ImagePixels(image.cx, zoom) + 2 * border;
    const LONG height = ImagePixels(image.cy, zoom) + 2 * border;
    const LONG left = work_area.left + (Width(work_area) - width) / 2;
    const LONG top = work_area.top + (Height(work_area) - height) / 2;
    return {{left, top, left + width, top + height}, zoom};
}

double NextPinZoom(double zoom, int direction, SIZE image, float scale) noexcept {
    if (direction == 0 || image.cx <= 0 || image.cy <= 0) { return zoom; }
    double next = zoom;
    if (direction > 0) {
        const auto it = std::find_if(kZoomSteps.begin(), kZoomSteps.end(), [&](double step) { return step > zoom + kZoomTolerance; });
        if (it != kZoomSteps.end()) { next = *it; }
    } else {
        const auto it = std::find_if(kZoomSteps.rbegin(), kZoomSteps.rend(), [&](double step) { return step < zoom - kZoomTolerance; });
        if (it != kZoomSteps.rend()) { next = *it; }
    }
    if (next == zoom) { return zoom; }
    const double shorter = static_cast<double>(std::min(image.cx, image.cy)) * next;
    const double longer = static_cast<double>(std::max(image.cx, image.cy)) * next;
    if (next < zoom && shorter < kPinMinimumSide * scale) { return zoom; }
    if (next > zoom && longer > static_cast<double>(kPinMaximumSide)) { return zoom; }
    return next;
}

RECT ZoomPinAround(const RECT& window, double old_zoom, double new_zoom, SIZE image, POINT anchor, float scale) noexcept {
    if (old_zoom <= 0.0 || new_zoom <= 0.0 || image.cx <= 0 || image.cy <= 0) { return window; }
    const LONG border = PinBorderPixels(scale);
    const double ratio = new_zoom / old_zoom;
    const double image_left = static_cast<double>(window.left + border);
    const double image_top = static_cast<double>(window.top + border);
    const LONG left = static_cast<LONG>(std::lround(anchor.x - (anchor.x - image_left) * ratio)) - border;
    const LONG top = static_cast<LONG>(std::lround(anchor.y - (anchor.y - image_top) * ratio)) - border;
    return {left, top, left + ImagePixels(image.cx, new_zoom) + 2 * border, top + ImagePixels(image.cy, new_zoom) + 2 * border};
}

double PinZoomForWindow(const RECT& window, SIZE image, float scale) noexcept {
    if (image.cx <= 0) { return 1.0; }
    const LONG shown = std::max<LONG>(1, Width(window) - 2 * PinBorderPixels(scale));
    return static_cast<double>(shown) / image.cx;
}

RECT KeepPinOnScreen(const RECT& window, const std::vector<RECT>& work_areas, const RECT& nearest_work_area, float scale) noexcept {
    const LONG need = Scaled(kPinVisibleMinimum, scale);
    const LONG need_width = std::min(need, Width(window));
    const LONG need_height = std::min(need, Height(window));
    for (const RECT& area : work_areas) {
        const RECT visible = Overlap(window, area);
        if (Width(visible) >= need_width && Height(visible) >= need_height && Width(visible) > 0 && Height(visible) > 0) {
            return window;
        }
    }
    const LONG left = Contain(window.left, Width(window), nearest_work_area.left, nearest_work_area.right);
    const LONG top = Contain(window.top, Height(window), nearest_work_area.top, nearest_work_area.bottom);
    return {left, top, left + Width(window), top + Height(window)};
}

}  // namespace easnap::ui
