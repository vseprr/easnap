// Easnap unit tests - where the islands go around the capture frame.
//
// Pure geometry: the production functions are called with rectangles of a
// 1920 x 1080 monitor at 125 % and of smaller and offset work areas. No window
// is created.

#include "ui/island_layout.h"

#include <cstdio>
#include <initializer_list>

namespace {

using easnap::ui::IslandPlacement;
using easnap::ui::IslandSide;
using easnap::ui::PlaceIslandForFrame;
using easnap::ui::PlaceModeIsland;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL %d: %s\n", line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

bool Overlaps(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
bool Within(const RECT& inner, const RECT& outer) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}
bool Same(const RECT& a, const RECT& b) { return ::EqualRect(&a, &b) != FALSE; }

// One 1920 x 1080 monitor at 125 %, taskbar at the bottom.
constexpr RECT kBounds{0, 0, 1920, 1080};
constexpr RECT kWork{0, 0, 1920, 1040};
constexpr LONG kGap = 10;
constexpr SIZE kTools{975, 60};          // The screenshot tools island.
constexpr SIZE kToolsAndContext{975, 130};
constexpr RECT kModeHome{581, 0, 1338, 65};  // Top centre.
constexpr RECT kNothing{};

void PreferenceOrder() {
    // Room below: under the frame, centred on it.
    IslandPlacement placed = PlaceIslandForFrame({500, 300, 1400, 700}, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Below);
    CHECK(Same(placed.rect, RECT{463, 710, 1438, 770}));

    // No room below: above.
    placed = PlaceIslandForFrame({500, 500, 1400, 1030}, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Above);
    CHECK(Same(placed.rect, RECT{463, 430, 1438, 490}));

    // A frame at the edge of the monitor keeps the island on the monitor.
    placed = PlaceIslandForFrame({0, 300, 300, 600}, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Below && placed.rect.left == 0 && placed.rect.top == 610);

    // Full height, narrow, on the left: beside it on the right, level with its bottom.
    placed = PlaceIslandForFrame({0, 0, 400, 1040}, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Right);
    CHECK(Same(placed.rect, RECT{410, 980, 1385, 1040}));

    // Full height, narrow, on the right: beside it on the left.
    placed = PlaceIslandForFrame({1520, 0, 1920, 1040}, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Left);
    CHECK(Same(placed.rect, RECT{535, 980, 1510, 1040}));

    // Room on both sides: the right one is tried first.
    placed = PlaceIslandForFrame({810, 0, 1110, 1040}, kBounds, kWork, SIZE{300, 60}, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Right && placed.rect.left == 1120);

    // Full height with gaps too narrow for the island: over the bottom of the frame.
    const RECT tall{300, 0, 1620, 1040};
    placed = PlaceIslandForFrame(tall, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::InsideBottom);
    CHECK(Same(placed.rect, RECT{472, 970, 1447, 1030}));
    CHECK(!Overlaps(placed.rect, kModeHome) && Within(placed.rect, kWork));

    // The same frame with the taller group of tools and properties.
    placed = PlaceIslandForFrame(tall, kBounds, kWork, kToolsAndContext, kGap, kModeHome);
    CHECK(placed.side == IslandSide::InsideBottom && placed.rect.bottom == 1030 && placed.rect.top == 900);

    // The frame is the work area: nothing outside it is usable.
    placed = PlaceIslandForFrame(kWork, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::InsideBottom && placed.rect.bottom == 1030);

    // The frame is the monitor: the middle of the work area, as before.
    placed = PlaceIslandForFrame(kBounds, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Centred);
    CHECK(Same(placed.rect, RECT{472, 490, 1447, 550}));
    // A recording region may be one pixel short where the encoder needs even numbers.
    placed = PlaceIslandForFrame({0, 0, 1919, 1079}, kBounds, kWork, kTools, kGap, kNothing);
    CHECK(placed.side == IslandSide::Centred);
}

void ModeIslandIsNeverCovered() {
    // Above would land on the mode island and has no room beside it.
    IslandPlacement placed = PlaceIslandForFrame({600, 120, 1300, 1030}, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::InsideBottom);
    CHECK(!Overlaps(placed.rect, kModeHome));

    // Just clear of it: above is used.
    placed = PlaceIslandForFrame({600, 140, 1300, 1030}, kBounds, kWork, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Above && placed.rect.top == 70 && !Overlaps(placed.rect, kModeHome));

    // A small island moves along the top of the frame, to the nearer side of the mode island.
    placed = PlaceIslandForFrame({800, 120, 1100, 1030}, kBounds, kWork, SIZE{300, 60}, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Above);
    CHECK(Same(placed.rect, RECT{271, 50, 571, 110}));
    placed = PlaceIslandForFrame({1000, 120, 1300, 1030}, kBounds, kWork, SIZE{300, 60}, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Above && placed.rect.left == 1348);

    // Without a mode island the same place is free.
    placed = PlaceIslandForFrame({600, 120, 1300, 1030}, kBounds, kWork, kTools, kGap, kNothing);
    CHECK(placed.side == IslandSide::Above && placed.rect.top == 50);

    // A frame beside the mode island: the island above it may share the top edge.
    placed = PlaceIslandForFrame({0, 100, 500, 1030}, kBounds, kWork, SIZE{400, 60}, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Above && placed.rect.top == 30 && !Overlaps(placed.rect, kModeHome));

    // The frame is the monitor and the work area is short: the centred island moves down.
    const RECT short_work{0, 0, 1920, 200};
    placed = PlaceIslandForFrame({0, 0, 1920, 200}, {0, 0, 1920, 200}, short_work, kTools, kGap, kModeHome);
    CHECK(placed.side == IslandSide::Centred && !Overlaps(placed.rect, kModeHome) && Within(placed.rect, short_work));
}

void ModeIsland() {
    // Nothing in its place: it stays.
    CHECK(Same(PlaceModeIsland(kModeHome, kNothing, 0, 1920, kGap), kModeHome));
    CHECK(Same(PlaceModeIsland(kModeHome, {500, 300, 1400, 700}, 0, 1920, kGap), kModeHome));
    CHECK(Same(PlaceModeIsland(kModeHome, {0, 0, 500, 1040}, 0, 1920, kGap), kModeHome));  // Beside it.
    CHECK(Same(PlaceModeIsland(kModeHome, {500, 65, 1400, 700}, 0, 1920, kGap), kModeHome));  // Touching it.

    // The frame takes its place and leaves room on one side.
    CHECK(Same(PlaceModeIsland(kModeHome, {1000, 0, 1920, 1040}, 0, 1920, kGap), RECT{233, 0, 990, 65}));
    CHECK(Same(PlaceModeIsland(kModeHome, {0, 0, 900, 1040}, 0, 1920, kGap), RECT{910, 0, 1667, 65}));
    // Room on both sides: the nearer one.
    const RECT narrow_mode{860, 0, 1060, 65};
    CHECK(Same(PlaceModeIsland(narrow_mode, {900, 0, 1300, 500}, 0, 1920, kGap), RECT{690, 0, 890, 65}));
    CHECK(Same(PlaceModeIsland(narrow_mode, {600, 0, 1000, 500}, 0, 1920, kGap), RECT{1010, 0, 1210, 65}));
    // No room on either side: it stays where it is, over the frame.
    CHECK(Same(PlaceModeIsland(kModeHome, {300, 0, 1620, 1040}, 0, 1920, kGap), kModeHome));
    CHECK(Same(PlaceModeIsland(kModeHome, kBounds, 0, 1920, kGap), kModeHome));
    // Limits other than the monitor's edges are respected.
    CHECK(Same(PlaceModeIsland(kModeHome, {1000, 0, 1920, 1040}, 300, 1920, kGap), kModeHome));
}

/// Every frame of a grid, with the mode island where production puts it:
/// the two never touch, the island stays on the monitor, and it is inside
/// the frame only when nothing outside was possible.
void Sweep(const RECT& bounds, const RECT& work, SIZE island, const RECT& mode_home) {
    const LONG step_x = (bounds.right - bounds.left) / 12;
    const LONG step_y = (bounds.bottom - bounds.top) / 9;
    int inside = 0, outside = 0;
    for (LONG left = bounds.left; left < bounds.right; left += step_x) {
        for (LONG top = bounds.top; top < bounds.bottom; top += step_y) {
            for (LONG width : {step_x, 4 * step_x, 9 * step_x, 12 * step_x}) {
                for (LONG height : {step_y, 3 * step_y, 7 * step_y, 9 * step_y}) {
                    const RECT frame{left, top, left + width, top + height};
                    if (frame.right > bounds.right || frame.bottom > bounds.bottom) { continue; }
                    const RECT mode = PlaceModeIsland(mode_home, frame, work.left, work.right, kGap);
                    CHECK(mode.top == mode_home.top && mode.bottom == mode_home.bottom);
                    CHECK(mode.right - mode.left == mode_home.right - mode_home.left);
                    CHECK(mode.left >= work.left && mode.right <= work.right);
                    CHECK(Same(mode, mode_home) || !Overlaps(mode, frame));
                    if (!Overlaps(mode_home, frame)) { CHECK(Same(mode, mode_home)); }

                    const IslandPlacement placed = PlaceIslandForFrame(frame, bounds, work, island, kGap, mode);
                    CHECK(Within(placed.rect, work));
                    CHECK(placed.rect.right - placed.rect.left == island.cx && placed.rect.bottom - placed.rect.top == island.cy);
                    CHECK(!Overlaps(placed.rect, mode));
                    const bool is_outside = placed.side != IslandSide::InsideBottom && placed.side != IslandSide::Centred;
                    if (is_outside) { CHECK(!Overlaps(placed.rect, frame)); ++outside; } else { ++inside; }
                    // The order of preference, stated from the outside: room
                    // below wins; without it, free room above wins.
                    const bool room_below = frame.bottom + kGap + island.cy <= work.bottom;
                    const bool below_is_free = frame.bottom + kGap >= mode.bottom;
                    if (room_below && below_is_free && placed.side != IslandSide::Centred) {
                        CHECK(placed.side == IslandSide::Below);
                    }
                    const bool room_above = frame.top - kGap - island.cy >= work.top;
                    const bool above_is_free = frame.top - kGap - island.cy >= mode.bottom;
                    if (!room_below && room_above && above_is_free) { CHECK(placed.side == IslandSide::Above); }
                    // Inside only when neither side holds the island either.
                    if (placed.side == IslandSide::InsideBottom) {
                        CHECK(!room_below || !below_is_free);
                        const bool room_right = frame.right + kGap + island.cx <= work.right;
                        const bool room_left = frame.left - kGap - island.cx >= work.left;
                        // A side with room is refused only where the mode island is in the way.
                        if (room_right || room_left) { CHECK(frame.bottom - island.cy < mode.bottom + kGap); }
                    }
                }
            }
        }
    }
    CHECK(inside > 0 && outside > 0);
}

}  // namespace

int main() {
    PreferenceOrder();
    ModeIslandIsNeverCovered();
    ModeIsland();
    Sweep(kBounds, kWork, kTools, kModeHome);
    Sweep(kBounds, kWork, kToolsAndContext, kModeHome);
    Sweep(kBounds, kWork, SIZE{488, 370}, kModeHome);  // The recording configuration panel.
    // A second monitor to the left of the primary one, taskbar on top, 100 %.
    Sweep({-1280, 0, 0, 1024}, {-1280, 40, 0, 1024}, SIZE{780, 48}, {-943, 0, -337, 52});
    // A small display at a high scale.
    Sweep({0, 0, 1366, 768}, {0, 0, 1366, 720}, SIZE{1366, 96}, {200, 0, 1166, 104});

    std::printf("island layout: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
