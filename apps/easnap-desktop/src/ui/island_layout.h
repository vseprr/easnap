// Easnap - where the floating islands go around the capture frame.
//
// Pure geometry over virtual-screen pixels. The screenshot tools, the recording
// configuration and the mode island follow one rule here, so that an island
// stays outside the frame while anything outside has room and never covers the
// mode island.
#pragma once

#include "platform/platform_utils.h"

namespace easnap::ui {

enum class IslandSide {
    Below,         ///< Under the frame, centred on it.
    Above,         ///< Over the frame, centred on it.
    Right,         ///< Beside the frame, level with its bottom edge.
    Left,
    InsideBottom,  ///< Nothing outside has room: over the bottom of the frame.
    Centred,       ///< The frame is the whole monitor: the middle of the work area.
};

struct IslandPlacement {
    RECT rect{};
    IslandSide side{IslandSide::Below};
};

/// Places an island of `size` for `frame` inside `work_area`: below, above,
/// right of, left of the frame, and only when none of those has room, over
/// the bottom of the frame. `gap` separates the island from the frame and from
/// `keep_clear`. The island never intersects `keep_clear` (empty: nothing to
/// avoid) unless the work area is too small to hold both; a place outside the
/// frame that `keep_clear` takes is moved along the frame's edge before the
/// next side is tried.
[[nodiscard]] IslandPlacement PlaceIslandForFrame(const RECT& frame, const RECT& monitor_bounds,
                                                  const RECT& work_area, SIZE size, LONG gap,
                                                  const RECT& keep_clear) noexcept;

/// The mode island lives on the top edge of its monitor, at `home`. When the
/// frame takes that place, the island moves along the edge to the nearer side
/// of the frame that has room between `left_limit` and `right_limit`. When
/// neither side has room it stays at `home`, over the frame.
[[nodiscard]] RECT PlaceModeIsland(const RECT& home, const RECT& frame, LONG left_limit, LONG right_limit,
                                   LONG gap) noexcept;

}  // namespace easnap::ui
