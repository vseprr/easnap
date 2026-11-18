// Easnap - screen geometry: rectangles, monitors and DPI awareness.
//
// All coordinates are virtual-desktop pixels unless noted.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <vector>

namespace easnap::platform {

/// One display: its rectangles in virtual-screen pixels and its DPI.
struct MonitorInfo {
    HMONITOR handle{nullptr};
    RECT bounds{};     ///< Full monitor rectangle in virtual-screen pixels.
    RECT work_area{};  ///< Monitor rectangle without taskbar/app bars.
    UINT dpi{96};
    bool primary{false};

    [[nodiscard]] float scale() const noexcept { return static_cast<float>(dpi) / 96.0f; }
};

/// Makes the process per-monitor DPI aware where no manifest did.
void EnableDpiAwareness();
/// Every connected display; never empty.
[[nodiscard]] std::vector<MonitorInfo> EnumerateMonitors();
/// The display that contains `pt`, or the nearest one.
[[nodiscard]] MonitorInfo MonitorForPoint(POINT pt);
/// The display that holds most of `rect`, or the nearest one.
[[nodiscard]] MonitorInfo MonitorForRect(const RECT& rect);
/// The rectangle that spans every display.
[[nodiscard]] RECT VirtualScreenBounds();

/// The rectangle with corners `a` and `b`, given in either order.
[[nodiscard]] RECT NormalizedRect(POINT a, POINT b);
/// The overlap of `a` and `b`; all zero when they do not overlap.
[[nodiscard]] RECT IntersectRects(const RECT& a, const RECT& b);
/// True when `r` has no width or no height.
[[nodiscard]] bool IsEmptyRect(const RECT& r) noexcept;
/// Width of `r`.
[[nodiscard]] inline LONG RectWidth(const RECT& r) noexcept { return r.right - r.left; }
/// Height of `r`.
[[nodiscard]] inline LONG RectHeight(const RECT& r) noexcept { return r.bottom - r.top; }
/// `rect` moved, and shrunk where needed, to lie inside `bounds`.
[[nodiscard]] RECT ClampRectToBounds(RECT rect, const RECT& bounds);

}  // namespace easnap::platform
