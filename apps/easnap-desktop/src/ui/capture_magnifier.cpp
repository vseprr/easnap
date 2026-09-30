#include "ui/capture_magnifier.h"

#include <algorithm>
#include <array>
#include <cwchar>

namespace easnap::ui {

std::optional<COLORREF> SampleDesktopPixel(
    std::span<const uint32_t> pixels, int width, int height,
    const RECT& desktop_bounds, POINT cursor) noexcept {
    if (width <= 0 || height <= 0 || !::PtInRect(&desktop_bounds, cursor)) {
        return std::nullopt;
    }
    const auto x = static_cast<int64_t>(cursor.x) - desktop_bounds.left;
    const auto y = static_cast<int64_t>(cursor.y) - desktop_bounds.top;
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return std::nullopt;
    }
    const auto index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
    if (index >= pixels.size()) {
        return std::nullopt;
    }
    const uint32_t bgra = pixels[index];
    return RGB((bgra >> 16) & 0xff, (bgra >> 8) & 0xff, bgra & 0xff);
}

std::wstring FormatCaptureColor(COLORREF color, ColorRepresentation representation) {
    wchar_t text[40]{};
    if (representation == ColorRepresentation::Hex) {
        swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    } else {
        swprintf_s(text, L"RGB(%u, %u, %u)", GetRValue(color), GetGValue(color), GetBValue(color));
    }
    return text;
}

void CaptureColorReadout::OnKeyDown(WPARAM key, bool repeated, bool control, bool alt) noexcept {
    if (key == VK_SHIFT) {
        if (!repeated) {
            shift_candidate_ = !control && !alt;
        }
    } else {
        shift_candidate_ = false;
    }
}

bool CaptureColorReadout::OnKeyUp(WPARAM key) noexcept {
    if (key != VK_SHIFT) {
        return false;
    }
    const bool toggle = shift_candidate_;
    shift_candidate_ = false;
    if (toggle) {
        representation_ = representation_ == ColorRepresentation::Rgb
            ? ColorRepresentation::Hex : ColorRepresentation::Rgb;
    }
    return toggle;
}

RECT PlaceCaptureMagnifier(
    POINT cursor, const RECT& monitor, SIZE size, LONG gap, const RECT& toolbar) noexcept {
    const LONG width = std::clamp(size.cx, 0L, std::max(0L, monitor.right - monitor.left));
    const LONG height = std::clamp(size.cy, 0L, std::max(0L, monitor.bottom - monitor.top));
    const auto at = [&](LONG x, LONG y) {
        x = std::clamp(x, monitor.left, monitor.right - width);
        y = std::clamp(y, monitor.top, monitor.bottom - height);
        return RECT{x, y, x + width, y + height};
    };
    const std::array<RECT, 4> candidates{
        at(cursor.x + gap, cursor.y + gap),
        at(cursor.x - gap - width, cursor.y + gap),
        at(cursor.x + gap, cursor.y - gap - height),
        at(cursor.x - gap - width, cursor.y - gap - height)};
    for (const RECT& candidate : candidates) {
        RECT overlap{};
        if (!::PtInRect(&candidate, cursor) && !::IntersectRect(&overlap, &candidate, &toolbar)) {
            return candidate;
        }
    }
    // At a top-edge toolbar, reserve the space below it even on small displays.
    // If the display cannot fit both surfaces, shrink the magnifier window;
    // the toolbar remains visible and the magnifier never escapes the monitor.
    RECT result = candidates.front();
    RECT overlap{};
    if (::IntersectRect(&overlap, &result, &toolbar)) {
        result.top = std::clamp(toolbar.bottom + gap, monitor.top, monitor.bottom);
        result.bottom = std::min(monitor.bottom, result.top + height);
    }
    return result;
}

}  // namespace easnap::ui
