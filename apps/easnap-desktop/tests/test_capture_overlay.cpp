#include "native_test_monitor.h"
// Production overlay policy and native message routing. No capture backend or
// clipboard mutation: the native pass uses a deterministic frozen image.
#include "strings/strings.h"
#include "ui/native_controls.h"
#include "ui/overlay.h"
#include "ui/ui_theme.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <imm.h>
#include <string_view>

namespace {
using namespace easnap;
using app::CaptureMode;
using ui::ColorRepresentation;
int passed = 0;
int failed = 0;

void Check(bool value, const char* expression, int line) {
    if (value) {
        ++passed;
    } else {
        ++failed;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(...) Check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

bool Equal(const RECT& a, const RECT& b) { return ::EqualRect(&a, &b) != FALSE; }

void TestColorReadout() {
    CHECK(ui::FormatCaptureColor(RGB(0, 1, 255), ColorRepresentation::Rgb) == L"RGB(0, 1, 255)");
    CHECK(ui::FormatCaptureColor(RGB(0, 1, 255), ColorRepresentation::Hex) == L"#0001FF");
    CHECK(ui::FormatCaptureColor(RGB(255, 0, 255), ColorRepresentation::Hex) == L"#FF00FF");
    CHECK(ui::FormatCaptureColor(RGB(255, 255, 255), ColorRepresentation::Rgb) == L"RGB(255, 255, 255)");

    ui::CaptureColorReadout readout;
    CHECK(readout.representation() == ColorRepresentation::Rgb);
    CHECK(!readout.OnKeyUp(VK_SHIFT)); // Hotkey release was not a plain Shift press.
    readout.OnKeyDown(VK_SHIFT, false, false, false);
    for (int repeat = 0; repeat < 50; ++repeat) {
        readout.OnKeyDown(VK_SHIFT, true, false, false);
        CHECK(readout.representation() == ColorRepresentation::Rgb);
    }
    CHECK(readout.OnKeyUp(VK_SHIFT));
    CHECK(readout.representation() == ColorRepresentation::Hex);
    CHECK(!readout.OnKeyUp(VK_SHIFT));
    readout.OnKeyDown(VK_SHIFT, false, false, false);
    CHECK(readout.OnKeyUp(VK_SHIFT));
    CHECK(readout.representation() == ColorRepresentation::Rgb);

    for (WPARAM key : {WPARAM(VK_LEFT), WPARAM(VK_RIGHT), WPARAM(VK_UP), WPARAM(VK_DOWN), WPARAM('C')}) {
        readout.OnKeyDown(VK_SHIFT, false, false, false);
        readout.OnKeyDown(key, false, false, false);
        readout.OnKeyDown(VK_SHIFT, true, false, false);
        CHECK(!readout.OnKeyUp(key));
        CHECK(!readout.OnKeyUp(VK_SHIFT));
        CHECK(readout.representation() == ColorRepresentation::Rgb);
    }
    readout.OnKeyDown(VK_SHIFT, false, true, false);
    CHECK(!readout.OnKeyUp(VK_SHIFT));
    readout.OnKeyDown(VK_SHIFT, false, false, true);
    CHECK(!readout.OnKeyUp(VK_SHIFT));
    readout.OnKeyDown(VK_SHIFT, false, false, false);
    readout.CancelPress();
    CHECK(!readout.OnKeyUp(VK_SHIFT));
}

void TestPixelMapping() {
    const std::array<uint32_t, 6> pixels{
        0x00010203, 0xffff00ff, 0xff00ff00, 0xffabcdef, 0xff000000, 0xffffffff};
    // Negative X/Y output, physical pixels regardless of its DPI.
    const RECT left{-1920, -1080, -1917, -1078};
    CHECK(ui::SampleDesktopPixel(pixels, 3, 2, left, {-1920, -1080}) == RGB(1, 2, 3));
    CHECK(ui::SampleDesktopPixel(pixels, 3, 2, left, {-1919, -1080}) == RGB(255, 0, 255));
    CHECK(ui::SampleDesktopPixel(pixels, 3, 2, left, {-1920, -1079}) == RGB(171, 205, 239));
    CHECK(ui::SampleDesktopPixel(pixels, 3, 2, left, {-1918, -1079}) == RGB(255, 255, 255));
    CHECK(!ui::SampleDesktopPixel(pixels, 3, 2, left, {-1917, -1079}));
    CHECK(!ui::SampleDesktopPixel(pixels, 3, 2, left, {-1918, -1078}));
    CHECK(!ui::SampleDesktopPixel(pixels, 3, 2, left, {-1921, -1080}));
    const RECT right{0, 0, 3, 2};
    CHECK(ui::SampleDesktopPixel(pixels, 3, 2, right, {0, 1}) == RGB(171, 205, 239));
    CHECK(!ui::SampleDesktopPixel(pixels, 3, 2, right, {-1, 1})); // Virtual desktop gap.
    CHECK(!ui::SampleDesktopPixel({}, 3, 2, right, {0, 0}));
    CHECK(!ui::SampleDesktopPixel(pixels, 0, 2, right, {0, 0}));
    CHECK(!ui::SampleDesktopPixel(std::span(pixels).first(2), 3, 2, right, {2, 1}));
    // A smaller fallback image is never stretched to invent edge samples.
    CHECK(!ui::SampleDesktopPixel(pixels, 3, 2, RECT{0, 0, 4, 3}, {3, 2}));
}

void TestPlacement() {
    for (float scale : {1.0f, 1.25f, 2.0f}) {
        const SIZE size{ui::theme::Scaled(ui::theme::kMagnifierWidth, scale),
            ui::theme::kMagnifierGridPixels * ui::theme::Scaled(ui::theme::kMagnifierCell, scale) +
            3 * ui::theme::Scaled(ui::theme::kMagnifierPadding, scale) +
            3 * ui::theme::Scaled(ui::theme::kMagnifierLineHeight, scale)};
        const LONG gap = ui::theme::Scaled(ui::theme::kMagnifierPointerGap, scale);
        for (const RECT monitor : {RECT{0, 0, 1920, 1080}, RECT{-1920, -1080, 0, 0}}) {
            const RECT toolbar{monitor.left + 600, monitor.top + 60,
                               monitor.left + 1300, monitor.top + 180};
            for (LONG x = monitor.left; x < monitor.right; x += 37) {
                for (LONG y = monitor.top; y < monitor.bottom; y += 31) {
                    const RECT box = ui::PlaceCaptureMagnifier({x, y}, monitor, size, gap, toolbar);
                    RECT overlap{};
                    CHECK(box.left >= monitor.left && box.right <= monitor.right &&
                          box.top >= monitor.top && box.bottom <= monitor.bottom);
                    CHECK(!::IntersectRect(&overlap, &box, &toolbar));
                    CHECK(box.right - box.left == size.cx && box.bottom - box.top == size.cy);
                }
            }
            for (POINT corner : {POINT{monitor.left, monitor.top}, POINT{monitor.right - 1, monitor.top},
                    POINT{monitor.left, monitor.bottom - 1}, POINT{monitor.right - 1, monitor.bottom - 1}}) {
                const RECT box = ui::PlaceCaptureMagnifier(corner, monitor, size, gap, toolbar);
                CHECK(!::PtInRect(&box, corner));
            }
        }
    }
    const RECT tiny{0, 0, 100, 100};
    const RECT box = ui::PlaceCaptureMagnifier({99, 99}, tiny, {190, 240}, 22, {});
    CHECK(Equal(box, tiny));
}

void TestSelectionDamage() {
    // Compare clipped rendering against a fresh full rendering using production
    // layout and GDI drawing, including label clamping and off-monitor frames.
    for (float scale : {1.0f, 1.25f, 2.0f}) {
        for (const RECT monitor : {RECT{0, 0, 640, 360}, RECT{-640, -360, 0, 0}}) {
            ui::RegionSelector selector;
            selector.Reset({-2000, -2000, 2000, 2000}); selector.set_enabled(true);
            const HFONT font = ui::theme::CreateUiFont(13.0f, scale, false);
            platform::BgraImage partial, full;
            partial.Reset(640, 360); full.Reset(640, 360);
            const RECT client{0, 0, 640, 360};
            auto render = [&](platform::BgraImage& image, HRGN damage) {
                platform::GdiImageSurface surface(image);
                CHECK(surface.valid());
                ::SelectClipRgn(surface.dc(), damage);
                ui::theme::FillSolid(surface.dc(), client, ui::theme::kChromeColorKey);
                selector.RenderGdi(surface.dc(), monitor, scale, font);
            };
            RECT previous = monitor;
            selector.SetSelection(previous); render(partial, nullptr);
            for (int i = 0; i < 96; ++i) {
                const LONG x = monitor.left - 80 + (i * 71) % 720;
                const LONG y = monitor.top - 60 + (i * 37) % 420;
                const LONG w = 8 + (i * 103) % 680, h = 8 + (i * 53) % 390;
                const RECT current = i % 16 == 0 ? monitor : RECT{x, y, x + w, y + h};
                selector.SetSelection(current);
                HRGN damage = ::CreateRectRgn(0, 0, 0, 0);
                for (const RECT& selection : {previous, current}) {
                    for (const RECT& rect : ui::RegionSelector::GdiSelectionDamage(selection, monitor, scale)) {
                        HRGN part = ::CreateRectRgnIndirect(&rect);
                        ::CombineRgn(damage, damage, part, RGN_OR); ::DeleteObject(part);
                    }
                }
                render(partial, damage); render(full, nullptr);
                CHECK(partial.pixels == full.pixels);
                ::DeleteObject(damage);
                previous = current;
            }
            if (font) { ::DeleteObject(font); }
        }
    }
}

struct NativeWindows {
    RECT monitor{};
    HWND dim{};
    HWND magnifier{};
    HWND chrome{};
    HWND image{};
    HWND modes{};
};

BOOL CALLBACK FindWindows(HWND hwnd, LPARAM param) {
    auto& windows = *reinterpret_cast<NativeWindows*>(param);
    wchar_t name[80]{};
    ::GetClassNameW(hwnd, name, 80);
    RECT bounds{};
    ::GetWindowRect(hwnd, &bounds);
    if (std::wstring_view(name) == L"EasnapOverlayDim" && Equal(bounds, windows.monitor)) {
        windows.dim = hwnd;
    }
    if (std::wstring_view(name) == L"EasnapCaptureMagnifier" && ::IsWindowVisible(hwnd)) {
        windows.magnifier = hwnd;
    }
    if (std::wstring_view(name) == L"EasnapOverlayChrome" && Equal(bounds, windows.monitor)) {
        windows.chrome = hwnd;
    }
    if (std::wstring_view(name) == L"EasnapCaptureModes") {
        const POINT center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        if (::PtInRect(&windows.monitor, center)) { windows.modes = hwnd; }
    }
    if (std::wstring_view(name) == L"EasnapCaptureImage") {
        const POINT center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        if (::PtInRect(&windows.monitor, center)) { windows.image = hwnd; }
    }
    return TRUE;
}

LPARAM ClientPoint(LONG x, LONG y) { return MAKELPARAM(static_cast<SHORT>(x), static_cast<SHORT>(y)); }

void TestFirstSelectionLatency() {
    using Clock = std::chrono::steady_clock;
    const auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) { CHECK(false); return; }
    const RECT bounds = test::SelectedTestMonitor() ? test::SelectedTestMonitor()->bounds:monitors.front().bounds;
    const LONG width = platform::RectWidth(bounds), height = platform::RectHeight(bounds);
    const POINT origin{bounds.left + width / 4, bounds.top + height / 3};
    capture::DesktopSnapshot snapshot; snapshot.virtual_bounds = platform::VirtualScreenBounds();
    ui::OverlayController overlay({});
    const auto started = Clock::now();
    CHECK(overlay.Show(snapshot, origin));
    const auto visible = Clock::now();
    NativeWindows windows{bounds};
    ::EnumThreadWindows(::GetCurrentThreadId(), FindWindows, reinterpret_cast<LPARAM>(&windows));
    CHECK(windows.dim != nullptr);
    const auto pump = [] {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message); ::DispatchMessageW(&message);
        }
        ::GdiFlush();
    };
    // Include the first presentation in the measured native input path. This
    // exercises USER32's first visible shade region, not just selector math.
    ::SendMessageW(windows.dim, WM_LBUTTONDOWN, 0, ClientPoint(width / 4, height / 3));
    std::array<double, 64> milliseconds{};
    for (size_t step = 0; step < milliseconds.size(); ++step) {
        const LONG x = width / 2 + static_cast<LONG>(step), y = height * 2 / 3;
        const auto before = Clock::now();
        ::SendMessageW(windows.dim, WM_MOUSEMOVE, MK_LBUTTON, ClientPoint(x, y));
        pump();
        milliseconds[step] = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
        CHECK(Equal(overlay.selector().selection(),
            RECT{origin.x, origin.y, bounds.left + x, bounds.top + y}));
    }
    const double first = milliseconds.front();
    std::sort(milliseconds.begin(), milliseconds.end());
    std::printf("Native selection timing: show %.2f ms, first drag %.2f ms, p95 %.2f ms, max %.2f ms\n",
        std::chrono::duration<double, std::milli>(visible - started).count(),
        first, milliseconds[60], milliseconds.back());
    ::SendMessageW(windows.dim, WM_LBUTTONUP, 0, ClientPoint(width / 2 + 63, height * 2 / 3));
    overlay.Destroy();
}

void TestModeFade() {
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    if (monitors.empty()) { return; }
    const RECT bounds = test::SelectedTestMonitor() ? test::SelectedTestMonitor()->bounds:monitors.front().bounds;
    const POINT point{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    capture::DesktopSnapshot snapshot; snapshot.virtual_bounds = platform::VirtualScreenBounds();
    ui::OverlayController overlay({});
    CHECK(overlay.Show(snapshot, point)); overlay.EnterEditing();
    NativeWindows windows{bounds};
    ::EnumThreadWindows(::GetCurrentThreadId(), FindWindows, reinterpret_cast<LPARAM>(&windows));
    CHECK(windows.modes && ::IsWindowVisible(windows.modes));
    overlay.Hide(); overlay.FixScreenshot(); overlay.Resume();
    CHECK(!overlay.workspace().can_switch_mode() && !overlay.selector().enabled());
    overlay.SetMode(CaptureMode::Record); CHECK(overlay.mode() == CaptureMode::Screenshot);
    ::SetFocus(windows.dim); const HWND focus = ::GetFocus();
    BYTE previous = 255; bool intermediate = false;
    const ULONGLONG started = ::GetTickCount64();
    while (::IsWindowVisible(windows.modes) && ::GetTickCount64() - started < 900) {
        BYTE alpha{}; DWORD flags{}; COLORREF key{};
        CHECK(::GetLayeredWindowAttributes(windows.modes, &key, &alpha, &flags));
        CHECK(alpha <= previous && flags == (LWA_COLORKEY | LWA_ALPHA) && key == ui::theme::kChromeColorKey);
        intermediate = intermediate || (alpha > 0 && alpha < 255); previous = alpha;
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message); ::DispatchMessageW(&message);
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    CHECK(intermediate && !::IsWindowVisible(windows.modes));
    std::printf("Fade state: focus_preserved=%d chrome_visible=%d image_visible=%d dpi=%u\n",
        ::GetFocus()==focus,::IsWindowVisible(windows.chrome),::IsWindowVisible(windows.image),::GetDpiForWindow(windows.chrome));
    CHECK(::GetFocus() == focus && ::IsWindowVisible(windows.chrome) && ::IsWindowVisible(windows.image));
    overlay.Hide(); overlay.Resume();
    CHECK(!::IsWindowVisible(windows.modes));
    overlay.Destroy();
    CHECK(!::IsWindow(windows.modes));
    CHECK(overlay.Show(snapshot, point));
    windows = NativeWindows{bounds};
    ::EnumThreadWindows(::GetCurrentThreadId(), FindWindows, reinterpret_cast<LPARAM>(&windows));
    BYTE alpha{}; DWORD flags{};
    CHECK(::GetLayeredWindowAttributes(windows.modes, nullptr, &alpha, &flags) && alpha == 255);
    CHECK(::IsWindowVisible(windows.modes) && overlay.workspace().can_switch_mode());
    overlay.EnterEditing(); overlay.FixScreenshot(); overlay.Destroy();
    CHECK(!::IsWindow(windows.modes)); // Teardown during the animation.
}

void TestNativeOverlay() {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    capture::DesktopSnapshot snapshot;
    snapshot.virtual_bounds = platform::VirtualScreenBounds();
    for (const auto& monitor : monitors) {
        capture::MonitorFrame frame;
        frame.desktop_rect = monitor.bounds;
        frame.image.Reset(platform::RectWidth(monitor.bounds), platform::RectHeight(monitor.bounds));
        std::fill(frame.image.pixels.begin(), frame.image.pixels.end(), 0xffff00ff);
        snapshot.monitors.push_back(std::move(frame));
    }
    int changes = 0, completions = 0, cancels = 0;
    int editor_keys = 0, annotation_inputs = 0;
    bool annotation_tool = false;
    ui::OverlayController::Callbacks callbacks;
    callbacks.on_editing_changed = [&](CaptureMode, const RECT&) { ++changes; };
    callbacks.on_screenshot_complete = [&] { ++completions; };
    callbacks.on_cancelled = [&] { ++cancels; };
    callbacks.on_editor_key = [&](UINT, WPARAM, LPARAM) { ++editor_keys; };
    callbacks.uses_annotation_tool = [&] { return annotation_tool; };
    callbacks.on_image_pointer = [&](UINT, POINT) { ++annotation_inputs; };
    ui::OverlayController overlay(callbacks);
    for (const auto& monitor : monitors) {
        const RECT bounds = monitor.bounds;
        const LONG width = platform::RectWidth(bounds), height = platform::RectHeight(bounds);
        const POINT origin{bounds.left + width / 2, bounds.top + height / 2};
        CHECK(overlay.Show(snapshot, origin));
        CHECK(overlay.mode() == CaptureMode::Screenshot);
        CHECK(!overlay.workspace().editing() && overlay.workspace().magnifier_visible());
    CHECK(overlay.workspace().accent() == ui::theme::Current().screenshot_border);
        CHECK(overlay.selector().has_implicit_selection() && Equal(overlay.selector().selection(), bounds));
        NativeWindows windows{bounds};
        ::EnumThreadWindows(::GetCurrentThreadId(), FindWindows, reinterpret_cast<LPARAM>(&windows));
        CHECK(windows.dim && windows.magnifier);
        CHECK(::IsWindowVisible(windows.chrome));
        if (!windows.dim || !windows.magnifier) { overlay.Destroy(); continue; }
        RECT magnifier_client{};
        ::GetClientRect(windows.magnifier, &magnifier_client);
        platform::BgraImage magnifier_pixels;
        magnifier_pixels.Reset(magnifier_client.right, magnifier_client.bottom);
        {
            platform::GdiImageSurface surface(magnifier_pixels);
            CHECK(surface.valid());
            ::SendMessageW(windows.magnifier, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(surface.dc()), PRF_CLIENT);
            ::GdiFlush();
        }
        const int cell = ui::theme::Scaled(ui::theme::kMagnifierCell, monitor.scale());
        const int grid = ui::theme::kMagnifierGridPixels * cell;
        const int grid_left = (magnifier_client.right - grid) / 2;
        const int grid_top = ui::theme::Scaled(ui::theme::kMagnifierPadding, monitor.scale());
        const auto painted_pixel = [&](int x, int y) {
            return magnifier_pixels.pixels[static_cast<size_t>(y) * magnifier_pixels.width + x] & 0xffffff;
        };
        CHECK(painted_pixel(grid_left, grid_top) == 0xff00ff); // No per-pixel border.
        CHECK(painted_pixel(grid_left + grid / 2, grid_top + grid / 2) == 0xff00ff); // Reticle leaves sample clear.
        auto send = [&](UINT message, LONG x, LONG y, WPARAM flags = 0) {
            ::SendMessageW(windows.dim, message, flags, ClientPoint(x, y));
        };
        const int before_changes = changes, before_completions = completions;
        // The mode buttons are as wide as their labels need, so their centres
        // come from the production windows: Screenshot, Scrollshot, Record.
        const auto mode_point = [&](int index) {
            RECT button{};
            ::GetWindowRect(::GetDlgItem(::FindWindowW(L"EasnapCaptureModes", nullptr), 3000 + index), &button);
            return POINT{(button.left + button.right) / 2 - bounds.left, (button.top + button.bottom) / 2 - bounds.top};
        };
        const POINT screenshot_button = mode_point(0), scrollshot_button = mode_point(1), record_button = mode_point(2);
        CHECK(screenshot_button.x < scrollshot_button.x && scrollshot_button.x < record_button.x);
        if (ui::CaptureExclusionSupported()) {
            // Choosing Scrollshot before a region exists changes only the pending mode.
            send(WM_LBUTTONDOWN, scrollshot_button.x, scrollshot_button.y);
            send(WM_LBUTTONUP, scrollshot_button.x, scrollshot_button.y);
            CHECK(overlay.mode() == CaptureMode::Scrollshot && !overlay.workspace().editing());
            CHECK(overlay.workspace().accent() == ui::theme::Current().screenshot_border);
            CHECK(Equal(overlay.selector().selection(), bounds) && changes == before_changes);
        }
        send(WM_LBUTTONDOWN, record_button.x, record_button.y);
        send(WM_MOUSEMOVE, width / 3, height / 3, MK_LBUTTON);
        send(WM_LBUTTONUP, width / 3, height / 3);
        CHECK(overlay.mode() == CaptureMode::Record && !overlay.workspace().editing());
        CHECK(overlay.workspace().accent() == ui::theme::Current().record_border);
        CHECK(Equal(overlay.selector().selection(), bounds) && changes == before_changes);
        RECT magnifier{};
        ::GetWindowRect(windows.magnifier, &magnifier);
        const LONG mx = magnifier.left - bounds.left + 5, my = magnifier.top - bounds.top + 5;
        send(WM_LBUTTONDOWN, mx, my);
        send(WM_LBUTTONUP, mx, my);
        CHECK(!overlay.workspace().editing());
        CHECK(Equal(overlay.selector().selection(), bounds));
        // Record's first click enters Editing; its double-click has no terminal effect.
        send(WM_LBUTTONDOWN, width / 2, height / 2);
        send(WM_LBUTTONUP, width / 2, height / 2);
        CHECK(overlay.workspace().editing() && overlay.workspace().recording_visible());
        CHECK(!overlay.selector().has_implicit_selection());
        CHECK(Equal(overlay.selector().selection(), bounds));
        CHECK(!::IsWindowVisible(windows.magnifier));
        CHECK(::IsWindowVisible(windows.chrome));
        send(WM_LBUTTONDBLCLK, width / 2, height / 2);
        send(WM_LBUTTONUP, width / 2, height / 2);
        CHECK(completions == before_completions && changes == before_changes + 1);

        overlay.SetMode(CaptureMode::Screenshot);
        CHECK(overlay.workspace().screenshot_visible());
        CHECK(Equal(overlay.selector().selection(), bounds));
        // Editing double-click completes only on second release.
        send(WM_LBUTTONDOWN, width / 2, height / 2);
        send(WM_LBUTTONUP, width / 2, height / 2);
        send(WM_LBUTTONDBLCLK, width / 2, height / 2);
        CHECK(completions == before_completions);
        send(WM_LBUTTONUP, width / 2, height / 2);
        CHECK(completions == before_completions + 1);

        CHECK(overlay.Show(snapshot, origin));
        windows = NativeWindows{bounds};
        ::EnumThreadWindows(::GetCurrentThreadId(), FindWindows, reinterpret_cast<LPARAM>(&windows));
        // Standby click/double-click pair remains eligible across Editing entry.
        send(WM_LBUTTONDOWN, width / 2, height / 2);
        send(WM_LBUTTONUP, width / 2, height / 2);
        send(WM_LBUTTONDBLCLK, width / 2, height / 2);
        send(WM_LBUTTONUP, width / 2, height / 2);
        CHECK(completions == before_completions + 2);

        CHECK(overlay.Show(snapshot, origin));
        windows = NativeWindows{bounds};
        ::EnumThreadWindows(::GetCurrentThreadId(), FindWindows, reinterpret_cast<LPARAM>(&windows));
        send(WM_LBUTTONDOWN, width / 4, height / 2);
        send(WM_MOUSEMOVE, width / 2, height * 3 / 4, MK_LBUTTON);
        send(WM_LBUTTONUP, width / 2 + 3, height * 3 / 4 + 3);
        const RECT custom{bounds.left + width / 4, bounds.top + height / 2,
            bounds.left + width / 2 + 3, bounds.top + height * 3 / 4 + 3};
        CHECK(overlay.workspace().editing() && Equal(overlay.selector().selection(), custom));
        CHECK(completions == before_completions + 2);
        for (int i = 0; i < 4; ++i) {
            const bool record = i % 2 == 0;
            send(WM_LBUTTONDOWN, record ? record_button.x : screenshot_button.x, record_button.y);
            send(WM_MOUSEMOVE, width / 3, height / 3, MK_LBUTTON);
            send(WM_LBUTTONUP, width / 3, height / 3);
            CHECK(overlay.mode() == (record ? CaptureMode::Record : CaptureMode::Screenshot));
            CHECK(Equal(overlay.selector().selection(), custom));
            CHECK(completions == before_completions + 2);
        }
        // Untouched round-trip: first non-drag release consumes the copy latch.
        send(WM_LBUTTONDOWN, width / 3, height * 5 / 8);
        CHECK(completions == before_completions + 2);
        send(WM_LBUTTONUP, width / 3, height * 5 / 8);
        CHECK(completions == before_completions + 3 && !overlay.workspace().quick_return_armed());
        // This callback fixture remains open: a subsequent real move cannot copy again.
        send(WM_LBUTTONDBLCLK, width / 3, height * 5 / 8);
        send(WM_MOUSEMOVE, width / 3 + 20, height * 5 / 8 + 20, MK_LBUTTON);
        send(WM_LBUTTONUP, width / 3 + 20, height * 5 / 8 + 20);
        RECT moved = custom; ::OffsetRect(&moved, 20, 20);
        CHECK(Equal(overlay.selector().selection(), moved));
        CHECK(completions == before_completions + 3);
        CHECK(overlay.SamplePixel(origin) == RGB(255, 0, 255));
        // Handle clicks and annotation-owned input never become content copy.
        for (UINT message : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP}) {
            send(message, moved.left - bounds.left, moved.top - bounds.top);
        }
        CHECK(Equal(overlay.selector().selection(), moved) && completions == before_completions + 3);
        annotation_tool = true;
        const int prior_annotation_inputs = annotation_inputs;
        send(WM_LBUTTONDOWN, width / 3 + 20, height * 5 / 8 + 20);
        send(WM_MOUSEMOVE, width / 3 + 50, height * 5 / 8 + 50, MK_LBUTTON);
        send(WM_LBUTTONUP, width / 3 + 50, height * 5 / 8 + 50);
        CHECK(annotation_inputs > prior_annotation_inputs);
        CHECK(Equal(overlay.selector().selection(), moved) && completions == before_completions + 3);
        annotation_tool = false;
        std::array<BYTE, 256> original_keys{}, keys{};
        ::GetKeyboardState(original_keys.data());
        keys[VK_SHIFT] = 0x80; ::SetKeyboardState(keys.data());
        ::SendMessageW(windows.dim, WM_KEYDOWN, VK_SHIFT, 0);
        ::SendMessageW(windows.dim, WM_KEYDOWN, VK_RIGHT, 0);
        ::SendMessageW(windows.dim, WM_KEYUP, VK_SHIFT, 0);
        ::OffsetRect(&moved, 10, 0);
        CHECK(Equal(overlay.selector().selection(), moved));
        CHECK(overlay.color_readout().representation() == ColorRepresentation::Rgb);
        keys.fill(0); keys[VK_CONTROL] = 0x80; ::SetKeyboardState(keys.data());
        const int prior_editor_keys = editor_keys;
        ::SendMessageW(windows.dim, WM_KEYDOWN, 'C', 0);
        CHECK(editor_keys == prior_editor_keys + 1 && completions == before_completions + 3);
        ::SendMessageW(windows.dim, WM_KEYDOWN, 'A', 0);
        CHECK(Equal(overlay.selector().selection(), bounds) && !overlay.selector().has_implicit_selection());
        // Provisional Editing has no image plane. Fixing the frame hides handles
        // and rejects both native geometry input and mode-toolbar commands.
        CHECK(!::IsWindowVisible(windows.image));
        overlay.selector().SetSelection(custom);
        overlay.FixScreenshot();
        CHECK(overlay.workspace().screenshot_fixed() && !overlay.selector().enabled());
        CHECK(::IsWindowVisible(windows.image));
        send(WM_LBUTTONDOWN, width / 3, height * 5 / 8);
        send(WM_MOUSEMOVE, width / 3 + 30, height * 5 / 8 + 30, MK_LBUTTON);
        send(WM_LBUTTONUP, width / 3 + 30, height * 5 / 8 + 30);
        send(WM_LBUTTONDOWN, custom.left - bounds.left, custom.top - bounds.top);
        send(WM_MOUSEMOVE, custom.left - bounds.left - 20, custom.top - bounds.top - 20, MK_LBUTTON);
        send(WM_LBUTTONUP, custom.left - bounds.left - 20, custom.top - bounds.top - 20);
        ::SendMessageW(windows.dim, WM_KEYDOWN, 'A', 0);
        keys.fill(0); ::SetKeyboardState(keys.data());
        ::SendMessageW(windows.dim, WM_KEYDOWN, VK_RIGHT, 0);
        send(WM_LBUTTONDOWN, record_button.x, record_button.y);
        send(WM_LBUTTONUP, record_button.x, record_button.y);
        CHECK(Equal(overlay.selector().selection(), custom));
        CHECK(overlay.mode() == CaptureMode::Screenshot && completions == before_completions + 3);
        send(WM_MOUSEMOVE, width / 3, height * 5 / 8);
        // Transparent sibling windows defer painting; clear existing damage so
        // this assertion measures only the next pointer event's invalidation.
        ::ValidateRect(windows.dim, nullptr); ::ValidateRect(windows.chrome, nullptr);
        send(WM_MOUSEMOVE, width / 3 + 1, height * 5 / 8 + 1);
        CHECK(!::GetUpdateRect(windows.dim, nullptr, FALSE));
        CHECK(!::GetUpdateRect(windows.chrome, nullptr, FALSE));
        // Disabling geometry must not disable explicit fixed-image completion.
        send(WM_LBUTTONDOWN, width / 3, height * 5 / 8);
        send(WM_LBUTTONUP, width / 3, height * 5 / 8);
        send(WM_LBUTTONDBLCLK, width / 3, height * 5 / 8);
        send(WM_LBUTTONUP, width / 3, height * 5 / 8);
        CHECK(Equal(overlay.selector().selection(), custom) && completions == before_completions + 4);
        // A returned, fixed screenshot also completes on an annotation-tool
        // click that commits no document mutation (e.g. a zero-size rectangle).
        CHECK(overlay.Show(snapshot, origin));
        windows = NativeWindows{bounds};
        ::EnumThreadWindows(::GetCurrentThreadId(), FindWindows, reinterpret_cast<LPARAM>(&windows));
        overlay.EnterEditing();
        overlay.SetMode(CaptureMode::Record); overlay.SetMode(CaptureMode::Screenshot);
        overlay.FixScreenshot();
        annotation_tool = true;
        send(WM_LBUTTONDOWN, width / 2, height / 2);
        send(WM_LBUTTONUP, width / 2, height / 2);
        CHECK(completions == before_completions + 5 && !overlay.workspace().quick_return_armed());
        annotation_tool = false;
        ::SetKeyboardState(original_keys.data());
        ::SendMessageW(windows.dim, WM_KEYDOWN, VK_ESCAPE, 0);
        // Screenshot Editing keyboard is routed to the editor callback (tested
        // with the real editor in capture_workspace_production).
        overlay.HandleCancelled();
        CHECK(cancels > 0);
        overlay.Destroy();
        // Warmed native resources remain bounded through repeated redraws and
        // return to the same count after every session (including cached DIB/DC).
        ::GdiFlush();
        DWORD idle_gdi = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
        DWORD idle_user = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
        for (int cycle = 0; cycle < 4; ++cycle) {
            CHECK(overlay.Show(snapshot, origin)); overlay.EnterEditing();
            windows = NativeWindows{bounds};
            ::EnumThreadWindows(::GetCurrentThreadId(), FindWindows, reinterpret_cast<LPARAM>(&windows));
            auto paint = [&] {
                ::SendMessageW(windows.dim, WM_PAINT, 0, 0);
                ::SendMessageW(windows.chrome, WM_PAINT, 0, 0);
                ::GdiFlush();
            };
            paint();
            const DWORD warm_gdi = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
            for (int step = 0; step < 32; ++step) {
                RECT region = custom; ::OffsetRect(&region, step, step / 2);
                overlay.selector().SetSelection(region); overlay.RegionChanged();
                CHECK(!::GetUpdateRect(windows.dim, nullptr, FALSE));
                paint();
                CHECK(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS) == warm_gdi);
                overlay.RegionChanged(); // No new geometry/gesture state.
                CHECK(!::GetUpdateRect(windows.chrome, nullptr, FALSE));
            }
            std::vector<HWND> owned_windows;
            ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM context) -> BOOL {
                wchar_t name[80]{}; ::GetClassNameW(window, name, 80);
                if (!std::wstring_view(name).starts_with(L"Easnap")) { return TRUE; }
                auto& windows = *reinterpret_cast<std::vector<HWND>*>(context);
                windows.push_back(window);
                ::EnumChildWindows(window, [](HWND child, LPARAM children) -> BOOL {
                    reinterpret_cast<std::vector<HWND>*>(children)->push_back(child);
                    return TRUE;
                }, context);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&owned_windows));
            CHECK(!owned_windows.empty());
            overlay.Destroy();
            for (HWND window : owned_windows) { CHECK(!::IsWindow(window)); }
            ::GdiFlush();
            std::printf("Overlay resources cycle %d: GDI %lu -> %lu, USER %lu -> %lu\n", cycle,
                idle_gdi, ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS),
                idle_user, ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS));
            if (cycle == 0) {
                ::EnumWindows([](HWND window, LPARAM) -> BOOL {
                    DWORD process{}; ::GetWindowThreadProcessId(window, &process);
                    if (process == ::GetCurrentProcessId()) {
                        wchar_t name[80]{}; ::GetClassNameW(window, name, 80);
                        std::printf("Warm native window: %ls visible=%d\n", name, ::IsWindowVisible(window));
                    }
                    return TRUE;
                }, 0);
                // Warm the complete native paint lifecycle before comparing
                // sessions; Windows retains one first-use GDI resource here.
                idle_gdi = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
                idle_user = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
            } else {
                CHECK(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS) == idle_gdi);
                CHECK(::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS) == idle_user);
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    // Assertions name controls by their English text, whatever Windows speaks.
    easnap::strings::SetPreference(easnap::strings::LanguagePreference::English);
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    if (argc > 1 && std::string_view(argv[1]) == "--native") {
        // This fixture has no editable text or IME behavior. Prevent Windows'
        // asynchronously created input-indicator UI from contaminating exact
        // process resource checks; production text/editor tests retain IME.
        // ImmDisableIME must run before this thread creates its first HWND.
        CHECK(::ImmDisableIME(::GetCurrentThreadId()));
        TestFirstSelectionLatency();
        TestSelectionDamage();
        TestNativeOverlay();
        TestModeFade();
    } else {
        TestColorReadout();
        TestPixelMapping();
        TestPlacement();
    }
    std::printf("Capture overlay: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
