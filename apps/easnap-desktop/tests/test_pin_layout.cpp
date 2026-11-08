// Easnap unit tests - where a pinned image goes and how it zooms (ADR-038).
//
// Pure geometry: the production functions are called with rectangles of a
// 1920 x 1080 monitor at 125 % and of smaller and offset work areas. No window
// is created.

#include "ui/pin_layout.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace easnap::ui;

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

constexpr float kScale = 1.25f;
constexpr RECT kWork{0, 0, 1920, 1032};

bool Same(const RECT& a, const RECT& b) { return ::EqualRect(&a, &b) != FALSE; }
bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }
LONG Width(const RECT& r) { return r.right - r.left; }
LONG Height(const RECT& r) { return r.bottom - r.top; }
bool Within(const RECT& inner, const RECT& outer) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

void Border() {
    CHECK(PinBorderPixels(1.0f) == 1);
    CHECK(PinBorderPixels(1.25f) == 1);
    CHECK(PinBorderPixels(2.0f) == 2);
    CHECK(PinBorderPixels(0.5f) == 1);  // Never thinner than one pixel.
}

void ScreenshotCoversItsSource() {
    const RECT source{300, 200, 940, 680};
    const auto placement = InitialPinPlacement({640, 480}, source, kWork, kScale);
    CHECK(Near(placement.zoom, 1.0));
    CHECK(Same(placement.window, RECT{299, 199, 941, 681}));
    // A whole monitor, taskbar included: still exactly over its source,
    // although it is taller than the work area.
    const auto monitor = InitialPinPlacement({1920, 1080}, {0, 0, 1920, 1080}, kWork, kScale);
    CHECK(Near(monitor.zoom, 1.0));
    CHECK(Same(monitor.window, RECT{-1, -1, 1921, 1081}));
    // At 200 % the border is two pixels and the image still covers its source.
    const auto doubled = InitialPinPlacement({100, 50}, {10, 20, 110, 70}, kWork, 2.0f);
    CHECK(Same(doubled.window, RECT{8, 18, 112, 72}));
}

void OtherImagesFitTheWorkArea() {
    // A stitched image, much taller than the screen, from a frame on the screen.
    const auto tall = InitialPinPlacement({800, 9000}, {560, 100, 1360, 900}, kWork, kScale);
    CHECK(tall.zoom < 1.0);
    CHECK(Within(tall.window, kWork));
    CHECK(Height(tall.window) <= Height(kWork));
    CHECK(std::abs((tall.window.left + tall.window.right) / 2 - 960) <= 1);  // Centred.
    CHECK(Near(tall.zoom, 1030.0 / 9000.0));
    // An image smaller than the work area but not the size of its source is
    // not enlarged.
    const auto small = InitialPinPlacement({200, 100}, {0, 0, 50, 50}, kWork, kScale);
    CHECK(Near(small.zoom, 1.0));
    CHECK(Width(small.window) == 202 && Height(small.window) == 102);
    // An offset work area on a second monitor.
    const RECT left_work{-1280, 40, 0, 1024};
    const auto offset = InitialPinPlacement({3000, 500}, {-1200, 100, -200, 300}, left_work, 1.0f);
    CHECK(Within(offset.window, left_work));
    CHECK(Width(offset.window) <= 1280);
    // Nothing to show.
    CHECK(Near(InitialPinPlacement({0, 10}, {0, 0, 0, 10}, kWork, kScale).zoom, 1.0));
    CHECK(Width(InitialPinPlacement({0, 10}, {0, 0, 0, 10}, kWork, kScale).window) == 0);
}

void ZoomSteps() {
    const SIZE image{800, 600};
    CHECK(Near(NextPinZoom(1.0, +1, image, kScale), 1.10));
    CHECK(Near(NextPinZoom(1.0, -1, image, kScale), 0.90));
    CHECK(Near(NextPinZoom(1.0, 0, image, kScale), 1.0));
    // From a zoom between steps (after a DPI change) to the neighbouring steps.
    CHECK(Near(NextPinZoom(0.8, +1, image, kScale), 0.90));
    CHECK(Near(NextPinZoom(0.8, -1, image, kScale), 0.75));
    // Up to the largest step and no further.
    double zoom = 1.0;
    for (int i = 0; i < 40; ++i) { zoom = NextPinZoom(zoom, +1, {100, 100}, kScale); }
    CHECK(Near(zoom, kPinMaximumZoom));
    // Down to the smallest step and no further.
    zoom = 1.0;
    for (int i = 0; i < 40; ++i) { zoom = NextPinZoom(zoom, -1, {4000, 3000}, kScale); }
    CHECK(Near(zoom, kPinMinimumZoom));
    // Every step down and back up returns to 100 %.
    zoom = 1.0;
    for (int i = 0; i < 5; ++i) { zoom = NextPinZoom(zoom, -1, image, kScale); }
    for (int i = 0; i < 5; ++i) { zoom = NextPinZoom(zoom, +1, image, kScale); }
    CHECK(Near(zoom, 1.0));
}

void ZoomLimits() {
    // The shorter side stops at 24 DIPs (30 pixels at 125 %).
    const SIZE thin{400, 60};
    CHECK(Near(NextPinZoom(1.0, -1, thin, kScale), 0.90));  // 54 pixels
    CHECK(Near(NextPinZoom(0.67, -1, thin, kScale), 0.50));  // 30 pixels: allowed
    CHECK(Near(NextPinZoom(0.50, -1, thin, kScale), 0.50));  // 19.8 pixels: refused
    // A tiny image can still be enlarged, but not reduced.
    CHECK(Near(NextPinZoom(1.0, -1, {20, 20}, kScale), 1.0));
    CHECK(Near(NextPinZoom(1.0, +1, {20, 20}, kScale), 1.10));
    // The longer side stops at 16384 pixels.
    const SIZE wide{7000, 300};
    CHECK(Near(NextPinZoom(2.0, +1, wide, kScale), 2.0));  // 2.5 x 7000 = 17500
    CHECK(Near(NextPinZoom(1.0, +1, wide, kScale), 1.10));
    // A pin that started below the smallest step (a fitted tall image) can
    // grow but not shrink further.
    CHECK(Near(NextPinZoom(0.05, -1, {800, 20000}, kScale), 0.05));
    CHECK(Near(NextPinZoom(0.05, +1, {800, 20000}, kScale), 0.10));
}

void ZoomKeepsThePointUnderThePointer() {
    const SIZE image{400, 300};
    const RECT window{99, 99, 501, 401};  // Image at 100, 100 - 500, 400.
    const POINT anchor{200, 250};          // Image pixel 100, 150.
    const RECT zoomed = ZoomPinAround(window, 1.0, 2.0, image, anchor, kScale);
    CHECK(Width(zoomed) == 802 && Height(zoomed) == 602);
    // Image pixel 100, 150 at zoom 2 is 200, 300 from the image's corner.
    CHECK(zoomed.left + 1 + 200 == anchor.x);
    CHECK(zoomed.top + 1 + 300 == anchor.y);
    // Back to 100 % returns to the same window.
    CHECK(Same(ZoomPinAround(zoomed, 2.0, 1.0, image, anchor, kScale), window));
    // Around the centre, as Ctrl+1 does.
    const RECT reduced = ZoomPinAround(window, 1.0, 0.5, image, {300, 250}, kScale);
    CHECK(Width(reduced) == 202 && Height(reduced) == 152);
    CHECK((reduced.left + reduced.right) / 2 == 300);
    CHECK((reduced.top + reduced.bottom) / 2 == 250);
    // Nonsense zooms leave the window alone.
    CHECK(Same(ZoomPinAround(window, 0.0, 2.0, image, anchor, kScale), window));
}

void ZoomForWindow() {
    CHECK(Near(PinZoomForWindow({0, 0, 402, 302}, {400, 300}, kScale), 1.0));
    CHECK(Near(PinZoomForWindow({0, 0, 202, 152}, {400, 300}, kScale), 0.5));
    // After a move from 125 % to 100 % Windows suggests 80 % of the width.
    CHECK(Near(PinZoomForWindow({0, 0, 322, 242}, {400, 300}, 1.0f), 0.8));
    CHECK(Near(PinZoomForWindow({0, 0, 10, 10}, {0, 0}, kScale), 1.0));
}

void StaysOnScreen() {
    const std::vector<RECT> areas{kWork, {1920, 0, 3840, 1040}};
    const RECT on_first{100, 100, 500, 400};
    CHECK(Same(KeepPinOnScreen(on_first, areas, kWork, kScale), on_first));
    // Across both monitors.
    const RECT across{1800, 100, 2100, 400};
    CHECK(Same(KeepPinOnScreen(across, areas, kWork, kScale), across));
    // 60 pixels left on the first monitor at 125 % (48 DIPs) are enough.
    const RECT edge{-340, 100, 60, 400};
    CHECK(Same(KeepPinOnScreen(edge, areas, kWork, kScale), edge));
    // 50 pixels are not: the pin moves fully into the nearest work area.
    const RECT barely{-350, 100, 50, 400};
    const RECT moved = KeepPinOnScreen(barely, areas, kWork, kScale);
    CHECK(Same(moved, RECT{0, 100, 400, 400}));
    // The second monitor went away: the pin comes back to the first one.
    const RECT gone{2500, 200, 2900, 500};
    const RECT back = KeepPinOnScreen(gone, {kWork}, kWork, kScale);
    CHECK(Same(back, RECT{1520, 200, 1920, 500}));
    // A pin larger than the work area is aligned with its top-left corner.
    const RECT huge{5000, 5000, 7000, 6200};
    CHECK(Same(KeepPinOnScreen(huge, {kWork}, kWork, kScale), RECT{0, 0, 2000, 1200}));
    // A small pin counts as visible when all of it is on a work area.
    const RECT tiny{10, 10, 30, 30};
    CHECK(Same(KeepPinOnScreen(tiny, {kWork}, kWork, kScale), tiny));
}

}  // namespace

int main() {
    Border();
    ScreenshotCoversItsSource();
    OtherImagesFitTheWorkArea();
    ZoomSteps();
    ZoomLimits();
    ZoomKeepsThePointUnderThePointer();
    ZoomForWindow();
    StaysOnScreen();

    std::printf("pin layout: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
