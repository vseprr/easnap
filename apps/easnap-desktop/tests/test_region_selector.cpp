// Easnap unit tests — RegionSelector interaction logic
//
// Tests the pure interaction logic of RegionSelector without exercising GDI
// or creating any window. RenderGdi is not called.
//
// Build with EASNAP_BUILD_TESTS=ON.

#include "ui/region_selector.h"

#include <cassert>
#include <cstdio>

using easnap::ui::RegionSelector;
using easnap::ui::SelectionHandle;

// ---------------------------------------------------------------------------
// Minimal test harness (shared style with test_session_state.cpp)
// ---------------------------------------------------------------------------

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expression);
    }
}

}  // namespace

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static RECT MakeRect(LONG left, LONG top, LONG right, LONG bottom) {
    return RECT{left, top, right, bottom};
}

static POINT MakePt(LONG x, LONG y) {
    return POINT{x, y};
}

static constexpr RECT kBounds = {0, 0, 1920, 1080};

static bool RectEquals(const RECT& actual, const RECT& expected) {
    return ::EqualRect(&actual, &expected) != FALSE;
}

// ---------------------------------------------------------------------------
// Tests: initial state
// ---------------------------------------------------------------------------

static void TestInitialState() {
    RegionSelector sel;
    sel.Reset(kBounds);
    CHECK(!sel.has_selection());
    CHECK(!sel.enabled());
    CHECK(!sel.CanCommit());
    CHECK(!sel.interacting());
}

// ---------------------------------------------------------------------------
// Tests: CanCommit semantics
// ---------------------------------------------------------------------------

static void TestCanCommitRequiresEnabledAndSelection() {
    RegionSelector sel;
    sel.Reset(kBounds);

    // No selection, not enabled → cannot commit.
    CHECK(!sel.CanCommit());

    // Enabled, but no selection → cannot commit.
    sel.set_enabled(true);
    CHECK(!sel.CanCommit());

    // Selection set, but not enabled → cannot commit.
    sel.set_enabled(false);
    sel.SetSelection(MakeRect(100, 100, 300, 300));
    CHECK(!sel.CanCommit());

    // Both enabled and selection present → can commit.
    sel.set_enabled(true);
    CHECK(sel.CanCommit());
}

static void TestMouseUpDoesNotCommit() {
    // ADR-002: releasing the mouse only ends the drag; it does not confirm.
    // CanCommit() must remain under explicit user control (Enter or double-click).
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);

    sel.OnMouseDown(MakePt(100, 100));
    sel.OnMouseMove(MakePt(400, 300), true);
    sel.OnMouseUp(MakePt(400, 300));

    // After mouse-up the selection should exist and CanCommit should be true,
    // but the calling code must still decide whether to commit — selector
    // never fires the commit itself.
    CHECK(sel.has_selection());
    CHECK(sel.CanCommit());
    CHECK(!sel.interacting());  // drag ended
}

static void TestModeEntryCreatesCommittableMonitorDefault() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetImplicitSelection(kBounds);

    CHECK(sel.has_selection());
    CHECK(sel.has_implicit_selection());
    CHECK(sel.CanCommit());
    CHECK(sel.CanCommitFromDoubleClick(MakePt(800, 500)));
    CHECK(RectEquals(sel.selection(), kBounds));
}

static void TestClickPreservesImplicitDefault() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetImplicitSelection(kBounds);

    sel.OnMouseDown(MakePt(500, 400));
    sel.OnMouseUp(MakePt(500, 400));

    CHECK(sel.has_implicit_selection());
    CHECK(sel.CanCommit());
    CHECK(RectEquals(sel.selection(), kBounds));
    CHECK(!sel.interacting());

    sel.OnMouseDown(MakePt(500, 400));
    sel.OnMouseMove(MakePt(504, 404), true);
    sel.OnMouseUp(MakePt(504, 404));
    CHECK(sel.has_implicit_selection());
    CHECK(RectEquals(sel.selection(), kBounds));
}

static void TestDragReplacesImplicitDefault() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetImplicitSelection(kBounds);

    sel.OnMouseDown(MakePt(220, 180));
    sel.OnMouseMove(MakePt(640, 460), true);
    sel.OnMouseUp(MakePt(640, 460));

    CHECK(sel.has_selection());
    CHECK(!sel.has_implicit_selection());
    CHECK(RectEquals(sel.selection(), MakeRect(220, 180, 640, 460)));
    CHECK(sel.CanCommit());
    CHECK(!sel.interacting());
}

static void TestImplicitDefaultUsesCrosshairAndHasNoBody() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetImplicitSelection(kBounds);

    const POINT interior = MakePt(800, 500);
    CHECK(sel.HitTest(interior) == SelectionHandle::None);
    CHECK(sel.CursorForPoint(interior) == IDC_CROSS);

    const RECT before = sel.selection();
    sel.OnMouseDown(interior);
    sel.OnMouseUp(interior);
    CHECK(RectEquals(sel.selection(), before));
}

static void TestCtrlAStyleRestoreUsesCurrentMonitor() {
    constexpr RECT virtual_bounds{-1920, 0, 1920, 1080};
    constexpr RECT interaction_monitor{-1920, 0, 0, 1080};
    RegionSelector sel;
    sel.Reset(virtual_bounds);
    sel.set_enabled(true);
    sel.SetSelection(MakeRect(100, 100, 500, 400));

    sel.SetImplicitSelection(interaction_monitor);

    CHECK(sel.has_implicit_selection());
    CHECK(RectEquals(sel.selection(), interaction_monitor));
    CHECK(sel.CanCommit());
    CHECK(sel.CursorForPoint(MakePt(-1000, 500)) == IDC_CROSS);
}

// ---------------------------------------------------------------------------
// Tests: SetSelection and minimum size
// ---------------------------------------------------------------------------

static void TestSetSelectionNormalisesCoordinates() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    // Inverted rect: right < left, bottom < top.
    sel.SetSelection(MakeRect(400, 300, 100, 100));
    const RECT s = sel.selection();
    // After normalisation the left/right and top/bottom must be ordered.
    CHECK(s.left <= s.right);
    CHECK(s.top <= s.bottom);
}

static void TestTinySelectionDoesNotCommit() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);

    // A selection smaller than kMinimumSize must not produce CanCommit().
    sel.SetSelection(MakeRect(100, 100, 104, 104));  // 4 px wide
    CHECK(!sel.has_selection());
    CHECK(!sel.CanCommit());
}

static void TestMinimumSizeSelectionCommits() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);

    // Exactly at the minimum boundary.
    const LONG min = RegionSelector::kMinimumSize;
    sel.SetSelection(MakeRect(100, 100, 100 + min, 100 + min));
    CHECK(sel.has_selection());
    CHECK(sel.CanCommit());
}

// ---------------------------------------------------------------------------
// Tests: bounds clamping
// ---------------------------------------------------------------------------

static void TestSelectionClampsToBounds() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);

    // Selection that extends beyond the monitor boundary.
    sel.SetSelection(MakeRect(-100, -50, 2500, 1200));
    const RECT s = sel.selection();
    CHECK(s.left >= kBounds.left);
    CHECK(s.top >= kBounds.top);
    CHECK(s.right <= kBounds.right);
    CHECK(s.bottom <= kBounds.bottom);
    CHECK(sel.has_selection());
}

// ---------------------------------------------------------------------------
// Tests: Reset clears state
// ---------------------------------------------------------------------------

static void TestResetClearsSelection() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetSelection(MakeRect(100, 100, 400, 300));
    CHECK(sel.has_selection());

    sel.Reset(kBounds);
    CHECK(!sel.has_selection());
    CHECK(!sel.enabled());
    CHECK(!sel.CanCommit());
}

// ---------------------------------------------------------------------------
// Tests: drag creates a selection
// ---------------------------------------------------------------------------

static void TestDragCreatesSelection() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);

    sel.OnMouseDown(MakePt(200, 150));
    CHECK(sel.interacting());

    sel.OnMouseMove(MakePt(600, 450), true);
    sel.OnMouseUp(MakePt(600, 450));

    CHECK(sel.has_selection());
    const RECT s = sel.selection();
    CHECK(s.left == 200);
    CHECK(s.top == 150);
    CHECK(s.right == 600);
    CHECK(s.bottom == 450);
}

// ---------------------------------------------------------------------------
// Tests: hit-test returns Body for interior
// ---------------------------------------------------------------------------

static void TestHitTestInterior() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetSelection(MakeRect(100, 100, 500, 400));

    // Centre of the selection should hit Body.
    const POINT centre = MakePt(300, 250);
    CHECK(sel.HitTest(centre) == SelectionHandle::Body);
}

static void TestHitTestOutsideReturnsNone() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetSelection(MakeRect(100, 100, 500, 400));

    // Clearly outside the selection.
    CHECK(sel.HitTest(MakePt(50, 50)) == SelectionHandle::None);
    CHECK(sel.HitTest(MakePt(800, 800)) == SelectionHandle::None);
}

// ---------------------------------------------------------------------------
// Tests: nudge moves selection
// ---------------------------------------------------------------------------

static void TestNudgeMoves() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetSelection(MakeRect(200, 200, 400, 350));
    const RECT before = sel.selection();

    sel.OnNudge(10, 5, false);  // move right 10, down 5
    const RECT after = sel.selection();

    CHECK(after.left == before.left + 10);
    CHECK(after.top == before.top + 5);
    CHECK(after.right == before.right + 10);
    CHECK(after.bottom == before.bottom + 5);
}

static void TestCustomMoveResizeAndMinimumSizeRemainAdjustable() {
    RegionSelector sel;
    sel.Reset(kBounds);
    sel.set_enabled(true);
    sel.SetSelection(MakeRect(200, 200, 500, 400));

    sel.OnMouseDown(MakePt(350, 300));
    sel.OnMouseMove(MakePt(375, 315), true);
    sel.OnMouseUp(MakePt(375, 315));
    CHECK(RectEquals(sel.selection(), MakeRect(225, 215, 525, 415)));

    sel.OnMouseDown(MakePt(525, 415));
    sel.OnMouseMove(MakePt(625, 465), true);
    sel.OnMouseUp(MakePt(625, 465));
    CHECK(RectEquals(sel.selection(), MakeRect(225, 215, 625, 465)));

    sel.SetSelection(MakeRect(100, 100, 100 + RegionSelector::kMinimumSize,
                              100 + RegionSelector::kMinimumSize));
    sel.OnNudge(-1, -1, true);
    const RECT minimum = sel.selection();
    CHECK(minimum.right - minimum.left == RegionSelector::kMinimumSize);
    CHECK(minimum.bottom - minimum.top == RegionSelector::kMinimumSize);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    TestInitialState();
    TestCanCommitRequiresEnabledAndSelection();
    TestMouseUpDoesNotCommit();
    TestModeEntryCreatesCommittableMonitorDefault();
    TestClickPreservesImplicitDefault();
    TestDragReplacesImplicitDefault();
    TestImplicitDefaultUsesCrosshairAndHasNoBody();
    TestCtrlAStyleRestoreUsesCurrentMonitor();
    TestSetSelectionNormalisesCoordinates();
    TestTinySelectionDoesNotCommit();
    TestMinimumSizeSelectionCommits();
    TestSelectionClampsToBounds();
    TestResetClearsSelection();
    TestDragCreatesSelection();
    TestHitTestInterior();
    TestHitTestOutsideReturnsNone();
    TestNudgeMoves();
    TestCustomMoveResizeAndMinimumSizeRemainAdjustable();

    if (g_fail == 0) {
        std::printf("PASSED  %d/%d tests\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d failed\n", g_pass, g_fail);
    return 1;
}
