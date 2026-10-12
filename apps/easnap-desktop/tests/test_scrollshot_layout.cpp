// Easnap unit tests - Scrollshot surface placement and the document viewport.
//
// Both are pure geometry over rectangles: no window, monitor or device is
// used. Monitors are described by tables, so layouts this computer does not
// have (stacked, side by side, negative origins, small work areas) are covered.

#include "editor/image_viewport.h"
#include "ui/scrollshot_layout.h"

#include <cmath>
#include <cstdio>
#include <vector>

using easnap::editor::FitWidthZoom;
using easnap::editor::ImageView;
using easnap::editor::LayoutImageView;
using easnap::editor::StepZoom;
using easnap::editor::ZoomImageView;
using easnap::platform::MonitorInfo;
using easnap::ui::PlaceScrollshotSurfaces;
using easnap::ui::ScrollshotPlacement;

namespace {

int g_pass = 0;
int g_fail = 0;
const char* g_case = "";

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  [%s]  %s\n", file, line, g_case, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

[[nodiscard]] MonitorInfo Monitor(RECT bounds, int taskbar = 40) {
    MonitorInfo monitor;
    monitor.bounds = bounds;
    monitor.work_area = RECT{bounds.left, bounds.top, bounds.right, bounds.bottom - taskbar};
    monitor.dpi = 96;
    return monitor;
}

[[nodiscard]] bool Overlaps(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

[[nodiscard]] bool InsideSomeWorkArea(const RECT& rect, const std::vector<MonitorInfo>& monitors) {
    for (const MonitorInfo& monitor : monitors) {
        const RECT& area = monitor.work_area;
        if (rect.left >= area.left && rect.top >= area.top && rect.right <= area.right && rect.bottom <= area.bottom) {
            return true;
        }
    }
    return false;
}

constexpr SIZE kIsland{520, 72};
constexpr LONG kPreviewWidth = 168;
constexpr LONG kPreviewMax = 1000;
constexpr LONG kGap = 12;

ScrollshotPlacement Place(const RECT& frame, const std::vector<MonitorInfo>& monitors) {
    return PlaceScrollshotSurfaces(frame, monitors, kIsland, kPreviewWidth, kPreviewMax, kGap);
}

void TestSurfacesStayOutsideTheFrame() {
    const std::vector<MonitorInfo> one{Monitor({0, 0, 1920, 1080})};
    struct Case { const char* name; RECT frame; };
    const Case cases[] = {
        {"centre", {500, 200, 1300, 700}},
        {"bottom edge", {500, 500, 1300, 1040}},      // No room below: the island goes above.
        {"full height", {560, 0, 1300, 1080}},        // Neither below nor above: beside.
        {"left edge", {0, 100, 900, 800}},
        {"right edge", {1100, 100, 1920, 800}},
        {"small", {900, 500, 1000, 560}},
        {"wide", {0, 300, 1920, 700}},                // Nothing fits beside: below it does.
    };
    for (const Case& item : cases) {
        g_case = item.name;
        const ScrollshotPlacement placement = Place(item.frame, one);
        CHECK(!placement.island_over_frame);
        CHECK(!Overlaps(placement.island, item.frame));
        CHECK(InsideSomeWorkArea(placement.island, one));
        CHECK(placement.island.right - placement.island.left == kIsland.cx);
        CHECK(placement.island.bottom - placement.island.top == kIsland.cy);
        if (!placement.preview_over_frame) {
            CHECK(!Overlaps(placement.preview, item.frame));
            CHECK(!Overlaps(placement.preview, placement.island));
            CHECK(InsideSomeWorkArea(placement.preview, one));
            CHECK(placement.preview.right - placement.preview.left == kPreviewWidth);
            CHECK(placement.preview.bottom - placement.preview.top >= kPreviewWidth);
        }
    }

    g_case = "preferred sides";
    const RECT frame{500, 200, 1300, 700};
    const ScrollshotPlacement placement = Place(frame, one);
    CHECK(placement.island.top == frame.bottom + kGap);               // Below the frame, centred on it.
    CHECK((placement.island.left + placement.island.right) / 2 == (frame.left + frame.right) / 2);
    CHECK(placement.preview.left == frame.right + kGap && placement.preview.top == frame.top);
    CHECK(!placement.preview_over_frame);

    g_case = "preview left";
    const ScrollshotPlacement at_right = Place(RECT{1100, 100, 1900, 800}, one);
    CHECK(!at_right.preview_over_frame && at_right.preview.right == 1100 - kGap);

    // Full height with sides narrower than the island: only the island has to
    // lie over the frame, at its bottom; the preview still fits beside it.
    g_case = "full height, narrow sides";
    const RECT tall{400, 0, 1500, 1080};
    const ScrollshotPlacement over = Place(tall, one);
    CHECK(over.island_over_frame && InsideSomeWorkArea(over.island, one));
    CHECK(over.island.bottom == 1040 - kGap);
    CHECK(!over.preview_over_frame && !Overlaps(over.preview, tall) && !Overlaps(over.preview, over.island));
}

void TestFullMonitorFrame() {
    g_case = "one monitor filled";
    const std::vector<MonitorInfo> one{Monitor({0, 0, 1920, 1080})};
    const RECT full{0, 0, 1920, 1080};
    ScrollshotPlacement placement = Place(full, one);
    // There is nowhere else: both sit over the frame, inside the work area,
    // and apart from each other.
    CHECK(placement.island_over_frame && placement.preview_over_frame);
    CHECK(InsideSomeWorkArea(placement.island, one));
    CHECK(Overlaps(placement.island, full) && Overlaps(placement.preview, full));
    CHECK(!Overlaps(placement.island, placement.preview));

    g_case = "second monitor free";
    const std::vector<MonitorInfo> two{Monitor({0, 0, 1920, 1080}), Monitor({1920, 0, 3840, 1080})};
    placement = Place(full, two);
    CHECK(!placement.island_over_frame && !placement.preview_over_frame);
    CHECK(!Overlaps(placement.island, full) && !Overlaps(placement.preview, full));
    CHECK(placement.island.left >= 1920 && placement.preview.left >= 1920);
    CHECK(!Overlaps(placement.island, placement.preview));

    g_case = "monitor above, negative origin";
    const std::vector<MonitorInfo> stacked{Monitor({0, 0, 1920, 1080}), Monitor({-200, -1200, 1720, 0}, 0)};
    placement = Place(full, stacked);
    CHECK(!placement.island_over_frame && !placement.preview_over_frame);
    CHECK(placement.island.bottom <= 0 && placement.preview.bottom <= 0);
    CHECK(InsideSomeWorkArea(placement.island, stacked) && InsideSomeWorkArea(placement.preview, stacked));
}

void TestFrameAcrossTwoMonitors() {
    g_case = "spanning";
    const std::vector<MonitorInfo> two{Monitor({0, 0, 1920, 1080}), Monitor({1920, 0, 3840, 1080})};
    const RECT frame{1400, 200, 2500, 700};
    const ScrollshotPlacement placement = Place(frame, two);
    CHECK(!placement.island_over_frame && !placement.preview_over_frame);
    CHECK(!Overlaps(placement.island, frame) && !Overlaps(placement.preview, frame));
    CHECK(InsideSomeWorkArea(placement.island, two) && InsideSomeWorkArea(placement.preview, two));
}

void TestSmallWorkArea() {
    g_case = "small work area";
    // 1280x720 with a tall taskbar: the frame takes most of it.
    const std::vector<MonitorInfo> laptop{Monitor({0, 0, 1280, 720}, 48)};
    const RECT frame{100, 60, 1180, 580};
    const ScrollshotPlacement placement = Place(frame, laptop);
    CHECK(!placement.island_over_frame);  // 72 + 2 x 12 fit under the frame.
    CHECK(!Overlaps(placement.island, frame) && InsideSomeWorkArea(placement.island, laptop));
    // No side has 168 + gaps: the preview has to lie over the frame.
    CHECK(placement.preview_over_frame && !Overlaps(placement.preview, placement.island));
}

// --- Document viewport -------------------------------------------------------

[[nodiscard]] bool Near(float a, float b, float tolerance = 0.01f) { return std::fabs(a - b) <= tolerance; }

void TestFitAndLayout() {
    g_case = "viewport fit";
    const RECT area{100, 50, 1700, 950};
    CHECK(FitWidthZoom(1280, area) == 1.0f);          // Never enlarged to fit.
    CHECK(Near(FitWidthZoom(3200, area), 0.5f));
    CHECK(FitWidthZoom(0, area) == 1.0f);

    // A tall image narrower than the area: full width, area height, at the
    // preferred left edge, scrolled to the top.
    ImageView view = LayoutImageView(1280, 9000, area, 300, 1.0f, 0.0f, 0.0f);
    CHECK(view.viewport.left == 300 && view.viewport.right == 1580);
    CHECK(view.viewport.top == 50 && view.viewport.bottom == 950);
    CHECK(view.scroll_x == 0.0f && view.scroll_y == 0.0f);
    CHECK(Near(view.visible_fraction(), 900.0f / 9000.0f) && view.top_fraction() == 0.0f);

    // The preferred edge gives way when the viewport would leave the area.
    view = LayoutImageView(1280, 9000, area, 900, 1.0f, 0.0f, 0.0f);
    CHECK(view.viewport.right == 1700 && view.viewport.left == 420);

    // An image shorter than the area is centred vertically and cannot scroll.
    view = LayoutImageView(1280, 400, area, 300, 1.0f, 0.0f, 250.0f);
    CHECK(view.viewport.bottom - view.viewport.top == 400);
    CHECK(view.viewport.top == 50 + (900 - 400) / 2);
    CHECK(view.scroll_y == 0.0f && view.visible_fraction() == 1.0f);

    // Scrolling is clamped to the image.
    view = LayoutImageView(1280, 9000, area, 300, 1.0f, -50.0f, 99999.0f);
    CHECK(view.scroll_x == 0.0f && view.scroll_y == 9000.0f - 900.0f);
    CHECK(Near(view.top_fraction() + view.visible_fraction(), 1.0f));

    // Scale is clamped as well.
    CHECK(LayoutImageView(1280, 9000, area, 300, 99.0f, 0, 0).zoom == easnap::editor::kMaximumZoom);
    CHECK(LayoutImageView(1280, 9000, area, 300, 0.0f, 0, 0).zoom == easnap::editor::kMinimumZoom);
}

void TestMappingRoundTrips() {
    g_case = "viewport mapping";
    const RECT area{100, 50, 1700, 950};
    for (float zoom : {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 4.0f}) {
        for (float scroll : {0.0f, 137.0f, 4321.0f, 8000.0f}) {
            const ImageView view = LayoutImageView(1280, 9000, area, 300, zoom, 0.0f, scroll);
            // The origin is on whole desktop pixels, so an unscaled image is
            // shown without resampling.
            const D2D1_POINT_2F origin = view.origin();
            CHECK(origin.x == std::round(origin.x) && origin.y == std::round(origin.y));
            for (const POINT point : {POINT{view.viewport.left, view.viewport.top},
                                      POINT{view.viewport.left + 300, view.viewport.top + 411},
                                      POINT{view.viewport.right - 1, view.viewport.bottom - 1}}) {
                const D2D1_POINT_2F image = view.ToImage(point);
                const D2D1_POINT_2F back = view.ToDesktop(image);
                CHECK(Near(back.x, static_cast<float>(point.x), 0.05f) && Near(back.y, static_cast<float>(point.y), 0.05f));
                CHECK(image.x >= -0.5f && image.x <= 1280.5f && image.y >= -0.5f && image.y <= 9000.5f);
            }
            // The top-left desktop pixel shows the scroll position.
            const D2D1_POINT_2F corner = view.ToImage(POINT{view.viewport.left, view.viewport.top});
            CHECK(Near(corner.y, view.scroll_y, 1.0f / zoom + 0.01f));
        }
    }
}

void TestZoomKeepsTheAnchor() {
    g_case = "viewport zoom";
    const RECT area{100, 50, 1700, 950};
    ImageView view = LayoutImageView(1280, 9000, area, 300, 1.0f, 0.0f, 3000.0f);
    const POINT anchor{view.viewport.left + 400, view.viewport.top + 500};
    const D2D1_POINT_2F before = view.ToImage(anchor);
    for (float zoom : {2.0f, 0.5f, 1.5f, 0.25f, 1.0f}) {
        view = ZoomImageView(view, area, 300, zoom, anchor);
        CHECK(view.zoom == zoom);
        // The image point under the pointer stays there, as far as the
        // image's own edges allow.
        if (::PtInRect(&view.viewport, anchor) != FALSE) {
            const D2D1_POINT_2F now = view.ToImage(anchor);
            CHECK(Near(now.y, before.y, 2.0f / zoom + 1.0f));
        }
        CHECK(view.viewport.left >= area.left && view.viewport.right <= area.right);
        CHECK(view.viewport.top >= area.top && view.viewport.bottom <= area.bottom);
    }

    // The ladder moves strictly and stops at its ends.
    float zoom = 1.0f;
    int steps = 0;
    while (StepZoom(zoom, 1) > zoom && steps < 32) { zoom = StepZoom(zoom, 1); ++steps; }
    CHECK(zoom == easnap::editor::kMaximumZoom && steps > 0 && steps < 32);
    steps = 0;
    while (StepZoom(zoom, -1) < zoom && steps < 32) { zoom = StepZoom(zoom, -1); ++steps; }
    CHECK(zoom == easnap::editor::kMinimumZoom && steps > 0 && steps < 32);
    CHECK(StepZoom(0.9f, 1) == 1.0f && StepZoom(1.1f, -1) == 1.0f);  // Off-ladder values snap to it.
}

}  // namespace

int main() {
    TestSurfacesStayOutsideTheFrame();
    TestFullMonitorFrame();
    TestFrameAcrossTwoMonitors();
    TestSmallWorkArea();
    TestFitAndLayout();
    TestMappingRoundTrips();
    TestZoomKeepsTheAnchor();
    std::printf("scrollshot layout: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
