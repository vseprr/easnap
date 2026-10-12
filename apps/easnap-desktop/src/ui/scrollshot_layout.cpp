// Easnap - placement of the Scrollshot island and live preview.
#include "ui/scrollshot_layout.h"

#include <algorithm>
#include <cstdlib>

namespace easnap::ui {
namespace {

using platform::MonitorInfo;

[[nodiscard]] bool Overlaps(const RECT& a, const RECT& b) noexcept {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

[[nodiscard]] bool Within(const RECT& inner, const RECT& outer) noexcept {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

[[nodiscard]] RECT Shrunk(RECT area, LONG gap) noexcept {
    area.left += gap;
    area.top += gap;
    area.right -= gap;
    area.bottom -= gap;
    return area;
}

[[nodiscard]] const MonitorInfo* MonitorAt(const std::vector<MonitorInfo>& monitors, const RECT& rect) noexcept {
    const POINT centre{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    for (const MonitorInfo& monitor : monitors) {
        if (::PtInRect(&monitor.bounds, centre) != FALSE) { return &monitor; }
    }
    return nullptr;
}

/// Moves `rect` into the usable area of the monitor it mostly lies on.
/// False when no monitor is there or the area is too small for it.
[[nodiscard]] bool Settle(const std::vector<MonitorInfo>& monitors, LONG gap, RECT& rect) noexcept {
    const MonitorInfo* monitor = MonitorAt(monitors, rect);
    if (monitor == nullptr) { return false; }
    const RECT area = Shrunk(monitor->work_area, gap);
    if (platform::RectWidth(rect) > platform::RectWidth(area) || platform::RectHeight(rect) > platform::RectHeight(area)) {
        return false;
    }
    rect = platform::ClampRectToBounds(rect, area);
    return Within(rect, area);
}

/// Monitors ordered by distance from the frame, nearest first.
[[nodiscard]] std::vector<const MonitorInfo*> ByDistance(const std::vector<MonitorInfo>& monitors, const RECT& frame) {
    std::vector<const MonitorInfo*> ordered;
    for (const MonitorInfo& monitor : monitors) { ordered.push_back(&monitor); }
    const auto distance = [&frame](const MonitorInfo* monitor) {
        const LONG dx = (monitor->bounds.left + monitor->bounds.right) / 2 - (frame.left + frame.right) / 2;
        const LONG dy = (monitor->bounds.top + monitor->bounds.bottom) / 2 - (frame.top + frame.bottom) / 2;
        return std::abs(dx) + std::abs(dy);
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const MonitorInfo* a, const MonitorInfo* b) {
        return distance(a) < distance(b);
    });
    return ordered;
}

}  // namespace

ScrollshotPlacement PlaceScrollshotSurfaces(const RECT& frame, const std::vector<MonitorInfo>& monitors,
                                            SIZE island, LONG preview_width, LONG preview_max_height, LONG gap) {
    ScrollshotPlacement result;
    const LONG centre_x = (frame.left + frame.right) / 2;
    const LONG centre_y = (frame.top + frame.bottom) / 2;
    const auto ordered = ByDistance(monitors, frame);

    // --- Island: below, above, right of, left of the frame ---------------------
    const RECT beside[] = {
        {centre_x - island.cx / 2, frame.bottom + gap, centre_x - island.cx / 2 + island.cx, frame.bottom + gap + island.cy},
        {centre_x - island.cx / 2, frame.top - gap - island.cy, centre_x - island.cx / 2 + island.cx, frame.top - gap},
        {frame.right + gap, centre_y - island.cy / 2, frame.right + gap + island.cx, centre_y - island.cy / 2 + island.cy},
        {frame.left - gap - island.cx, centre_y - island.cy / 2, frame.left - gap, centre_y - island.cy / 2 + island.cy},
    };
    bool placed = false;
    for (RECT candidate : beside) {
        if (Settle(monitors, gap, candidate) && !Overlaps(candidate, frame)) {
            result.island = candidate;
            placed = true;
            break;
        }
    }
    if (!placed) {
        // The frame leaves no room on its own monitor: the nearest other one.
        for (const MonitorInfo* monitor : ordered) {
            const RECT area = Shrunk(monitor->work_area, gap);
            RECT candidate{(area.left + area.right - island.cx) / 2, area.bottom - island.cy,
                           (area.left + area.right - island.cx) / 2 + island.cx, area.bottom};
            if (Within(candidate, area) && !Overlaps(candidate, frame)) {
                result.island = candidate;
                placed = true;
                break;
            }
        }
    }
    if (!placed) {
        // Nowhere else: over the bottom of the frame, on whichever monitor shows it.
        RECT candidate{centre_x - island.cx / 2, frame.bottom - gap - island.cy,
                       centre_x - island.cx / 2 + island.cx, frame.bottom - gap};
        if (!Settle(monitors, gap, candidate) && !ordered.empty()) {
            candidate = platform::ClampRectToBounds(candidate, ordered.front()->work_area);
        }
        result.island = candidate;
        result.island_over_frame = true;
    }

    // --- Preview: right of, left of the frame, as tall as there is room -------
    const LONG minimum_height = std::min(preview_max_height, preview_width);
    const auto fit_beside = [&](bool right, RECT& out) {
        RECT probe{right ? frame.right + gap : frame.left - gap - preview_width, frame.top,
                   right ? frame.right + gap + preview_width : frame.left - gap, frame.top + minimum_height};
        const MonitorInfo* monitor = MonitorAt(monitors, probe);
        if (monitor == nullptr) { return false; }
        const RECT area = Shrunk(monitor->work_area, gap);
        if (probe.left < area.left || probe.right > area.right) { return false; }
        const LONG top = std::clamp(frame.top, area.top, std::max(area.top, area.bottom - minimum_height));
        const LONG height = std::min(preview_max_height, area.bottom - top);
        if (height < minimum_height) { return false; }
        out = RECT{probe.left, top, probe.right, top + height};
        return !Overlaps(out, frame) && !Overlaps(out, result.island);
    };
    placed = fit_beside(true, result.preview) || fit_beside(false, result.preview);
    if (!placed) {
        for (const MonitorInfo* monitor : ordered) {
            const RECT area = Shrunk(monitor->work_area, gap);
            const LONG height = std::min(preview_max_height, platform::RectHeight(area));
            for (const bool right : {true, false}) {
                const LONG left = right ? area.right - preview_width : area.left;
                const RECT candidate{left, area.top, left + preview_width, area.top + height};
                if (height >= minimum_height && Within(candidate, area) && !Overlaps(candidate, frame) &&
                    !Overlaps(candidate, result.island)) {
                    result.preview = candidate;
                    placed = true;
                    break;
                }
            }
            if (placed) { break; }
        }
    }
    if (!placed) {
        // Over the frame's top corner that the island does not use.
        const MonitorInfo* monitor = MonitorAt(monitors, frame);
        RECT area = monitor != nullptr ? Shrunk(platform::IntersectRects(frame, monitor->work_area), gap) : Shrunk(frame, gap);
        if (platform::IsEmptyRect(area)) { area = frame; }
        const LONG height = std::max<LONG>(1, std::min(preview_max_height, platform::RectHeight(area) - island.cy - gap));
        RECT candidate{area.right - preview_width, area.top, area.right, area.top + height};
        if (Overlaps(candidate, result.island)) {
            candidate = RECT{area.left, area.top, area.left + preview_width, area.top + height};
        }
        result.preview = candidate;
        result.preview_over_frame = true;
    }
    return result;
}

}  // namespace easnap::ui
