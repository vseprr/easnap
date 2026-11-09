// Easnap native tests - images pinned to the screen (ADR-038).
//
// Real pin windows from the production `ui::PinnedImages`, driven by
// messages on a private window station and desktop: nothing is shown on the
// user's screen, and the clipboard the copy check fills is the station's own.

#include "platform/image_export.h"
#include "temp_folder.h"
#include "ui/native_controls.h"
#include "ui/pin_layout.h"
#include "ui/pinned_images.h"
#include "ui/ui_theme.h"
#include "app/app_settings.h"
#include "strings/strings.h"

#include <windowsx.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

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

class PrivateDesktop {
public:
    bool Open() {
        old_station_ = ::GetProcessWindowStation();
        old_desktop_ = ::GetThreadDesktop(::GetCurrentThreadId());
        station_ = ::CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
        if (!station_ || !::SetProcessWindowStation(station_)) { return false; }
        desktop_ = ::CreateDesktopW(L"EasnapPinTests", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        return desktop_ && ::SetThreadDesktop(desktop_);
    }
    ~PrivateDesktop() {
        CHECK(::SetThreadDesktop(old_desktop_));
        CHECK(::SetProcessWindowStation(old_station_));
        if (desktop_) { ::CloseDesktop(desktop_); }
        if (station_) { ::CloseWindowStation(station_); }
    }

private:
    HWINSTA old_station_{}, station_{};
    HDESK old_desktop_{}, desktop_{};
};

platform::BgraImage Picture(int width, int height) {
    platform::BgraImage image;
    image.Reset(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.row(y)[x] = 0xff000000u | (static_cast<uint32_t>((x * 7) % 251) << 16) |
                              (static_cast<uint32_t>((y * 5) % 241) << 8) | static_cast<uint32_t>((x + y) % 239);
        }
    }
    return image;
}

RECT WindowRect(HWND window) {
    RECT r{};
    ::GetWindowRect(window, &r);
    return r;
}
LONG Width(const RECT& r) { return r.right - r.left; }
LONG Height(const RECT& r) { return r.bottom - r.top; }
LONG Border(HWND window) { return ui::PinBorderPixels(static_cast<float>(::GetDpiForWindow(window)) / 96.0f); }
uint32_t Rgb(uint32_t pixel) { return pixel & 0x00ffffffu; }
uint32_t FromColorRef(COLORREF color) {
    return (static_cast<uint32_t>(GetRValue(color)) << 16) | (static_cast<uint32_t>(GetGValue(color)) << 8) | GetBValue(color);
}

/// What the pin draws into its client area, read through WM_PRINTCLIENT.
platform::BgraImage Painted(HWND window) {
    RECT client{};
    ::GetClientRect(window, &client);
    platform::BgraImage picture;
    picture.Reset(client.right, client.bottom);
    {
        platform::GdiImageSurface surface(picture);
        if (surface.valid()) { ::SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(surface.dc()), PRF_CLIENT); }
    }
    return picture;
}

void Wheel(HWND window, int delta, POINT at) {
    ::SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(delta))), MAKELPARAM(at.x, at.y));
}

void KeyWithControl(HWND window, WPARAM key) {
    std::array<BYTE, 256> keys{};
    keys[VK_CONTROL] = 0x80;
    ::SetKeyboardState(keys.data());
    ::SendMessageW(window, WM_KEYDOWN, key, 0);
    keys.fill(0);
    ::SetKeyboardState(keys.data());
}

void PumpPosted() {
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

/// Runs this thread's messages until `done` holds or the deadline passes: a
/// copy or save runs on a worker and reports back with a message.
template <typename Condition>
bool PumpUntil(Condition done, DWORD timeout_ms = 30000) {
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    while (!done()) {
        PumpPosted();
        if (done()) { return true; }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) { return false; }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>((std::min<ULONGLONG>)(deadline - now, 100)),
                                    QS_ALLINPUT);
    }
    return true;
}

RECT SourceOnPrimary(LONG width, LONG height) {
    const auto monitor = platform::MonitorForPoint({0, 0});
    const LONG left = monitor.work_area.left + 120;
    const LONG top = monitor.work_area.top + 90;
    return {left, top, left + width, top + height};
}

void WindowAndPicture() {
    app::AppSettings settings;
    ui::PinnedImages pins(&settings);
    const platform::BgraImage image = Picture(200, 120);
    const RECT source = SourceOnPrimary(200, 120);
    CHECK(!pins.Pin(platform::BgraImage{}, source));  // Nothing to show.
    CHECK(pins.count() == 0);
    CHECK(pins.Pin(image, source));
    CHECK(pins.count() == 1 && pins.windows().size() == 1);
    const HWND pin = pins.windows().front();

    // A plain topmost tool window: not layered, no caption, not excluded.
    const LONG_PTR style = ::GetWindowLongPtrW(pin, GWL_STYLE);
    const LONG_PTR ex_style = ::GetWindowLongPtrW(pin, GWL_EXSTYLE);
    CHECK((style & WS_POPUP) != 0 && (style & (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME)) == 0);
    CHECK((ex_style & WS_EX_TOPMOST) != 0 && (ex_style & WS_EX_TOOLWINDOW) != 0 && (ex_style & WS_EX_LAYERED) == 0);
    DWORD affinity = 0xffff;
    CHECK(::GetWindowDisplayAffinity(pin, &affinity) && affinity == WDA_NONE);
    // A private window station counts no window as visible; the style says it.
    CHECK(ui::IsScreenContent(pin) && (style & WS_VISIBLE) != 0);
    wchar_t name[64]{};
    ::GetWindowTextW(pin, name, 64);
    CHECK(std::wstring_view(name) == L"Pinned image");

    // Exactly over its source at 100 %, the border outside the image.
    const LONG border = Border(pin);
    const RECT window = WindowRect(pin);
    CHECK(window.left == source.left - border && window.top == source.top - border);
    CHECK(window.right == source.right + border && window.bottom == source.bottom + border);

    // The whole pin moves it.
    const POINT inside{(window.left + window.right) / 2, (window.top + window.bottom) / 2};
    CHECK(::SendMessageW(pin, WM_NCHITTEST, 0, MAKELPARAM(inside.x, inside.y)) == HTCAPTION);

    // The painted pixels: the image at 100 % inside the border colour.
    ::SendMessageW(pin, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
    platform::BgraImage painted = Painted(pin);
    CHECK(painted.width == image.width + 2 * border && painted.height == image.height + 2 * border);
    bool same = painted.width == image.width + 2 * border && painted.height == image.height + 2 * border;
    for (int y = 0; same && y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (Rgb(painted.row(y + border)[x + border]) != Rgb(image.row(y)[x])) {
                same = false;
                break;
            }
        }
    }
    CHECK(same);
    if (painted.width > 0) {
        CHECK(Rgb(painted.row(0)[0]) == FromColorRef(ui::theme::Current().border));
        // Active, the border takes the focus colour.
        ::SendMessageW(pin, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        painted = Painted(pin);
        CHECK(Rgb(painted.row(0)[0]) == FromColorRef(ui::theme::Current().focus));
        CHECK(Rgb(painted.row(border)[border]) == Rgb(image.row(0)[0]));
    }

    // Wheel: one step larger, the image point under the pointer stays put.
    const POINT anchor{source.left + 50, source.top + 30};  // Image pixel 50, 30.
    Wheel(pin, WHEEL_DELTA, anchor);
    RECT zoomed = WindowRect(pin);
    CHECK(Width(zoomed) == std::lround(200 * 1.1) + 2 * border && Height(zoomed) == std::lround(120 * 1.1) + 2 * border);
    CHECK(std::abs((anchor.x - (zoomed.left + border)) - std::lround(50 * 1.1)) <= 1);
    CHECK(std::abs((anchor.y - (zoomed.top + border)) - std::lround(30 * 1.1)) <= 1);
    // Enlarged, the corners are still the image's corners.
    painted = Painted(pin);
    if (painted.width > 2 * border && painted.height > 2 * border) {
        CHECK(Rgb(painted.row(border)[border]) == Rgb(image.row(0)[0]));
        CHECK(Rgb(painted.row(painted.height - border - 1)[painted.width - border - 1]) == Rgb(image.row(119)[199]));
    }
    // Two half notches make one step; then back down twice to 90 %.
    Wheel(pin, WHEEL_DELTA / 2, anchor);
    CHECK(Width(WindowRect(pin)) == Width(zoomed));
    Wheel(pin, WHEEL_DELTA / 2, anchor);
    CHECK(Width(WindowRect(pin)) == std::lround(200 * 1.25) + 2 * border);
    Wheel(pin, -WHEEL_DELTA, anchor);
    Wheel(pin, -WHEEL_DELTA, anchor);
    Wheel(pin, -WHEEL_DELTA, anchor);
    CHECK(Width(WindowRect(pin)) == std::lround(200 * 0.9) + 2 * border);
    // Reduced, the picture has the reduced size.
    painted = Painted(pin);
    CHECK(painted.width == std::lround(200 * 0.9) + 2 * border);
    // Ctrl+1 returns to the captured size, around the pin's centre.
    const RECT before = WindowRect(pin);
    KeyWithControl(pin, '1');
    const RECT original = WindowRect(pin);
    CHECK(Width(original) == 200 + 2 * border && Height(original) == 120 + 2 * border);
    CHECK(std::abs((original.left + original.right) - (before.left + before.right)) <= 2);
    // Original size from the menu is the same command.
    Wheel(pin, WHEEL_DELTA, anchor);
    ::SendMessageW(pin, WM_COMMAND, ui::kPinCommandOriginalSize, 0);
    CHECK(Width(WindowRect(pin)) == 200 + 2 * border);

    // Copy from the menu: the full image on this station's clipboard, once
    // the worker has prepared it.
    ::SendMessageW(pin, WM_COMMAND, ui::kPinCommandCopy, 0);
    CHECK(platform::ImageExportActive());
    CHECK(PumpUntil([] { return !platform::ImageExportActive(); }));
    CHECK(::IsClipboardFormatAvailable(CF_DIB));
    const bool opened = ::OpenClipboard(nullptr) != FALSE;
    CHECK(opened);
    if (opened) {
        HGLOBAL dib = ::GetClipboardData(CF_DIB);
        const auto* header = dib ? static_cast<const BITMAPINFOHEADER*>(::GlobalLock(dib)) : nullptr;
        CHECK(header && header->biWidth == 200 && std::abs(header->biHeight) == 120);
        if (header) { ::GlobalUnlock(dib); }
        ::CloseClipboard();
    }

    // A change of scale: the pin takes the suggested rectangle; the zoom
    // follows from its width, and the next wheel step starts from there.
    const UINT dpi = ::GetDpiForWindow(pin);
    RECT suggested{original.left, original.top, original.left + 160 + 2 * border, original.top + 96 + 2 * border};
    ::SendMessageW(pin, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
    const RECT adopted = WindowRect(pin);
    CHECK(::EqualRect(&adopted, &suggested) != FALSE);
    Wheel(pin, WHEEL_DELTA, {adopted.left, adopted.top});
    CHECK(Width(WindowRect(pin)) == std::lround(200 * 0.9) + 2 * border);

    // A display change that leaves the pin off every work area brings it back.
    const RECT virtual_screen = platform::VirtualScreenBounds();
    ::SetWindowPos(pin, nullptr, virtual_screen.right + 2000, virtual_screen.bottom + 2000, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ::SendMessageW(pin, WM_DISPLAYCHANGE, 32, 0);
    const RECT returned = WindowRect(pin);
    const RECT work = platform::MonitorForRect(returned).work_area;
    CHECK(returned.left >= work.left && returned.top >= work.top && returned.right <= work.right && returned.bottom <= work.bottom);

    // Esc closes the pin and releases its window.
    ::SendMessageW(pin, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(!::IsWindow(pin) && pins.count() == 0 && pins.windows().empty());
}

// A pin does not hold back keeping the workspace on screen during a capture;
// a plain visible window of the process still does. Runs on the isolated
// desktop, where windows count as visible (a private window station counts
// none), and copies nothing.
void ScreenContentDoesNotHoldBackCapture() {
    if (!ui::CaptureExclusionSupported()) {
        std::printf("NOTE: capture exclusion is not supported here; the screen-content check was not run\n");
        return;
    }
    // Easnap always has its hidden host window; without any window on the
    // desktop EnumWindows reports failure, and the answer is false.
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    CHECK(host != nullptr);
    app::AppSettings settings;
    ui::PinnedImages pins(&settings);
    CHECK(pins.Pin(Picture(64, 48), SourceOnPrimary(64, 48)));
    const HWND pin = pins.count() == 1 ? pins.windows().front() : nullptr;
    if (pin == nullptr || ::IsWindowVisible(pin) == FALSE) {
        std::printf("NOTE: windows do not count as visible on this desktop; the screen-content check was not run\n");
        pins.CloseAll();
        ::DestroyWindow(host);
        return;
    }
    // This also fails when the pin brings a window of its own that is not
    // marked, as the class shadow (CS_DROPSHADOW) did.
    CHECK(ui::CanCaptureVisibleWindows());
    HWND plain = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP | WS_VISIBLE, 0, 0, 20, 20, nullptr, nullptr,
                                   ::GetModuleHandleW(nullptr), nullptr);
    CHECK(plain != nullptr && ::IsWindowVisible(plain) && !ui::CanCaptureVisibleWindows());
    if (plain) { ::DestroyWindow(plain); }
    CHECK(ui::CanCaptureVisibleWindows());
    pins.CloseAll();
    CHECK(ui::CanCaptureVisibleWindows());
    ::DestroyWindow(host);
}

void ClosingPins() {
    app::AppSettings settings;
    ui::PinnedImages pins(&settings);
    const platform::BgraImage image = Picture(80, 60);
    // Double-click on the pin closes it.
    CHECK(pins.Pin(image, SourceOnPrimary(80, 60)));
    HWND pin = pins.windows().back();
    const RECT r = WindowRect(pin);
    ::SendMessageW(pin, WM_NCLBUTTONDBLCLK, HTCAPTION, MAKELPARAM(r.left + 5, r.top + 5));
    CHECK(!::IsWindow(pin) && pins.count() == 0);
    // Close from the menu, and WM_CLOSE (Alt+F4).
    CHECK(pins.Pin(image, SourceOnPrimary(80, 60)));
    pin = pins.windows().back();
    ::SendMessageW(pin, WM_COMMAND, ui::kPinCommandClose, 0);
    CHECK(!::IsWindow(pin));
    CHECK(pins.Pin(image, SourceOnPrimary(80, 60)));
    pin = pins.windows().back();
    ::SendMessageW(pin, WM_CLOSE, 0, 0);
    CHECK(!::IsWindow(pin) && pins.count() == 0);

    // Several pins, newest on top; Close all from one of them closes every one.
    for (int i = 0; i < 3; ++i) { CHECK(pins.Pin(image, SourceOnPrimary(80, 60))); }
    std::vector<HWND> open = pins.windows();
    CHECK(open.size() == 3 && pins.count() == 3);
    ::SendMessageW(open[1], WM_COMMAND, ui::kPinCommandCloseAll, 0);
    for (HWND window : open) { CHECK(!::IsWindow(window)); }
    CHECK(pins.count() == 0);
    // CloseAll from the owner, and pins made after closed ones.
    for (int i = 0; i < 2; ++i) { CHECK(pins.Pin(image, SourceOnPrimary(80, 60))); }
    CHECK(pins.count() == 2);
    pins.CloseAll();
    CHECK(pins.count() == 0 && pins.windows().empty());
}

// A pin asked to close while its own menu is open closes when the menu
// returns, not under it.
struct BusyClose {
    ui::PinnedImages* pins{};
    HWND pin{};
    bool menu_seen{};
    bool alive_while_busy{};
    int ticks{};
};
BusyClose g_busy;

void CALLBACK BusyTimer(HWND, UINT, UINT_PTR timer, DWORD) {
    ++g_busy.ticks;
    if (ui::PopupActive()) {
        g_busy.menu_seen = true;
        g_busy.pins->CloseAll();
        g_busy.alive_while_busy = ::IsWindow(g_busy.pin) != FALSE;
        ::KillTimer(nullptr, timer);
        ::EndMenu();
    } else if (g_busy.ticks > 100) {
        ::KillTimer(nullptr, timer);
    }
}

void CloseWhileBusy() {
    app::AppSettings settings;
    ui::PinnedImages pins(&settings);
    CHECK(pins.Pin(Picture(120, 90), SourceOnPrimary(120, 90)));
    CHECK(pins.Pin(Picture(120, 90), SourceOnPrimary(120, 90)));
    g_busy = {&pins, pins.windows().front()};
    const RECT r = WindowRect(g_busy.pin);
    const UINT_PTR timer = ::SetTimer(nullptr, 0, 20, BusyTimer);
    CHECK(timer != 0);
    ::SendMessageW(g_busy.pin, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(g_busy.pin), MAKELPARAM(r.left + 10, r.top + 10));
    if (!g_busy.menu_seen) {
        // The menu could not run its loop here: the check is not available,
        // which is reported, not counted as a pass.
        ::KillTimer(nullptr, timer);
        std::printf("NOTE: the pin's menu did not open on this desktop; the busy-close check was not run\n");
        pins.CloseAll();
        return;
    }
    PumpPosted();
    CHECK(g_busy.alive_while_busy);
    CHECK(!::IsWindow(g_busy.pin) && pins.count() == 0);
}

/// Save from a pin with quick save: written into the screenshots folder (one
/// of the test's own) on a worker. A close asked for meanwhile waits for it.
void QuickSave() {
    test::TempFolder folder(L"easnap-pin-quick-save");
    CHECK(folder.ok());
    app::AppSettings settings;
    settings.quick_save_images = true;
    settings.image_folder = folder.path();
    ui::PinnedImages pins(&settings);
    const platform::BgraImage image = Picture(200, 120);
    CHECK(pins.Pin(image, SourceOnPrimary(200, 120)));
    const HWND pin = pins.windows().front();
    KeyWithControl(pin, 'S');
    CHECK(platform::ImageExportActive());
    ::SendMessageW(pin, WM_CLOSE, 0, 0);
    CHECK(::IsWindow(pin) != FALSE);  // Busy: it closes once the file is there.
    CHECK(PumpUntil([] { return !platform::ImageExportActive(); }));
    CHECK(!::IsWindow(pin) && pins.count() == 0);
    const std::vector<std::wstring> saved = folder.Files(L"Easnap*.png");
    CHECK(saved.size() == 1 && folder.StagedFiles() == 0 && settings.last_image_directory.empty());
    if (saved.size() == 1) {
        platform::BgraImage decoded;
        CHECK(test::DecodeImageFile(folder.File(saved.front().c_str()), decoded));
        CHECK(decoded.width == 200 && decoded.height == 120 && decoded.pixels == image.pixels);
    }
}

void PlacementOfOtherImages() {
    app::AppSettings settings;
    ui::PinnedImages pins(&settings);
    // A stitched image, much taller than the screen, pinned from its frame.
    const RECT frame = SourceOnPrimary(300, 400);
    CHECK(pins.Pin(Picture(300, 6000), frame));
    const HWND pin = pins.windows().front();
    const RECT window = WindowRect(pin);
    const RECT work = platform::MonitorForRect(frame).work_area;
    CHECK(window.left >= work.left && window.top >= work.top && window.right <= work.right && window.bottom <= work.bottom);
    CHECK(Height(window) < 6000);
    pins.CloseAll();
}

} // namespace

int main() {
    // Assertions name windows by their English text, whatever Windows speaks.
    strings::SetPreference(strings::LanguagePreference::English);
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    ScreenContentDoesNotHoldBackCapture();
    std::thread tests([] {
        PrivateDesktop desktop;
        if (!desktop.Open()) {
            std::fprintf(stderr, "Private desktop creation failed: %lu\n", ::GetLastError());
            ++failed;
            return;
        }
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        WindowAndPicture();
        ClosingPins();
        CloseWhileBusy();
        QuickSave();
        PlacementOfOtherImages();
        ::CoUninitialize();
        // Notices from the copy check close on their own timers; end them here.
        ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
            wchar_t name[64]{};
            ::GetClassNameW(window, name, 64);
            if (std::wstring_view(name) == L"EasnapCaptureCopiedNotice") { ::DestroyWindow(window); }
            return TRUE; }, 0);
    });
    tests.join();
    std::printf("Pinned images: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
