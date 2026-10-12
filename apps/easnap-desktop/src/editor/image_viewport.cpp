// Easnap - viewport onto an image that can be larger than the screen.
#include "editor/image_viewport.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace easnap::editor {
namespace {

constexpr std::array<float, 13> kZoomLadder{0.1f, 0.125f, 0.25f, 0.33f, 0.5f, 0.67f, 0.75f,
                                            1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f};

[[nodiscard]] LONG Width(const RECT& rect) noexcept { return rect.right - rect.left; }
[[nodiscard]] LONG Height(const RECT& rect) noexcept { return rect.bottom - rect.top; }

}  // namespace

D2D1_POINT_2F ImageView::origin() const noexcept {
    return D2D1::Point2F(static_cast<float>(viewport.left) - std::round(scroll_x * zoom),
                         static_cast<float>(viewport.top) - std::round(scroll_y * zoom));
}

D2D1_POINT_2F ImageView::ToImage(POINT desktop) const noexcept {
    const D2D1_POINT_2F start = origin();
    const float scale = zoom > 0.0f ? zoom : 1.0f;
    return D2D1::Point2F((static_cast<float>(desktop.x) - start.x) / scale,
                         (static_cast<float>(desktop.y) - start.y) / scale);
}

D2D1_POINT_2F ImageView::ToDesktop(D2D1_POINT_2F image) const noexcept {
    const D2D1_POINT_2F start = origin();
    return D2D1::Point2F(start.x + image.x * zoom, start.y + image.y * zoom);
}

float ImageView::top_fraction() const noexcept {
    return image_height > 0 ? std::clamp(scroll_y / static_cast<float>(image_height), 0.0f, 1.0f) : 0.0f;
}

float ImageView::visible_fraction() const noexcept {
    if (image_height <= 0 || zoom <= 0.0f) { return 1.0f; }
    return std::clamp(static_cast<float>(Height(viewport)) / zoom / static_cast<float>(image_height), 0.0f, 1.0f);
}

float FitWidthZoom(int image_width, const RECT& area) noexcept {
    if (image_width <= 0 || Width(area) <= 0) { return 1.0f; }
    return std::clamp(static_cast<float>(Width(area)) / static_cast<float>(image_width), kMinimumZoom, 1.0f);
}

ImageView LayoutImageView(int image_width, int image_height, const RECT& area, LONG preferred_left, float zoom,
                          float scroll_x, float scroll_y) noexcept {
    ImageView view;
    view.image_width = std::max(1, image_width);
    view.image_height = std::max(1, image_height);
    view.zoom = std::clamp(zoom, kMinimumZoom, kMaximumZoom);

    const LONG shown_width = static_cast<LONG>(std::ceil(static_cast<float>(view.image_width) * view.zoom));
    const LONG shown_height = static_cast<LONG>(std::ceil(static_cast<float>(view.image_height) * view.zoom));
    const LONG width = std::clamp<LONG>(shown_width, 1, std::max<LONG>(1, Width(area)));
    const LONG height = std::clamp<LONG>(shown_height, 1, std::max<LONG>(1, Height(area)));
    const LONG left = std::clamp(preferred_left, area.left, std::max(area.left, area.right - width));
    const LONG top = area.top + std::max<LONG>(0, (Height(area) - height) / 2);
    view.viewport = RECT{left, top, left + width, top + height};

    // The scroll position is kept on whole desktop pixels.
    const float limit_x = std::max(0.0f, static_cast<float>(view.image_width) - static_cast<float>(width) / view.zoom);
    const float limit_y = std::max(0.0f, static_cast<float>(view.image_height) - static_cast<float>(height) / view.zoom);
    view.scroll_x = std::round(std::clamp(scroll_x, 0.0f, limit_x) * view.zoom) / view.zoom;
    view.scroll_y = std::round(std::clamp(scroll_y, 0.0f, limit_y) * view.zoom) / view.zoom;
    return view;
}

float StepZoom(float zoom, int direction) noexcept {
    if (direction > 0) {
        for (float step : kZoomLadder) {
            if (step > zoom * 1.001f) { return step; }
        }
        return kZoomLadder.back();
    }
    for (auto step = kZoomLadder.rbegin(); step != kZoomLadder.rend(); ++step) {
        if (*step < zoom * 0.999f) { return *step; }
    }
    return kZoomLadder.front();
}

ImageView ZoomImageView(const ImageView& view, const RECT& area, LONG preferred_left, float zoom,
                        POINT anchor) noexcept {
    const float wanted = std::clamp(zoom, kMinimumZoom, kMaximumZoom);
    // The image point under the anchor, then the scroll that puts it back
    // under the same desktop point at the new scale.
    const D2D1_POINT_2F fixed = view.ToImage(anchor);
    ImageView next = LayoutImageView(view.image_width, view.image_height, area, preferred_left, wanted, 0.0f, 0.0f);
    const float scroll_x = fixed.x - static_cast<float>(anchor.x - next.viewport.left) / wanted;
    const float scroll_y = fixed.y - static_cast<float>(anchor.y - next.viewport.top) / wanted;
    return LayoutImageView(view.image_width, view.image_height, area, preferred_left, wanted, scroll_x, scroll_y);
}

}  // namespace easnap::editor
