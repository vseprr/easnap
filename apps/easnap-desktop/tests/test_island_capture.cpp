// Easnap integration test - an island excluded from capture never appears in
// a captured picture, whichever way the screen is read.
//
// The recording panel and the Scrollshot island stay on screen while Easnap
// reads it. A recording reads the screen through Desktop Duplication or, when
// duplication has no composed frame yet, through a GDI blit of the screen DC.
// Each island is put over a window of one colour and the region under it is
// read: every pixel must be that colour, never the island and never black.
//
// What the exclusion must not change for the user: a dialog dragged by its
// header and the themed menu show only what Easnap paints. They are read as
// the user sees them, with their exclusion lifted for the read.
//
// Visible desktop: capture needs the composed input desktop.

#include "native_test_monitor.h"

#include "app/app_settings.h"
#include "app/recording_workflow.h"
#include "app/session_transitions.h"
#include "capture/capture_provider.h"
#include "editor/screenshot_editor.h"
#include "strings/strings.h"
#include "ui/recording_controls.h"
#include "ui/native_controls.h"
#include "ui/scrollshot_controls.h"
#include "ui/settings_dialog.h"
#include "ui/ui_theme.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>

// The islands run with the version 6 common controls, as in Easnap.exe
// (src/main.cpp); the tooltip below needs them.
#if defined(_MSC_VER)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

using namespace easnap;

int passed{}, failed{};
#define CHECK(...)                                                         \
    do {                                                                   \
        if (__VA_ARGS__) {                                                 \
            ++passed;                                                      \
        } else {                                                           \
            ++failed;                                                      \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); \
        }                                                                  \
    } while (false)

constexpr COLORREF kColours[]{RGB(64, 128, 192), RGB(160, 96, 48)};

void Pump(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    do {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::DwmFlush();
    } while (std::chrono::steady_clock::now() < end);
}

LRESULT CALLBACK BackdropProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(window, &paint);
        RECT client{};
        ::GetClientRect(window, &client);
        ui::theme::FillSolid(dc, client, static_cast<COLORREF>(::GetWindowLongPtrW(window, GWLP_USERDATA)));
        ::EndPaint(window, &paint);
        return 0;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

void Paint(HWND window, COLORREF colour) {
    ::SetWindowLongPtrW(window, GWLP_USERDATA, static_cast<LONG_PTR>(colour));
    ::InvalidateRect(window, nullptr, FALSE);
    ::UpdateWindow(window);
}

bool Matches(uint32_t pixel, COLORREF colour) {
    return std::abs(static_cast<int>((pixel >> 16) & 0xff) - GetRValue(colour)) <= 2 &&
           std::abs(static_cast<int>((pixel >> 8) & 0xff) - GetGValue(colour)) <= 2 &&
           std::abs(static_cast<int>(pixel & 0xff) - GetBValue(colour)) <= 2;
}

struct Foreign {
    long count{-1};
    uint32_t sample{};
    RECT bounds{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
};

/// Pixels of `image` (read at `area`) inside `window` that are not `colour`.
Foreign ForeignPixels(const platform::BgraImage& image, const RECT& area, const RECT& window, COLORREF colour) {
    Foreign foreign;
    RECT inside{};
    if (!::IntersectRect(&inside, &area, &window)) { return foreign; }
    foreign.count = 0;
    for (LONG y = inside.top; y < inside.bottom; ++y) {
        const uint32_t* row = image.row(static_cast<int>(y - area.top));
        for (LONG x = inside.left; x < inside.right; ++x) {
            const uint32_t pixel = row[x - area.left];
            if (!Matches(pixel, colour)) {
                if (foreign.count++ == 0) { foreign.sample = pixel & 0xffffffu; }
                foreign.bounds = RECT{(std::min)(foreign.bounds.left, x), (std::min)(foreign.bounds.top, y),
                                      (std::max)(foreign.bounds.right, x + 1), (std::max)(foreign.bounds.bottom, y + 1)};
            }
        }
    }
    return foreign;
}

/// The corner of the picture, where no island is, shows the backdrop in
/// `colour`: the picture is current, not one from before the last repaint.
bool Current(const platform::BgraImage& image, COLORREF colour) {
    return !image.empty() && Matches(image.row(4)[4], colour);
}

/// The screen read the way the GDI fallback reads it: a blit from the screen DC.
bool ScreenBlit(const RECT& area, platform::BgraImage& out) {
    const int width = platform::RectWidth(area), height = platform::RectHeight(area);
    out.Reset(width, height);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (memory && bitmap && bits) {
        const HGDIOBJ previous = ::SelectObject(memory, bitmap);
        ok = ::BitBlt(memory, 0, 0, width, height, screen, area.left, area.top, SRCCOPY) != FALSE;
        ::SelectObject(memory, previous);
        ::GdiFlush();
        for (int y = 0; ok && y < height; ++y) {
            std::memcpy(out.row(y), static_cast<const uint8_t*>(bits) + static_cast<size_t>(y) * width * 4,
                        static_cast<size_t>(width) * 4);
        }
    }
    if (bitmap) { ::DeleteObject(bitmap); }
    if (memory) { ::DeleteDC(memory); }
    ::ReleaseDC(nullptr, screen);
    return ok;
}

enum class Path { ScreenBlit,
                  Gdi,
                  Duplication };
const char* Name(Path path) {
    return path == Path::ScreenBlit ? "a screen blit" : path == Path::Gdi ? "the provider's GDI fallback"
                                                                          : "Desktop Duplication";
}

/// Reads `area` through `path` once the backdrop shows `colour`. False when
/// that path could not be reached on this machine; the check is then not run.
bool Capture(Path path, HWND backdrop, COLORREF colour, const RECT& area, platform::BgraImage& out) {
    const COLORREF other = colour == kColours[0] ? kColours[1] : kColours[0];
    if (path == Path::ScreenBlit) {
        Paint(backdrop, colour);
        Pump(std::chrono::milliseconds(100));
        return ScreenBlit(area, out) && Current(out, colour);
    }
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto provider = capture::CreateRegionCaptureProvider();
        capture::RegionImageOptions options;
        capture::RegionCaptureResult result;
        if (path == Path::Gdi) {
            // A duplication created on a still desktop has no composed frame:
            // its first picture is black and the GDI blit takes over. The
            // default options give the output up there; a recording reads
            // that one picture through GDI the same way and asks again later.
            Paint(backdrop, colour);
            Pump(std::chrono::milliseconds(400));
            result = provider->CaptureRegionImage(area, options, out);
            if (result.status == capture::RegionCaptureStatus::FrameAcquired &&
                result.primary_failure.stage == capture::RegionCaptureFailureStage::DuplicationBlankFrame &&
                Current(out, colour)) {
                return true;
            }
        } else {
            // Repaint until a composed frame shows the backdrop in `colour`.
            options.await_first_composed_frame = true;
            options.wait_milliseconds = 100;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                Paint(backdrop, other);
                Pump(std::chrono::milliseconds(20));
                Paint(backdrop, colour);
                Pump(std::chrono::milliseconds(20));
                result = provider->CaptureRegionImage(area, options, out);
                if (result.status == capture::RegionCaptureStatus::FrameAcquired &&
                    result.primary_failure.stage == capture::RegionCaptureFailureStage::None && Current(out, colour)) {
                    return true;
                }
            }
        }
        provider->Release();
    }
    return false;
}

struct Reached {
    bool blit{}, gdi{}, duplication{};
};

/// Reads the area under `windows` through every path. With `assert_absent`
/// each must be missing from the picture; otherwise the counts are reported.
void Examine(const char* name, std::initializer_list<HWND> windows, HWND backdrop, const RECT& area, bool assert_absent,
             Reached& reached) {
    for (const Path path : {Path::ScreenBlit, Path::Gdi, Path::Duplication}) {
        for (const COLORREF colour : kColours) {
            platform::BgraImage image;
            if (!Capture(path, backdrop, colour, area, image)) {
                std::printf("%s: %s could not be reached on this machine; not checked\n", name, Name(path));
                break;
            }
            (path == Path::ScreenBlit ? reached.blit : path == Path::Gdi ? reached.gdi
                                                                         : reached.duplication) = true;
            CHECK(image.width == platform::RectWidth(area) && image.height == platform::RectHeight(area));
            for (HWND window : windows) {
                RECT rect{};
                ::GetWindowRect(window, &rect);
                wchar_t title[64]{};
                ::GetWindowTextW(window, title, 64);
                const Foreign foreign = ForeignPixels(image, area, rect, colour);
                std::printf("%s%s [%ls] through %s over RGB(%d,%d,%d): %ld foreign pixels (first 0x%06x)\n",
                            assert_absent ? "" : "MEASURE ", name, title, Name(path), GetRValue(colour), GetGValue(colour),
                            GetBValue(colour), foreign.count, static_cast<unsigned>(foreign.sample));
                if (assert_absent) { CHECK(foreign.count == 0); }
            }
        }
    }
}

void CheckExcluded(HWND window) {
    DWORD affinity = 0;
    CHECK(::GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE);
}

/// `area` of the screen as the user sees it, `window` included: its capture
/// exclusion is lifted for the read and put back.
bool ReadAsSeen(HWND window, const RECT& area, platform::BgraImage& out) {
    DWORD affinity = WDA_NONE;
    if (!::GetWindowDisplayAffinity(window, &affinity) || !::SetWindowDisplayAffinity(window, WDA_NONE)) { return false; }
    ::DwmFlush();
    ::DwmFlush();
    const bool read = ScreenBlit(area, out);
    CHECK(::SetWindowDisplayAffinity(window, affinity) != FALSE);
    return read;
}

/// Pixels that differ visibly between two pictures of the same size.
Foreign Differences(const platform::BgraImage& expected, const platform::BgraImage& seen) {
    Foreign foreign;
    if (expected.empty() || expected.width != seen.width || expected.height != seen.height) { return foreign; }
    foreign.count = 0;
    for (int y = 0; y < seen.height; ++y) {
        const uint32_t* a = expected.row(y);
        const uint32_t* b = seen.row(y);
        for (int x = 0; x < seen.width; ++x) {
            const auto apart = [&](int shift) {
                return std::abs(static_cast<int>((a[x] >> shift) & 0xff) - static_cast<int>((b[x] >> shift) & 0xff)) > 24;
            };
            if (apart(16) || apart(8) || apart(0)) {
                if (foreign.count++ == 0) { foreign.sample = b[x] & 0xffffffu; }
                foreign.bounds = RECT{(std::min)(foreign.bounds.left, static_cast<LONG>(x)),
                                      (std::min)(foreign.bounds.top, static_cast<LONG>(y)),
                                      (std::max)(foreign.bounds.right, static_cast<LONG>(x + 1)),
                                      (std::max)(foreign.bounds.bottom, static_cast<LONG>(y + 1))};
            }
        }
    }
    return foreign;
}

/// No paint is pending for `window` or any window inside it.
bool Settled(HWND window) {
    if (::GetUpdateRect(window, nullptr, FALSE)) { return false; }
    bool settled = true;
    ::EnumChildWindows(
        window,
        [](HWND child, LPARAM reference) -> BOOL {
            if (::GetUpdateRect(child, nullptr, FALSE)) {
                *reinterpret_cast<bool*>(reference) = false;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&settled));
    return settled;
}

bool WaitFor(const std::function<bool()>& condition, ULONGLONG milliseconds) {
    const ULONGLONG end = ::GetTickCount64() + milliseconds;
    while (!condition()) {
        if (::GetTickCount64() > end) { return false; }
        ::Sleep(2);
    }
    return true;
}

/// `window` as Easnap paints it, through WM_PRINT.
bool ReadAsPainted(HWND window, platform::BgraImage& out) {
    RECT rect{};
    ::GetWindowRect(window, &rect);
    out.Reset(platform::RectWidth(rect), platform::RectHeight(rect));
    platform::GdiImageSurface surface(out);
    if (!surface.valid()) { return false; }
    ::SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(surface.dc()), PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
    ::GdiFlush();
    return true;
}

// A dialog dragged by its header, as the user moves Settings or the Exit
// question. Its Close button is read as the user sees it and compared with
// what Easnap paints for it, before and after the drag: nothing but
// Easnap's own painting may be on it.
enum DragStage { kFindDialog,
                 kHoverClose,
                 kSettleBefore,
                 kReadyToDrag,
                 kDragged,
                 kSettleAfter,
                 kDragDone,
                 kDragFailed };
struct DialogDrag {
    HWND thief{};       // activated before the drag when set
    bool hover_close{};  // the pointer rests on Close before the drag
    HWND dialog{}, header{}, close{};
    POINT press{};
    std::atomic<int> stage{kFindDialog};
    ULONGLONG since{}, deadline{};
    RECT before_rect{}, after_rect{};
    Foreign before, after;
    std::atomic<int> steps_moved{0};
};
DialogDrag* active_drag{};

/// Pixels on the Close button that Easnap did not paint there.
Foreign CloseButtonAsSeen(const DialogDrag& drag) {
    RECT rect{};
    ::GetWindowRect(drag.close, &rect);
    platform::BgraImage seen, painted;
    Foreign foreign;
    if (!ReadAsSeen(drag.dialog, rect, seen) || !ReadAsPainted(drag.close, painted)) {
        CHECK(!"the Close button could not be read");
        return foreign;
    }
    return Differences(painted, seen);
}

void CALLBACK DragTimer(HWND, UINT, UINT_PTR id, DWORD) {
    DialogDrag& drag = *active_drag;
    const ULONGLONG now = ::GetTickCount64();
    const auto finish = [&](int stage) {
        drag.stage = stage;
        ::KillTimer(nullptr, id);
        // Close whatever dialog this thread shows.
        ::EnumThreadWindows(
            ::GetCurrentThreadId(),
            [](HWND window, LPARAM) -> BOOL {
                wchar_t name[32]{};
                ::GetClassNameW(window, name, 32);
                if (::lstrcmpW(name, L"#32770") == 0) { ::PostMessageW(window, WM_CLOSE, 0, 0); }
                return TRUE;
            },
            0);
    };
    if (now > drag.deadline) {
        std::printf("dialog drag: timed out at stage %d\n", drag.stage.load());
        finish(kDragFailed);
        return;
    }
    switch (drag.stage.load()) {
        case kFindDialog: {
            ::EnumThreadWindows(
                ::GetCurrentThreadId(),
                [](HWND window, LPARAM reference) -> BOOL {
                    wchar_t name[32]{};
                    ::GetClassNameW(window, name, 32);
                    if (::lstrcmpW(name, L"#32770") == 0 && ::IsWindowVisible(window)) {
                        *reinterpret_cast<HWND*>(reference) = window;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&drag.dialog));
            if (!drag.dialog) { return; }
            drag.header = ::FindWindowExW(drag.dialog, nullptr, L"EasnapWindowChrome", nullptr);
            drag.close = drag.header ? ::GetWindow(drag.header, GW_CHILD) : nullptr;
            if (!drag.close) {
                std::printf("dialog drag: the dialog has no header with a Close button\n");
                finish(kDragFailed);
                return;
            }
            RECT header{};
            ::GetWindowRect(drag.header, &header);
            drag.press = {header.left + platform::RectWidth(header) / 3, (header.top + header.bottom) / 2};
            CHECK(::SetCursorPos(drag.press.x, drag.press.y) != FALSE);
            if (drag.thief) {
                ::SetForegroundWindow(drag.thief);
                CHECK(::GetForegroundWindow() == drag.thief);
            }
            if (drag.hover_close) {
                RECT close{};
                ::GetWindowRect(drag.close, &close);
                CHECK(::SetCursorPos((close.left + close.right) / 2, (close.top + close.bottom) / 2) != FALSE);
            }
            drag.since = now;
            drag.stage = drag.hover_close ? kHoverClose : kSettleBefore;
            return;
        }
        case kHoverClose:
            if (!Settled(drag.dialog)) {
                drag.since = now;
                return;
            }
            if (now - drag.since < 300) { return; }
            CHECK(::SetCursorPos(drag.press.x, drag.press.y) != FALSE);
            drag.since = now;
            drag.stage = kSettleBefore;
            return;
        case kSettleBefore:
        case kSettleAfter: {
            if (!Settled(drag.dialog)) {
                drag.since = now;
                return;
            }
            if (now - drag.since < 300) { return; }
            if (drag.stage.load() == kSettleBefore) {
                ::GetWindowRect(drag.dialog, &drag.before_rect);
                drag.before = CloseButtonAsSeen(drag);
                drag.stage = kReadyToDrag;
            } else {
                ::GetWindowRect(drag.dialog, &drag.after_rect);
                drag.after = CloseButtonAsSeen(drag);
                finish(kDragDone);
            }
            return;
        }
        case kDragged:
            drag.since = now;
            drag.stage = kSettleAfter;
            return;
        default: return;
    }
}

/// Presses on the header, moves the pointer 80 by 40 pixels in eight steps
/// and releases, from a thread of its own while the dialog runs its loop.
void DriveDrag(DialogDrag& drag, DWORD ui_thread) {
    const bool ready = WaitFor([&] { const int stage = drag.stage.load(); return stage == kReadyToDrag || stage == kDragFailed; }, 12000);
    if (!ready || drag.stage.load() != kReadyToDrag) { return; }
    const auto moving = [&] {
        GUITHREADINFO info{sizeof(info)};
        return ::GetGUIThreadInfo(ui_thread, &info) && (info.flags & GUI_INMOVESIZE) != 0;
    };
    // Never send input into another application.
    if (::WindowFromPoint(drag.press) != drag.header) {
        std::printf("dialog drag: the pointer is not over the dialog's header; no input sent\n");
        int expected = kReadyToDrag;
        drag.stage.compare_exchange_strong(expected, kDragged);
        return;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    CHECK(::SendInput(1, &input, sizeof(input)) == 1);
    const bool entered = WaitFor(moving, 2000);
    BOOL full_windows = TRUE;
    ::SystemParametersInfoW(SPI_GETDRAGFULLWINDOWS, 0, &full_windows, 0);
    for (int step = 1; entered && step <= 8; ++step) {
        ::SetCursorPos(drag.press.x + 10 * step, drag.press.y + 5 * step);
        const bool moved = WaitFor(
            [&] {
                RECT now{};
                ::GetWindowRect(drag.dialog, &now);
                return now.left == drag.before_rect.left + 10 * step && now.top == drag.before_rect.top + 5 * step;
            },
            full_windows ? 1000 : 100);
        if (moved) { ++drag.steps_moved; }
    }
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    CHECK(::SendInput(1, &input, sizeof(input)) == 1);
    WaitFor([&] { return !moving(); }, 2000);
    if (!entered) { std::printf("dialog drag: the move loop was not entered\n"); }
    if (!full_windows) { std::printf("dialog drag: Windows shows only an outline while dragging\n"); }
    int expected = kReadyToDrag;
    drag.stage.compare_exchange_strong(expected, kDragged);
}

// A themed menu with a submenu, as the tray menu's Appearance: the submenu
// item on screen is what Easnap draws for it, and nothing else. Windows
// draws its own arrow into an owner-drawn item that has a submenu unless
// the item is clipped after drawing.
struct MenuLook {
    HWND owner{};
    HMENU root{};
    HWND menu{};
    RECT item{};
    ULONGLONG since{}, deadline{};
    platform::BgraImage seen, drawn;
    bool done{};
};
MenuLook* active_menu{};

void CALLBACK MenuTimer(HWND, UINT, UINT_PTR id, DWORD) {
    MenuLook& look = *active_menu;
    const ULONGLONG now = ::GetTickCount64();
    if (now > look.deadline) {
        std::printf("themed menu: timed out\n");
        ::KillTimer(nullptr, id);
        ::EndMenu();
        return;
    }
    if (!look.menu) {
        ::EnumThreadWindows(
            ::GetCurrentThreadId(),
            [](HWND window, LPARAM reference) -> BOOL {
                wchar_t name[32]{};
                ::GetClassNameW(window, name, 32);
                if (::lstrcmpW(name, L"#32768") == 0 && ::IsWindowVisible(window)) {
                    *reinterpret_cast<HWND*>(reference) = window;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&look.menu));
        look.since = now;
        return;
    }
    if (!Settled(look.menu) || !::GetMenuItemRect(nullptr, look.root, 0, &look.item)) {
        look.since = now;
        return;
    }
    if (now - look.since < 200) { return; }
    ::KillTimer(nullptr, id);
    CHECK(ReadAsSeen(look.menu, look.item, look.seen));
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_DATA | MIIM_STATE;
    CHECK(::GetMenuItemInfoW(look.root, 0, TRUE, &info) != FALSE);
    look.drawn.Reset(platform::RectWidth(look.item), platform::RectHeight(look.item));
    {
        platform::GdiImageSurface surface(look.drawn);
        CHECK(surface.valid());
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_MENU;
        draw.itemAction = ODA_DRAWENTIRE;
        draw.hwndItem = reinterpret_cast<HWND>(look.root);
        draw.hDC = surface.dc();
        draw.rcItem = {0, 0, platform::RectWidth(look.item), platform::RectHeight(look.item)};
        draw.itemData = info.dwItemData;
        ::SendMessageW(look.owner, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
        ::GdiFlush();
    }
    look.done = true;
    ::EndMenu();
}

int Run() {
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    if (monitors.empty()) { return 1; }
    const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
    const RECT area{monitor.work_area.left + 80, monitor.work_area.top + 80, monitor.work_area.left + 880,
                    monitor.work_area.top + 580};

    WNDCLASSW backdrop_class{};
    backdrop_class.hInstance = ::GetModuleHandleW(nullptr);
    backdrop_class.lpszClassName = L"EasnapIslandCaptureBackdrop";
    backdrop_class.lpfnWndProc = BackdropProc;
    CHECK(::RegisterClassW(&backdrop_class));
    HWND backdrop = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, backdrop_class.lpszClassName,
                                      L"Island capture test background", WS_POPUP, area.left, area.top,
                                      platform::RectWidth(area), platform::RectHeight(area), nullptr, nullptr,
                                      backdrop_class.hInstance, nullptr);
    CHECK(backdrop != nullptr);
    if (!backdrop) { return 1; }
    Paint(backdrop, kColours[0]);
    ::ShowWindow(backdrop, SW_SHOWNOACTIVATE);
    Pump(std::chrono::milliseconds(100));

    Reached reached;
    const auto put_over_backdrop = [&](HWND island) {
        RECT rect{};
        ::GetWindowRect(island, &rect);
        const LONG left = area.left + (platform::RectWidth(area) - platform::RectWidth(rect)) / 2;
        const LONG top = area.top + (platform::RectHeight(area) - platform::RectHeight(rect)) / 2;
        ::SetWindowPos(island, HWND_TOPMOST, left, top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        Pump(std::chrono::milliseconds(100));
    };

    // How Windows treats an excluded window of each kind on each path,
    // reported for the record.
    {
        HWND plain = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, backdrop_class.lpszClassName,
                                       L"plain", WS_POPUP, area.left + 60, area.top + 60, 160, 120,
                                       nullptr, nullptr, backdrop_class.hInstance, nullptr);
        HWND layered = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                         backdrop_class.lpszClassName, L"layered", WS_POPUP,
                                         area.left + 300, area.top + 60, 160, 120, nullptr, nullptr,
                                         backdrop_class.hInstance, nullptr);
        CHECK(plain != nullptr && layered != nullptr);
        if (plain && layered) {
            CHECK(::SetLayeredWindowAttributes(layered, 0, 255, LWA_ALPHA) != FALSE);
            for (HWND window : {plain, layered}) {
                Paint(window, RGB(220, 30, 30));
                CHECK(::SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE) != FALSE);
                ::ShowWindow(window, SW_SHOWNOACTIVATE);
            }
            Pump(std::chrono::milliseconds(100));
            Examine("excluded window", {plain, layered}, backdrop, area, false, reached);
        }
        if (plain) { ::DestroyWindow(plain); }
        if (layered) { ::DestroyWindow(layered); }
        Pump(std::chrono::milliseconds(50));
    }

    // A still desktop, as when a screenshot is fixed: the backdrop is painted
    // once, then the islands are shown, shown again after being hidden (as a
    // frame drag does) and moved while visible, and nothing under them is
    // drawn again before the screen is read the way a screenshot reads it.
    // A plain and a layered excluded test window are measured beside them.
    {
        HWND plain = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, backdrop_class.lpszClassName,
                                       L"plain", WS_POPUP, area.left + 60, area.top + 60, 160, 120,
                                       nullptr, nullptr, backdrop_class.hInstance, nullptr);
        HWND layered = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                         backdrop_class.lpszClassName, L"layered", WS_POPUP,
                                         area.left + 300, area.top + 60, 160, 120, nullptr, nullptr,
                                         backdrop_class.hInstance, nullptr);
        CHECK(plain != nullptr && layered != nullptr);
        app::AppSettings settings;
        editor::ScreenshotEditor editor({});
        const RECT frame{area.left + 100, area.top + 100, area.right - 100, area.bottom - 100};
        CHECK(editor.Open(frame, &settings, nullptr));
        ui::RecordingControls panel({});
        CHECK(panel.ShowConfiguring(frame, app::RecordingConfiguration{}, false, nullptr));
        panel.SetSessionState(app::SessionState::RecordingActive);
        ui::ScrollshotControls scrollshot({});
        CHECK(scrollshot.Show(ui::ScrollshotControls::View::Capture, frame, nullptr));
        if (plain && layered && editor.hwnd() && panel.hwnd() && scrollshot.hwnd()) {
            CHECK(::SetLayeredWindowAttributes(layered, 0, 255, LWA_ALPHA) != FALSE);
            for (HWND window : {plain, layered}) {
                Paint(window, RGB(220, 30, 30));
                CHECK(::SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE) != FALSE);
            }
            struct Island {
                HWND window;
                const char* name;
                POINT place;
                bool product;
            };
            const Island islands[]{
                {plain, "plain excluded window", {area.left + 60, area.top + 40}, false},
                {layered, "layered excluded window", {area.left + 300, area.top + 40}, false},
                {editor.hwnd(), "screenshot tools island", {area.left + 20, area.top + 190}, true},
                {panel.hwnd(), "recording panel", {area.left + 20, area.top + 290}, true},
                {scrollshot.hwnd(), "Scrollshot island", {area.left + 420, area.top + 290}, true},
            };
            const char* rounds[]{"first shown", "shown again after hiding", "moved while visible"};
            std::unique_ptr<capture::IRegionCaptureProvider> provider;
            COLORREF colour = kColours[0];
            for (int round = 0; round < 3; ++round) {
                if (round < 2) {
                    for (const Island& island : islands) { ::ShowWindow(island.window, SW_HIDE); }
                    if (editor.context_hwnd()) { ::ShowWindow(editor.context_hwnd(), SW_HIDE); }
                    colour = kColours[round];
                    Paint(backdrop, colour);
                    Pump(std::chrono::milliseconds(400));
                    // The session reads the desktop once when it starts.
                    if (provider) { provider->Release(); }
                    provider = capture::CreateRegionCaptureProvider();
                    capture::DesktopSnapshot start;
                    CHECK(provider->CaptureDesktop(false, start));
                }
                for (const Island& island : islands) {
                    const LONG shift = round == 2 ? 24 : 0;
                    ::SetWindowPos(island.window, HWND_TOPMOST, island.place.x + shift, island.place.y + shift, 0, 0,
                                   SWP_NOSIZE | SWP_NOACTIVATE | (round < 2 ? SWP_SHOWWINDOW : 0));
                }
                Pump(std::chrono::milliseconds(400));
                for (const bool through_provider : {true, false}) {
                    platform::BgraImage image;
                    bool read = false;
                    if (through_provider) {
                        capture::DesktopSnapshot fixed;
                        read = provider->CaptureDesktop(false, fixed) && fixed.CropRegion(area, image);
                    } else {
                        read = ScreenBlit(area, image);
                    }
                    CHECK(read && Current(image, colour));
                    if (!read) { continue; }
                    reached.blit = true;
                    for (const Island& island : islands) {
                        RECT rect{};
                        ::GetWindowRect(island.window, &rect);
                        const Foreign foreign = ForeignPixels(image, area, rect, colour);
                        std::printf("%sstill desktop, %s: %s through %ls: %ld foreign pixels (first 0x%06x) in %ld,%ld,%ld,%ld\n",
                                    island.product ? "" : "MEASURE ", rounds[round], island.name,
                                    through_provider ? provider->name() : L"a screen blit", foreign.count,
                                    static_cast<unsigned>(foreign.sample), foreign.bounds.left, foreign.bounds.top,
                                    foreign.bounds.right, foreign.bounds.bottom);
                        if (island.product) { CHECK(foreign.count == 0); }
                    }
                }
            }
            if (provider) { provider->Release(); }
        }
        scrollshot.Destroy();
        panel.Destroy();
        editor.Destroy();
        if (plain) { ::DestroyWindow(plain); }
        if (layered) { ::DestroyWindow(layered); }
        Pump(std::chrono::milliseconds(50));
    }

    // A tooltip styled as every island styles its own, over a still desktop:
    // shown, shown again after hiding and moved the way tooltips are, through
    // activation and tracking. It must never be in a picture.
    {
        HWND tooltip = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
                                         WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, backdrop, nullptr,
                                         ::GetModuleHandleW(nullptr), nullptr);
        CHECK(tooltip != nullptr);
        TOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        info.hwnd = backdrop;
        info.uId = 1;
        info.lpszText = const_cast<wchar_t*>(L"Tooltip over the captured region");
        if (tooltip && ::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info)) != FALSE) {
            ui::StyleTooltip(tooltip);
            const char* rounds[]{"first shown", "shown again after hiding", "moved while visible"};
            std::unique_ptr<capture::IRegionCaptureProvider> provider;
            COLORREF colour = kColours[0];
            for (int round = 0; round < 3; ++round) {
                const POINT at{area.left + 200 + (round == 2 ? 120 : 0), area.top + 200 + (round == 2 ? 60 : 0)};
                if (round < 2) {
                    ::SendMessageW(tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
                    colour = kColours[round];
                    Paint(backdrop, colour);
                    Pump(std::chrono::milliseconds(400));
                    if (provider) { provider->Release(); }
                    provider = capture::CreateRegionCaptureProvider();
                    capture::DesktopSnapshot start;
                    CHECK(provider->CaptureDesktop(false, start));
                }
                ::SendMessageW(tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(at.x, at.y));
                ::SendMessageW(tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
                Pump(std::chrono::milliseconds(400));
                CHECK(::IsWindowVisible(tooltip) != FALSE);
                CheckExcluded(tooltip);
                RECT rect{};
                ::GetWindowRect(tooltip, &rect);
                for (const bool through_provider : {true, false}) {
                    platform::BgraImage image;
                    capture::DesktopSnapshot fixed;
                    const bool read = through_provider ? provider->CaptureDesktop(false, fixed) && fixed.CropRegion(area, image)
                                                       : ScreenBlit(area, image);
                    CHECK(read && Current(image, colour));
                    if (!read) { continue; }
                    const Foreign foreign = ForeignPixels(image, area, rect, colour);
                    std::printf("still desktop, %s: tooltip through %ls: %ld foreign pixels (first 0x%06x)\n", rounds[round],
                                through_provider ? provider->name() : L"a screen blit", foreign.count,
                                static_cast<unsigned>(foreign.sample));
                    CHECK(foreign.count == 0);
                }
            }
            if (provider) { provider->Release(); }
        } else {
            CHECK(!"the tooltip could not be created");
        }
        if (tooltip) { ::DestroyWindow(tooltip); }
        Pump(std::chrono::milliseconds(50));
    }

    // The recording panel as it is while recording and while paused.
    {
        ui::RecordingControls controls({});
        const RECT region{area.left + 40, area.top + 40, area.right - 40, area.bottom - 40};
        CHECK(controls.ShowConfiguring(region, app::RecordingConfiguration{}, false, nullptr));
        for (const app::SessionState state : {app::SessionState::RecordingActive, app::SessionState::RecordingPaused}) {
            controls.SetSessionState(state);
            put_over_backdrop(controls.hwnd());
            CHECK(::IsWindowVisible(controls.hwnd()) != FALSE);
            CheckExcluded(controls.hwnd());
            Examine(state == app::SessionState::RecordingActive ? "recording panel" : "paused recording panel",
                    {controls.hwnd()}, backdrop, area, true, reached);
        }
        controls.Destroy();
    }

    // The Scrollshot island during a capture.
    {
        ui::ScrollshotControls controls({});
        const RECT frame{area.left + 200, area.top + 100, area.right - 200, area.bottom - 100};
        CHECK(controls.Show(ui::ScrollshotControls::View::Capture, frame, nullptr));
        put_over_backdrop(controls.hwnd());
        CHECK(::IsWindowVisible(controls.hwnd()) != FALSE);
        CheckExcluded(controls.hwnd());
        Examine("Scrollshot island", {controls.hwnd()}, backdrop, area, true, reached);
        controls.Destroy();
    }

    // The panel through the start of a recording over a full-monitor frame,
    // where it lies over the recorded region: the frames read while it
    // changes owner, size and shape, and while it pauses and resumes.
    {
        const RECT full = monitor.bounds;
        ::SetWindowPos(backdrop, HWND_TOPMOST, full.left, full.top, platform::RectWidth(full), platform::RectHeight(full),
                       SWP_NOACTIVATE);
        Paint(backdrop, kColours[0]);
        Pump(std::chrono::milliseconds(100));
        RECT placed{};
        ::GetWindowRect(backdrop, &placed);
        std::printf("recording start over the region: backdrop %ld,%ld,%ld,%ld on a monitor %ld,%ld,%ld,%ld, work area bottom %ld\n",
                    placed.left, placed.top, placed.right, placed.bottom, full.left, full.top, full.right, full.bottom,
                    monitor.work_area.bottom);
        std::atomic<bool> stop{false};
        std::atomic<int> frames{0}, current{0}, dirty{0};
        std::atomic<long> worst{0};
        std::atomic<uint32_t> sample{0};
        std::thread reader([&] {
            ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto provider = capture::CreateRegionCaptureProvider();
            capture::RegionImageOptions options;
            options.await_first_composed_frame = true;
            options.wait_milliseconds = 16;
            platform::BgraImage image;
            while (!stop.load()) {
                const auto result = provider->CaptureRegionImage(full, options, image);
                if (result.status != capture::RegionCaptureStatus::FrameAcquired) { continue; }
                ++frames;
                // Frames from before the backdrop was composed are not about the panel.
                if (!Current(image, kColours[0])) { continue; }
                ++current;
                // The work area: a taskbar may stay above the backdrop.
                const Foreign foreign = ForeignPixels(image, full, monitor.work_area, kColours[0]);
                if (foreign.count > 0) {
                    ++dirty;
                    if (foreign.count > worst.load()) {
                        worst = foreign.count;
                        sample = foreign.sample;
                        std::printf("  frame %d: %ld foreign pixels in %ld,%ld,%ld,%ld\n", frames.load(), foreign.count,
                                    foreign.bounds.left, foreign.bounds.top, foreign.bounds.right, foreign.bounds.bottom);
                    }
                }
            }
            provider->Release();
            ::CoUninitialize();
        });
        Pump(std::chrono::milliseconds(300));
        for (int repeat = 0; repeat < 3; ++repeat) {
            ui::RecordingControls controls({});
            CHECK(controls.ShowConfiguring(full, app::RecordingConfiguration{}, true, backdrop));
            Pump(std::chrono::milliseconds(250));
            controls.RetireWorkspace();
            controls.SetElapsed(0);
            controls.SetSessionState(app::SessionState::RecordingActive);
            Pump(std::chrono::milliseconds(250));
            controls.SetSessionState(app::SessionState::RecordingPaused);
            Pump(std::chrono::milliseconds(150));
            controls.SetSessionState(app::SessionState::RecordingActive);
            Pump(std::chrono::milliseconds(150));
            CHECK(::IsWindowVisible(controls.hwnd()) != FALSE);
            CheckExcluded(controls.hwnd());
            controls.Destroy();
            Pump(std::chrono::milliseconds(150));
        }
        stop = true;
        reader.join();
        std::printf("recording start over the region: %d frames read, %d current, %d with foreign pixels (worst %ld, first 0x%06x)\n",
                    frames.load(), current.load(), dirty.load(), worst.load(), static_cast<unsigned>(sample.load()));
        if (current.load() > 0) {
            reached.duplication = true;
            CHECK(dirty.load() == 0);
        } else {
            std::printf("recording start over the region: no composed frame was read; not checked\n");
        }
    }

    // What making a dialog layered for its exclusion must not change: dragged
    // by its header, it shows only its own painting. The Exit question owned
    // by a window, as during a recording, and Settings, which has no owner,
    // dragged while active and dragged when another window was active, in
    // both appearances.
    const app::AppearanceTheme appearance = ui::theme::Preference();
    ::ShowWindow(backdrop, SW_HIDE);
    HWND thief = ::CreateWindowExW(0, backdrop_class.lpszClassName, L"Island capture test focus", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                   monitor.work_area.left + 10, monitor.work_area.top + 10, 200, 120, nullptr, nullptr,
                                   backdrop_class.hInstance, nullptr);
    CHECK(thief != nullptr);
    Paint(thief, kColours[1]);
    // The dialogs open in front, as Settings does after a click in the tray
    // menu: a click on a window of the test makes this the foreground process.
    if (thief) {
        ::SetWindowPos(thief, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Pump(std::chrono::milliseconds(100));
        RECT rect{};
        ::GetClientRect(thief, &rect);
        POINT centre{rect.right / 2, rect.bottom / 2};
        ::ClientToScreen(thief, &centre);
        CHECK(::SetCursorPos(centre.x, centre.y) != FALSE);
        Pump(std::chrono::milliseconds(50));
        if (::WindowFromPoint(centre) == thief) {
            INPUT click[2]{};
            click[0].type = click[1].type = INPUT_MOUSE;
            click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            CHECK(::SendInput(2, click, sizeof(INPUT)) == 2);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (::GetForegroundWindow() != thief && std::chrono::steady_clock::now() < deadline) {
                Pump(std::chrono::milliseconds(20));
            }
        }
        CHECK(::GetForegroundWindow() == thief);
        ::SetWindowPos(thief, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    struct Scenario {
        const char* name;
        bool settings, deactivated, hover_close;
        app::AppearanceTheme theme;
    };
    for (const Scenario scenario :
         {Scenario{"message dialog, dark", false, false, false, app::AppearanceTheme::Dark},
          Scenario{"Settings, dark", true, false, false, app::AppearanceTheme::Dark},
          Scenario{"Settings, dark, pointer on Close first", true, false, true, app::AppearanceTheme::Dark},
          Scenario{"Settings, dark, another window active first", true, true, false, app::AppearanceTheme::Dark},
          Scenario{"Settings, light, another window active first", true, true, false, app::AppearanceTheme::Light},
          Scenario{"Settings, light, pointer on Close first", true, false, true, app::AppearanceTheme::Light},
          Scenario{"message dialog, light, another window active first", false, true, false, app::AppearanceTheme::Light}}) {
        ui::theme::ApplyPreference(scenario.theme);
        ui::theme::NotifyWindows();
        DialogDrag drag;
        drag.thief = scenario.deactivated ? thief : nullptr;
        drag.hover_close = scenario.hover_close;
        drag.deadline = ::GetTickCount64() + 15000;
        active_drag = &drag;
        std::thread input(DriveDrag, std::ref(drag), ::GetCurrentThreadId());
        CHECK(::SetTimer(nullptr, 0, 50, DragTimer) != 0);
        if (scenario.settings) {
            ui::SettingsDialogModel model;
            static_cast<void>(ui::ShowSettingsDialog(nullptr, model, {}));
        } else {
            ui::ShowMessage(thief, L"The header of this dialog is dragged by the test.", L"Dialog drag test", MB_OK);
        }
        if (drag.stage.load() != kDragDone && drag.stage.load() != kDragFailed) { drag.stage = kDragFailed; }
        input.join();
        active_drag = nullptr;
        std::printf("dialog dragged by its header (%s): moved %ld,%ld in %d of 8 steps; on its Close button %ld pixels "
                    "not painted by Easnap before the drag, %ld after (first 0x%06x in %ld,%ld,%ld,%ld)\n",
                    scenario.name, drag.after_rect.left - drag.before_rect.left, drag.after_rect.top - drag.before_rect.top,
                    drag.steps_moved.load(), drag.before.count, drag.after.count, static_cast<unsigned>(drag.after.sample),
                    drag.after.bounds.left, drag.after.bounds.top, drag.after.bounds.right, drag.after.bounds.bottom);
        CHECK(drag.stage.load() == kDragDone);
        CHECK(drag.after_rect.left - drag.before_rect.left == 80 && drag.after_rect.top - drag.before_rect.top == 40);
        CHECK(drag.before.count == 0);
        CHECK(drag.after.count == 0);
        Pump(std::chrono::milliseconds(100));
    }
    if (thief) { ::DestroyWindow(thief); }
    ui::theme::ApplyPreference(app::AppearanceTheme::Dark);
    ui::theme::NotifyWindows();
    ::ShowWindow(backdrop, SW_SHOWNOACTIVATE);
    Pump(std::chrono::milliseconds(100));

    // The themed menu's submenu item, in both appearances.
    for (const app::AppearanceTheme theme : {app::AppearanceTheme::Dark, app::AppearanceTheme::Light}) {
        ui::theme::ApplyPreference(theme);
        ui::theme::NotifyWindows();
        HMENU root = ::CreatePopupMenu();
        HMENU submenu = ui::CreateAppearanceMenu(false);
        CHECK(root != nullptr && submenu != nullptr);
        CHECK(::AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), L"Appearance") != FALSE);
        CHECK(::AppendMenuW(root, MF_STRING, 1, L"About Easnap") != FALSE);
        MenuLook look;
        look.owner = backdrop;
        look.root = root;
        look.deadline = ::GetTickCount64() + 10000;
        active_menu = &look;
        const POINT at{monitor.work_area.left + 200, monitor.work_area.top + 200};
        // The pointer away from the menu: no item is hot.
        CHECK(::SetCursorPos(monitor.work_area.left + 20, monitor.work_area.bottom - 20) != FALSE);
        {
            ui::PopupScope scope(nullptr);
            ui::MenuThemeScope themed(backdrop, root);
            CHECK(::SetTimer(nullptr, 0, 50, MenuTimer) != 0);
            ::TrackPopupMenuEx(root, TPM_RETURNCMD | TPM_NONOTIFY | TPM_NOANIMATION, at.x, at.y, backdrop, nullptr);
        }
        active_menu = nullptr;
        ::DestroyMenu(root);
        CHECK(look.done);
        if (look.done) {
            const Foreign extra = Differences(look.drawn, look.seen);
            std::printf("themed menu, %s: %ld pixels of the %ldx%ld submenu item not drawn by Easnap (first 0x%06x in "
                        "%ld,%ld,%ld,%ld)\n",
                        theme == app::AppearanceTheme::Dark ? "dark" : "light", extra.count,
                        platform::RectWidth(look.item), platform::RectHeight(look.item),
                        static_cast<unsigned>(extra.sample), extra.bounds.left, extra.bounds.top, extra.bounds.right,
                        extra.bounds.bottom);
            CHECK(extra.count == 0);
        }
        Pump(std::chrono::milliseconds(100));
    }
    ui::theme::ApplyPreference(appearance);
    ui::theme::NotifyWindows();

    ::DestroyWindow(backdrop);
    ::UnregisterClassW(backdrop_class.lpszClassName, backdrop_class.hInstance);
    // The lab reads the capture path from this line: Desktop Duplication is
    // the path the production provider uses; a run that only reached the GDI
    // reads proves less, and the verdict says so.
    const char* path = "Unknown";
    if (reached.duplication) {
        path = "Duplication";
    } else if (reached.gdi || reached.blit) {
        path = "GDI";
    }
    std::printf("EASNAP_CAPTURE_PATH=%s\n", path);
    std::printf("island capture: %d passed, %d failed (screen blit %s, GDI fallback %s, Desktop Duplication %s)\n",
                passed, failed, reached.blit ? "checked" : "not reached", reached.gdi ? "checked" : "not reached",
                reached.duplication ? "checked" : "not reached");
    if (failed) { return 1; }
    // No path reached: nothing was shown. Skipped, not passed.
    return reached.blit || reached.gdi || reached.duplication ? 0 : 77;
}

}  // namespace

int main() {
    // Island text is measured in English whatever Windows speaks.
    strings::SetPreference(strings::LanguagePreference::English);
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    ::InitCommonControlsEx(&controls);
    const int code = Run();
    platform::GraphicsEnvironment::Get().Shutdown();
    ::CoUninitialize();
    return code;
}
