// Easnap - where the floating islands go around the capture frame.
#include "ui/island_layout.h"

#include <algorithm>
#include <cstdlib>

namespace easnap::ui {
namespace {

[[nodiscard]] bool Overlaps(const RECT& a, const RECT& b) noexcept {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

[[nodiscard]] RECT At(LONG left, LONG top, SIZE size) noexcept {
    return RECT{left, top, left + size.cx, top + size.cy};
}

/// A coordinate that keeps `extent` inside [low, high]; `low` when it cannot fit.
[[nodiscard]] LONG Fit(LONG value, LONG low, LONG high, LONG extent) noexcept {
    return std::clamp(value, low, std::max(low, high - extent));
}

/// Moves `rect` out of `keep_clear` along one axis without leaving `area`,
/// by the shorter of the two ways. False when neither way has room.
[[nodiscard]] bool SlideClear(RECT& rect, const RECT& keep_clear, const RECT& area, LONG gap, bool horizontal) noexcept {
    if (platform::IsEmptyRect(keep_clear) || !Overlaps(rect, keep_clear)) { return true; }
    const SIZE size{rect.right - rect.left, rect.bottom - rect.top};
    const LONG before = horizontal ? keep_clear.left - gap - size.cx : keep_clear.top - gap - size.cy;
    const LONG after = horizontal ? keep_clear.right + gap : keep_clear.bottom + gap;
    const LONG current = horizontal ? rect.left : rect.top;
    const bool before_fits = before >= (horizontal ? area.left : area.top);
    const bool after_fits = after + (horizontal ? size.cx : size.cy) <= (horizontal ? area.right : area.bottom);
    if (!before_fits && !after_fits) { return false; }
    const LONG chosen = before_fits && (!after_fits || std::abs(before - current) <= std::abs(after - current))
                            ? before : after;
    rect = horizontal ? At(chosen, rect.top, size) : At(rect.left, chosen, size);
    return true;
}

/// The frame is the monitor. A recording region may be a pixel short on the
/// right and bottom, where the encoder needs even dimensions.
[[nodiscard]] bool CoversMonitor(const RECT& frame, const RECT& bounds) noexcept {
    return frame.left == bounds.left && frame.top == bounds.top &&
           bounds.right - frame.right >= 0 && bounds.right - frame.right <= 1 &&
           bounds.bottom - frame.bottom >= 0 && bounds.bottom - frame.bottom <= 1;
}

}  // namespace

IslandPlacement PlaceIslandForFrame(const RECT& frame, const RECT& monitor_bounds, const RECT& work_area,
                                    SIZE size, LONG gap, const RECT& keep_clear) noexcept {
    const LONG centred_left =
        Fit(frame.left + (platform::RectWidth(frame) - size.cx) / 2, work_area.left, work_area.right, size.cx);

    if (CoversMonitor(frame, monitor_bounds)) {
        RECT rect = At(work_area.left + (platform::RectWidth(work_area) - size.cx) / 2,
                       work_area.top + (platform::RectHeight(work_area) - size.cy) / 2, size);
        rect = platform::ClampRectToBounds(rect, work_area);
        static_cast<void>(SlideClear(rect, keep_clear, work_area, gap, false));
        return {rect, IslandSide::Centred};
    }

    // Outside the frame, in the order of preference. A place counts only when
    // the whole island is inside the work area and clear of the frame.
    const LONG beside_top = Fit(frame.bottom - size.cy, work_area.top, work_area.bottom, size.cy);
    struct Candidate {
        IslandSide side;
        RECT rect;
        bool fits;
    };
    const Candidate outside[] = {
        {IslandSide::Below, At(centred_left, frame.bottom + gap, size),
         frame.bottom + gap + size.cy <= work_area.bottom && size.cx <= platform::RectWidth(work_area)},
        {IslandSide::Above, At(centred_left, frame.top - gap - size.cy, size),
         frame.top - gap - size.cy >= work_area.top && size.cx <= platform::RectWidth(work_area)},
        {IslandSide::Right, At(frame.right + gap, beside_top, size),
         frame.right + gap + size.cx <= work_area.right && size.cy <= platform::RectHeight(work_area)},
        {IslandSide::Left, At(frame.left - gap - size.cx, beside_top, size),
         frame.left - gap - size.cx >= work_area.left && size.cy <= platform::RectHeight(work_area)},
    };
    for (const Candidate& candidate : outside) {
        if (!candidate.fits) { continue; }
        // Along the frame's edge first, then away from it: either keeps the
        // island on its side of the frame, or the place is given up.
        const bool along_frame_edge = candidate.side == IslandSide::Below || candidate.side == IslandSide::Above;
        for (const bool horizontal : {along_frame_edge, !along_frame_edge}) {
            RECT rect = candidate.rect;
            if (SlideClear(rect, keep_clear, work_area, gap, horizontal) && !Overlaps(rect, frame)) {
                return {rect, candidate.side};
            }
        }
    }

    // Over the bottom of the frame, as far as the work area shows it.
    RECT rect = At(centred_left, std::min(frame.bottom, work_area.bottom) - gap - size.cy, size);
    rect = platform::ClampRectToBounds(rect, work_area);
    if (!SlideClear(rect, keep_clear, work_area, gap, false)) {
        static_cast<void>(SlideClear(rect, keep_clear, work_area, gap, true));
    }
    return {rect, IslandSide::InsideBottom};
}

RECT PlaceModeIsland(const RECT& home, const RECT& frame, LONG left_limit, LONG right_limit, LONG gap) noexcept {
    if (platform::IsEmptyRect(frame) || !Overlaps(home, frame)) { return home; }
    const LONG width = home.right - home.left;
    const LONG left = frame.left - gap - width;
    const LONG right = frame.right + gap;
    const bool left_fits = left >= left_limit;
    const bool right_fits = right + width <= right_limit;
    if (!left_fits && !right_fits) { return home; }
    const LONG chosen = left_fits && (!right_fits || std::abs(left - home.left) <= std::abs(right - home.left))
                            ? left : right;
    return RECT{chosen, home.top, chosen + width, home.bottom};
}

}  // namespace easnap::ui
