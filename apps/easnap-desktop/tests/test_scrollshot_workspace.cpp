// Easnap integration tests - Scrollshot in the production workspace.
//
// Production windows only, driven with window messages so the test can run on
// a private, non-input desktop: the three-mode bar, the locked frame and its
// open input plane, the Start/help island, the four-action island with its
// live preview, the tools island's Scrollshot button, and the shared editor on
// a stitched image that scrolls and zooms. Nothing is captured from a screen.

#include "native_test_monitor.h"
#include "scrollshot_synthetic.h"

#include "editor/screenshot_editor.h"
#include "strings/strings.h"
#include "ui/capture_workspace.h"
#include "ui/native_controls.h"
#include "ui/overlay.h"
#include "ui/scrollshot_controls.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace easnap;
using app::CaptureMode;
using easnap_test::synthetic::Image;
using easnap_test::synthetic::MakeArticle;
using ui::ScrollshotCommand;
using ui::ScrollshotControls;

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

platform::BgraImage ToBgra(const Image& image, int first_row = 0, int rows = -1) {
    platform::BgraImage out;
    const int count = rows < 0 ? image.height - first_row : rows;
    out.Reset(image.width, count);
    for (int y = 0; y < count; ++y) {
        std::copy(image.Row(first_row + y), image.Row(first_row + y) + image.width, out.row(y));
    }
    return out;
}

capture::DesktopSnapshot FlatSnapshot(const std::vector<platform::MonitorInfo>& monitors) {
    capture::DesktopSnapshot snapshot;
    snapshot.virtual_bounds = platform::VirtualScreenBounds();
    for (const auto& monitor : monitors) {
        capture::MonitorFrame frame;
        frame.desktop_rect = monitor.bounds;
        frame.image.Reset(platform::RectWidth(monitor.bounds), platform::RectHeight(monitor.bounds));
        std::fill(frame.image.pixels.begin(), frame.image.pixels.end(), 0xff203040u);
        snapshot.monitors.push_back(std::move(frame));
    }
    return snapshot;
}

void SetControlKey(bool down) {
    std::array<BYTE, 256> keys{};
    if (down) { keys[VK_CONTROL] = 0x80; }
    ::SetKeyboardState(keys.data());
}

int ThreadWindowCount() {
    int count = 0;
    ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND, LPARAM value) -> BOOL {
        ++*reinterpret_cast<int*>(value);
        return TRUE; }, reinterpret_cast<LPARAM>(&count));
    return count;
}

std::wstring WindowText(HWND window) {
    wchar_t text[256]{};
    ::GetWindowTextW(window, text, ARRAYSIZE(text));
    return text;
}

/// Visible, enabled push buttons of a window, left to right.
std::vector<HWND> VisibleButtons(HWND parent) {
    std::vector<HWND> buttons;
    for (HWND child = ::GetWindow(parent, GW_CHILD); child != nullptr; child = ::GetWindow(child, GW_HWNDNEXT)) {
        wchar_t name[16]{};
        ::GetClassNameW(child, name, ARRAYSIZE(name));
        if (::lstrcmpiW(name, L"BUTTON") == 0 && (::GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0) {
            buttons.push_back(child);
        }
    }
    std::sort(buttons.begin(), buttons.end(), [](HWND a, HWND b) {
        RECT ra{}, rb{};
        ::GetWindowRect(a, &ra);
        ::GetWindowRect(b, &rb);
        return ra.left < rb.left;
    });
    return buttons;
}

bool Overlaps(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

/// The visible windows that darken the desktop outside the frame on `monitor`.
std::vector<HWND> VisibleShading(const RECT& monitor) {
    struct Query {
        std::vector<HWND> windows;
        RECT monitor;
    } query{{}, monitor};
    ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        auto* found = reinterpret_cast<Query*>(value);
        RECT rect{};
        if (::IsWindowVisible(window) && WindowText(window) == L"Easnap outside shading" &&
            ::GetWindowRect(window, &rect) && Overlaps(rect, found->monitor)) {
            found->windows.push_back(window);
        }
        return TRUE; }, reinterpret_cast<LPARAM>(&query));
    return query.windows;
}

// --- Workspace policy ---------------------------------------------------------

void Policy() {
    ui::CaptureWorkspace workspace;
    workspace.BeginScrollshotCapture();  // Not before a region is being edited.
    CHECK(!workspace.scrollshot_locked() && workspace.mode() == CaptureMode::Screenshot);

    workspace.EnterEditing();
    workspace.SetMode(CaptureMode::Scrollshot);
    CHECK(workspace.scrollshot_visible() && !workspace.screenshot_visible() && !workspace.recording_visible());
    CHECK(workspace.can_adjust_selection() && workspace.can_switch_mode());  // Start has not been pressed.
    CHECK(workspace.accent() == ui::theme::Current().screenshot_border);
    workspace.BeginScrollshotCapture();
    CHECK(workspace.scrollshot_locked() && workspace.scrollshot_capturing());
    CHECK(!workspace.can_adjust_selection() && !workspace.can_switch_mode());
    workspace.SetMode(CaptureMode::Record);  // Locked: ignored.
    CHECK(workspace.mode() == CaptureMode::Scrollshot);
    workspace.StopScrollshotCapture();
    CHECK(workspace.scrollshot_locked() && !workspace.scrollshot_capturing());
    CHECK(workspace.scrollshot_phase() == ui::ScrollshotPhase::Stopped);
    CHECK(!workspace.can_adjust_selection() && !workspace.can_switch_mode());

    // Edit turns it into a fixed screenshot that never completes by itself.
    workspace.EditScrollshot();
    CHECK(!workspace.scrollshot_locked() && workspace.screenshot_fixed() && workspace.screenshot_visible());
    CHECK(!workspace.quick_return_armed() && !workspace.DocumentCommitted());

    // The tools island's button: from an untouched Screenshot frame straight
    // to capturing. It is not a round trip that could copy on return.
    workspace.Reset();
    workspace.EnterEditing();
    workspace.BeginScrollshotCapture();
    CHECK(workspace.mode() == CaptureMode::Scrollshot && workspace.scrollshot_capturing());
    CHECK(!workspace.quick_return_armed() && !workspace.screenshot_fixed());

    // A fixed screenshot can no longer become a Scrollshot.
    workspace.Reset();
    workspace.EnterEditing();
    workspace.EditorFeatureUsed();
    workspace.BeginScrollshotCapture();
    CHECK(!workspace.scrollshot_locked() && workspace.screenshot_fixed());
}

// --- Frame drawn around the content ---------------------------------------------

void FrameAroundContent() {
    const RECT monitor{0, 0, 400, 300};
    const COLORREF blank = RGB(255, 0, 255), accent = RGB(10, 200, 30);
    const auto paint = [&](const RECT& selection, std::vector<COLORREF>& out) {
        ui::RegionSelector selector;
        selector.Reset(monitor);
        selector.SetSelection(selection);
        platform::BgraImage pixels;
        pixels.Reset(400, 300);
        {
            platform::GdiImageSurface surface(pixels);
            CHECK(surface.valid());
            ui::theme::FillSolid(surface.dc(), monitor, blank);
            selector.RenderGdi(surface.dc(), monitor, 1.0f, nullptr, false, accent, true);
            ::GdiFlush();
        }
        out.resize(pixels.pixels.size());
        for (size_t i = 0; i < out.size(); ++i) {
            const uint32_t p = pixels.pixels[i];
            out[i] = RGB((p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
        }
    };
    const auto at = [](const std::vector<COLORREF>& pixels, int x, int y) { return pixels[static_cast<size_t>(y) * 400 + x]; };

    // Room on every side: not one pixel of the selection is covered.
    std::vector<COLORREF> pixels;
    const RECT inner{100, 80, 300, 220};
    paint(inner, pixels);
    int covered = 0, outside = 0;
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 400; ++x) {
            const bool in = x >= inner.left && x < inner.right && y >= inner.top && y < inner.bottom;
            if (at(pixels, x, y) == accent) { in ? ++covered : ++outside; }
        }
    }
    CHECK(covered == 0 && outside > 0);
    CHECK(at(pixels, inner.left - 1, 150) == accent && at(pixels, inner.right, 150) == accent);
    CHECK(at(pixels, 200, inner.top - 1) == accent && at(pixels, 200, inner.bottom) == accent);
    CHECK(at(pixels, inner.left - 3, 150) == blank);  // Two pixels wide at scale 1, no handles, no label.

    // A frame that fills the monitor has no outside: its edges stay visible inside.
    paint(monitor, pixels);
    CHECK(at(pixels, 0, 150) == accent && at(pixels, 399, 150) == accent);
    CHECK(at(pixels, 200, 0) == accent && at(pixels, 200, 299) == accent);
    CHECK(at(pixels, 200, 150) == blank);
}

// --- A Windows edition without Media Foundation ---------------------------------------

void RecordingUnavailable() {
    const auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) { return; }
    const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
    const capture::DesktopSnapshot snapshot = FlatSnapshot(monitors);

    int refused = 0, mode_changes = 0;
    ui::OverlayController::Callbacks callbacks;
    callbacks.on_editing_changed = [&](CaptureMode, const RECT&) { ++mode_changes; };
    callbacks.on_recording_unavailable = [&] { ++refused; };
    ui::OverlayController overlay(std::move(callbacks));
    overlay.SetRecordingAvailable(false);
    CHECK(overlay.Show(snapshot, POINT{monitor.bounds.left + 200, monitor.bounds.top + 200}));

    const HWND modes = ::FindWindowW(L"EasnapCaptureModes", nullptr);
    const HWND record = ::GetDlgItem(modes, 3002);
    // The button stays in the bar and can be chosen: choosing it is how the
    // reason is given.
    CHECK(record != nullptr && ::IsWindowEnabled(record) != FALSE && WindowText(record) == L"Record");
    overlay.EnterEditing();
    overlay.selector().SetSelection(RECT{monitor.bounds.left + 300, monitor.bounds.top + 200,
                                         monitor.bounds.left + 900, monitor.bounds.top + 600});
    overlay.RegionChanged();
    const int changes_before = mode_changes;
    ::SendMessageW(modes, WM_COMMAND, MAKEWPARAM(3002, BN_CLICKED), reinterpret_cast<LPARAM>(record));
    CHECK(refused == 1 && overlay.mode() == CaptureMode::Screenshot && mode_changes == changes_before);
    CHECK(::SendMessageW(record, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    CHECK(::SendMessageW(::GetDlgItem(modes, 3000), BM_GETCHECK, 0, 0) == BST_CHECKED);
    ::SendMessageW(modes, WM_COMMAND, MAKEWPARAM(3002, BN_CLICKED), reinterpret_cast<LPARAM>(record));
    CHECK(refused == 2 && overlay.mode() == CaptureMode::Screenshot);  // Asked again, told again.

    // Once the media features are there the same button enters the mode.
    overlay.SetRecordingAvailable(true);
    ::SendMessageW(modes, WM_COMMAND, MAKEWPARAM(3002, BN_CLICKED), reinterpret_cast<LPARAM>(record));
    CHECK(refused == 2 && overlay.mode() == CaptureMode::Record && mode_changes == changes_before + 1);
    overlay.Destroy();
}

// --- Overlay phases ----------------------------------------------------------------

void OverlayPhases() {
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    if (monitors.empty()) { return; }
    const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
    const capture::DesktopSnapshot snapshot = FlatSnapshot(monitors);

    int cancels = 0, mode_changes = 0;
    CaptureMode changed_to = CaptureMode::Screenshot;
    std::vector<ScrollshotCommand> commands;
    ui::OverlayController::Callbacks callbacks;
    callbacks.on_cancelled = [&] { ++cancels; };
    callbacks.on_editing_changed = [&](CaptureMode mode, const RECT&) { ++mode_changes; changed_to = mode; };
    callbacks.on_scrollshot_command = [&](ScrollshotCommand command) { commands.push_back(command); };
    int wheel_delta = 0;
    bool wheel_control = false;
    callbacks.on_editor_wheel = [&](int delta, bool control, bool, POINT) { wheel_delta += delta; wheel_control = control; };
    ui::OverlayController overlay(std::move(callbacks));
    const POINT pointer{monitor.bounds.left + 200, monitor.bounds.top + 200};
    CHECK(overlay.Show(snapshot, pointer));

    const HWND modes = ::FindWindowW(L"EasnapCaptureModes", nullptr);
    CHECK(modes != nullptr);
    // Screenshot | Scrollshot | Record, left to right, labelled from the string table.
    const std::array<const wchar_t*, 3> labels{L"Screenshot", L"Scrollshot", L"Record"};
    LONG previous_right = LONG_MIN;
    LONG width = 0;
    for (int i = 0; i < 3; ++i) {
        HWND button = ::GetDlgItem(modes, 3000 + i);
        CHECK(button != nullptr);
        RECT rect{};
        ::GetWindowRect(button, &rect);
        CHECK(rect.left > previous_right);
        previous_right = rect.right;
        if (i == 0) { width = rect.right - rect.left; }
        CHECK(rect.right - rect.left == width);  // One width for all three.
        if (ui::CaptureExclusionSupported() || i != 1) { CHECK(WindowText(button) == labels[static_cast<size_t>(i)]); }
        CHECK(::IsWindowEnabled(button) == ((i != 1 || ui::CaptureExclusionSupported()) ? TRUE : FALSE));
    }
    CHECK(::GetDlgItem(modes, 3003) == nullptr);
    if (!ui::CaptureExclusionSupported()) {
        std::printf("Scrollshot needs capture exclusion; the remaining overlay checks are not applicable here.\n");
        overlay.Destroy();
        return;
    }

    const RECT frame{monitor.bounds.left + 300, monitor.bounds.top + 200, monitor.bounds.left + 900, monitor.bounds.top + 600};
    overlay.EnterEditing();
    overlay.selector().SetSelection(frame);
    overlay.RegionChanged();
    const int changes_before = mode_changes;
    ::SendMessageW(modes, WM_COMMAND, MAKEWPARAM(3001, BN_CLICKED), reinterpret_cast<LPARAM>(::GetDlgItem(modes, 3001)));
    CHECK(overlay.mode() == CaptureMode::Scrollshot && mode_changes == changes_before + 1);
    CHECK(changed_to == CaptureMode::Scrollshot && overlay.workspace().can_adjust_selection());

    const HWND dim = overlay.owner_window();
    const auto client = [&](LONG x, LONG y) { return MAKELPARAM(x - monitor.bounds.left, y - monitor.bounds.top); };
    HRGN region = ::CreateRectRgn(0, 0, 0, 0);
    CHECK(::GetWindowRgn(dim, region) == ERROR);  // The input plane covers everything.

    // Enter on the workspace asks to start while the frame is still adjustable.
    SetControlKey(false);
    ::SendMessageW(dim, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(commands.size() == 1 && commands.back() == ScrollshotCommand::Start);
    commands.clear();

    // One translucent window with a region shades the monitor while the frame
    // is adjusted. Before the first picture four plain windows around the
    // frame replace it, so that nothing translucent contains the frame.
    std::vector<HWND> shading = VisibleShading(monitor.bounds);
    RECT shade_rect{};
    CHECK(shading.size() == 1 && ::GetWindowRect(shading[0], &shade_rect) && ::EqualRect(&shade_rect, &monitor.bounds));
    overlay.ExposeFrame(true);
    CHECK(overlay.frame_exposed());
    shading = VisibleShading(monitor.bounds);
    CHECK(shading.size() == 4);
    LONGLONG shaded_area = 0;
    bool around_frame = true;
    for (HWND strip : shading) {
        RECT rect{};
        ::GetWindowRect(strip, &rect);
        shaded_area += static_cast<LONGLONG>(platform::RectWidth(rect)) * platform::RectHeight(rect);
        DWORD affinity = 0;
        around_frame = around_frame && !Overlaps(rect, frame) && rect.left >= monitor.bounds.left &&
                       rect.top >= monitor.bounds.top && rect.right <= monitor.bounds.right &&
                       rect.bottom <= monitor.bounds.bottom && ::GetWindowDisplayAffinity(strip, &affinity) &&
                       affinity == 0x11 && (::GetWindowLongPtrW(strip, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
    }
    CHECK(around_frame);
    CHECK(shaded_area == static_cast<LONGLONG>(platform::RectWidth(monitor.bounds)) * platform::RectHeight(monitor.bounds) -
                             static_cast<LONGLONG>(platform::RectWidth(frame)) * platform::RectHeight(frame));

    overlay.BeginScrollshotCapture();
    CHECK(overlay.workspace().scrollshot_capturing() && !overlay.selector().enabled());
    for (int i = 0; i < 3; ++i) { CHECK(::IsWindowEnabled(::GetDlgItem(modes, 3000 + i)) == FALSE); }
    // The plane now has a hole exactly over the frame: the wheel and the
    // pointer go to the content there, and nowhere else.
    CHECK(::GetWindowRgn(dim, region) != ERROR);
    const auto in_plane = [&](LONG x, LONG y) {
        return ::PtInRegion(region, x - monitor.bounds.left, y - monitor.bounds.top) != FALSE;
    };
    CHECK(!in_plane(frame.left, frame.top) && !in_plane(frame.right - 1, frame.bottom - 1));
    CHECK(!in_plane((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2));
    CHECK(in_plane(frame.left - 1, frame.top) && in_plane(frame.right, frame.bottom - 1));
    CHECK(in_plane(frame.left, frame.top - 1) && in_plane(frame.left, frame.bottom));
    CHECK(in_plane(monitor.bounds.left + 5, monitor.bounds.top + 5));

    // Outside the frame a drag does nothing: no new selection, no cancellation.
    // A right-click leaves the way Esc does, through Discard, which asks the
    // user first when there is a scrolled image to lose.
    ::SendMessageW(dim, WM_LBUTTONDOWN, MK_LBUTTON, client(monitor.bounds.left + 50, monitor.bounds.top + 80));
    ::SendMessageW(dim, WM_MOUSEMOVE, MK_LBUTTON, client(monitor.bounds.left + 250, monitor.bounds.top + 180));
    ::SendMessageW(dim, WM_LBUTTONUP, 0, client(monitor.bounds.left + 250, monitor.bounds.top + 180));
    ::SendMessageW(dim, WM_RBUTTONUP, 0, client(monitor.bounds.left + 50, monitor.bounds.top + 80));
    ::SendMessageW(dim, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
    const RECT selection = overlay.selector().selection();
    CHECK(::EqualRect(&selection, &frame) && cancels == 0);
    CHECK(commands == (std::vector<ScrollshotCommand>{ScrollshotCommand::Discard}));
    commands.clear();

    // Shortcuts while the workspace has the focus.
    ::SendMessageW(dim, WM_KEYDOWN, VK_RETURN, 0);
    ::SendMessageW(dim, WM_KEYDOWN, VK_ESCAPE, 0);
    ::SendMessageW(dim, WM_KEYDOWN, 'S', 0);  // Without Ctrl: nothing.
    SetControlKey(true);
    ::SendMessageW(dim, WM_KEYDOWN, 'S', 0);
    ::SendMessageW(dim, WM_KEYDOWN, 'C', 0);
    SetControlKey(false);
    CHECK(commands == (std::vector<ScrollshotCommand>{ScrollshotCommand::Copy, ScrollshotCommand::Discard,
                                                      ScrollshotCommand::Save, ScrollshotCommand::Copy}));
    CHECK(cancels == 0);
    commands.clear();

    // A wheel turn that arrives here although the pointer is over the frame
    // means Windows routes the wheel by focus.
    ::SendMessageW(dim, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(frame.left + 50, frame.top + 50));
    CHECK(commands.size() == 1 && commands.back() == ScrollshotCommand::WheelBlocked);
    ::SendMessageW(dim, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(frame.left - 50, frame.top + 50));
    CHECK(commands.size() == 1 && wheel_delta == 0);
    commands.clear();

    overlay.StopScrollshotCapture();
    CHECK(overlay.workspace().scrollshot_locked() && !overlay.workspace().scrollshot_capturing());
    CHECK(::GetWindowRgn(dim, region) == ERROR);  // Closed again; the frame stays locked.
    shading = VisibleShading(monitor.bounds);  // Nothing is captured any more: one window again.
    CHECK(!overlay.frame_exposed() && shading.size() == 1 && ::GetWindowRect(shading[0], &shade_rect) &&
          ::EqualRect(&shade_rect, &monitor.bounds));
    ::SendMessageW(dim, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(commands.size() == 1 && commands.back() == ScrollshotCommand::Copy);

    // Edit: the same workspace becomes the viewport of the stitched image.
    const RECT viewport{monitor.bounds.left + 300, monitor.bounds.top + 60, monitor.bounds.left + 900, monitor.bounds.top + 760};
    overlay.BeginDocumentView(viewport);
    CHECK(overlay.document_view() && overlay.workspace().screenshot_fixed() && !overlay.workspace().scrollshot_locked());
    RECT shown = overlay.selector().selection();
    CHECK(::EqualRect(&shown, &viewport));
    HWND image_window = ::FindWindowW(L"EasnapCaptureImage", nullptr);
    while (image_window != nullptr && ::IsWindowVisible(image_window) == FALSE) {
        image_window = ::FindWindowExW(nullptr, image_window, L"EasnapCaptureImage", nullptr);
    }
    CHECK(image_window != nullptr);
    if (image_window != nullptr) {
        RECT plane{};
        ::GetWindowRect(image_window, &plane);
        CHECK(::EqualRect(&plane, &viewport));
    }
    const RECT zoomed{viewport.left + 100, viewport.top, viewport.left + 400, viewport.bottom};
    overlay.SetDocumentView(zoomed, 0.25f, 0.5f, L"640 × 9000   50%");
    shown = overlay.selector().selection();
    CHECK(::EqualRect(&shown, &zoomed) && overlay.document_label() == L"640 × 9000   50%");
    CHECK(overlay.document_top_fraction() == 0.25f && overlay.document_visible_fraction() == 0.5f);
    // The wheel over a fixed image goes to the editor: scroll, or zoom with Ctrl.
    ::SendMessageW(dim, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(zoomed.left + 20, zoomed.top + 20));
    CHECK(wheel_delta == -WHEEL_DELTA && !wheel_control);
    ::SendMessageW(dim, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), MAKELPARAM(zoomed.left + 20, zoomed.top + 20));
    CHECK(wheel_delta == 0 && wheel_control);

    ::DeleteObject(region);
    overlay.Destroy();
    CHECK(!overlay.document_view());
}

// --- Island and preview ---------------------------------------------------------------

void Controls() {
    const auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) { return; }
    const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
    const RECT frame{monitor.bounds.left + 400, monitor.bounds.top + 260, monitor.bounds.left + 1000, monitor.bounds.top + 660};

    using Action = ScrollshotControls::Action;
    std::vector<Action> actions;
    int wheel_blocked = 0;
    ScrollshotControls::Callbacks callbacks;
    callbacks.on_action = [&](Action action) { actions.push_back(action); };
    callbacks.on_wheel_blocked = [&] { ++wheel_blocked; };
    ScrollshotControls controls(std::move(callbacks));
    CHECK(!controls.Show(ScrollshotControls::View::Start, RECT{}, nullptr));

    // --- Start view: Start Scrollshot and help, nothing else ----------------
    CHECK(controls.Show(ScrollshotControls::View::Start, frame, nullptr));
    HWND island = controls.hwnd();
    CHECK(island != nullptr && controls.visible() && ::IsWindowVisible(island));
    std::vector<HWND> buttons = VisibleButtons(island);
    CHECK(buttons.size() == 2);
    if (buttons.size() == 2) {
        CHECK(WindowText(buttons[0]) == L"Start Scrollshot" && ::GetDlgCtrlID(buttons[0]) == IDOK);
        CHECK(WindowText(buttons[1]) == L"Tips for scrolling screenshots");
        ::SendMessageW(buttons[0], BM_CLICK, 0, 0);
        ::SendMessageW(buttons[1], BM_CLICK, 0, 0);
        CHECK(actions == (std::vector<Action>{Action::Start, Action::Help}));
        actions.clear();
        MSG enter{buttons[0], WM_KEYDOWN, VK_RETURN, 0};
        CHECK(controls.ProcessDialogMessage(enter));
        CHECK(actions == std::vector<Action>{Action::Start});
        actions.clear();
    }
    RECT island_rect{};
    ::GetWindowRect(island, &island_rect);
    CHECK(!Overlaps(island_rect, frame) && !controls.island_over_frame());
    CHECK(controls.preview_hwnd() != nullptr && ::IsWindowVisible(controls.preview_hwnd()) == FALSE);

    // The island follows the frame while it is being adjusted: it stays
    // outside the frame and comes back with it. With room to spare below the
    // frame it moves exactly as the frame does; a short monitor presses the
    // island against the edge of the work area or puts it on another side.
    RECT moved = frame;
    ::OffsetRect(&moved, -200, -120);
    controls.SetFrame(moved);
    RECT followed{};
    ::GetWindowRect(island, &followed);
    CHECK(!Overlaps(followed, moved) && !::EqualRect(&followed, &island_rect));
    if (monitor.work_area.bottom - frame.bottom >= 3 * platform::RectHeight(island_rect)) {
        CHECK(followed.left == island_rect.left - 200 && followed.top == island_rect.top - 120);
    }
    controls.SetFrame(frame);
    ::GetWindowRect(island, &followed);
    CHECK(::EqualRect(&followed, &island_rect));

    // --- Capture view: exactly Edit, Save, Discard and Copy ------------------
    CHECK(controls.Show(ScrollshotControls::View::Capture, frame, nullptr));
    CHECK(controls.hwnd() == island && controls.view() == ScrollshotControls::View::Capture);
    buttons = VisibleButtons(island);
    CHECK(buttons.size() == 4);
    if (buttons.size() == 4) {
        const std::array<const wchar_t*, 4> names{L"Edit", L"Save (Ctrl+S)", L"Discard (Esc)", L"Copy to clipboard (Enter)"};
        for (size_t i = 0; i < 4; ++i) {
            CHECK(WindowText(buttons[i]) == names[i]);
            CHECK(::IsWindowEnabled(buttons[i]) != FALSE);
            ::SendMessageW(buttons[i], BM_CLICK, 0, 0);
        }
        CHECK(actions == (std::vector<Action>{Action::Edit, Action::Save, Action::Discard, Action::Copy}));
        actions.clear();

        // Enter copies, Esc discards, Ctrl+S saves, Ctrl+C copies.
        SetControlKey(false);
        MSG enter{island, WM_KEYDOWN, VK_RETURN, 0};
        CHECK(controls.ProcessDialogMessage(enter));
        MSG escape{buttons[0], WM_KEYDOWN, VK_ESCAPE, 0};
        CHECK(controls.ProcessDialogMessage(escape));
        SetControlKey(true);
        MSG save{buttons[0], WM_KEYDOWN, 'S', 0};
        CHECK(controls.ProcessDialogMessage(save));
        MSG copy{island, WM_KEYDOWN, 'C', 0};
        CHECK(controls.ProcessDialogMessage(copy));
        SetControlKey(false);
        CHECK(actions == (std::vector<Action>{Action::Copy, Action::Discard, Action::Save, Action::Copy}));
        actions.clear();

        controls.SetBusy(true);
        for (HWND button : buttons) { CHECK(::IsWindowEnabled(button) == FALSE); }
        controls.SetBusy(false);
        for (HWND button : buttons) { CHECK(::IsWindowEnabled(button) != FALSE); }
    }
    // A message for another window is not this island's.
    MSG foreign{::GetDesktopWindow(), WM_KEYDOWN, VK_RETURN, 0};
    CHECK(!controls.ProcessDialogMessage(foreign) && actions.empty());

    controls.SetStatus(L"640 × 1200", L"Scroll the content inside the frame.", false);
    CHECK(WindowText(::GetDlgItem(island, 210)) == L"640 × 1200");
    CHECK(WindowText(::GetDlgItem(island, 211)) == L"Scroll the content inside the frame.");

    // The preview: beside the frame, never taking input.
    scrollshot::ScrollshotPreview preview;
    preview.width = ScrollshotControls::PreviewPixelWidth(frame);
    preview.height = 300;
    preview.total_height = 300;
    preview.version = 1;
    preview.pixels.assign(static_cast<size_t>(preview.width) * preview.height, 0xff4080c0u);
    controls.SetPreview(preview);
    HWND preview_window = controls.preview_hwnd();
    CHECK(::IsWindowVisible(preview_window));
    const LONG_PTR ex_style = ::GetWindowLongPtrW(preview_window, GWL_EXSTYLE);
    CHECK((ex_style & WS_EX_TRANSPARENT) != 0 && (ex_style & WS_EX_NOACTIVATE) != 0 && (ex_style & WS_EX_LAYERED) != 0);
    RECT preview_rect{};
    ::GetWindowRect(preview_window, &preview_rect);
    CHECK(::SendMessageW(preview_window, WM_NCHITTEST, 0, MAKELPARAM(preview_rect.left + 5, preview_rect.top + 5)) == HTTRANSPARENT);
    ::GetWindowRect(island, &island_rect);
    CHECK(!controls.island_over_frame() && !Overlaps(island_rect, frame));
    CHECK(!controls.preview_over_frame() && !Overlaps(preview_rect, frame) && !Overlaps(preview_rect, island_rect));
    CHECK(platform::RectWidth(preview_rect) > preview.width && platform::RectHeight(preview_rect) > preview.height);
    // It grows with the image up to the room beside the frame, then shows the end.
    preview.height = preview.total_height = 4000;
    preview.pixels.assign(static_cast<size_t>(preview.width) * preview.height, 0xff4080c0u);
    controls.SetPreview(preview);
    RECT grown{};
    ::GetWindowRect(preview_window, &grown);
    CHECK(grown.top == preview_rect.top && platform::RectHeight(grown) > platform::RectHeight(preview_rect));
    CHECK(grown.bottom <= monitor.work_area.bottom && !Overlaps(grown, frame));
    {
        // Painting the reduced image must not read outside its buffer.
        platform::BgraImage surface_pixels;
        surface_pixels.Reset(platform::RectWidth(grown), platform::RectHeight(grown));
        ::RedrawWindow(preview_window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    // The wheel reaching the island while the pointer is over the frame.
    ::SendMessageW(island, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(frame.left + 10, frame.top + 10));
    ::SendMessageW(island, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(island_rect.left + 2, island_rect.top + 2));
    CHECK(wheel_blocked == 1);

    // A frame that fills its monitor: both surfaces go to another monitor when
    // there is one, and only otherwise over the frame.
    controls.SetFrame(monitor.bounds);
    ::GetWindowRect(island, &island_rect);
    CHECK(controls.island_over_frame() == (monitors.size() == 1));
    CHECK(Overlaps(island_rect, monitor.bounds) == (monitors.size() == 1));
    controls.SetFrame(frame);

    controls.Hide();
    CHECK(!controls.visible() && ::IsWindowVisible(island) == FALSE && ::IsWindowVisible(preview_window) == FALSE);
    controls.Destroy();
    CHECK(controls.hwnd() == nullptr && ::IsWindow(island) == FALSE && ::IsWindow(preview_window) == FALSE);

    // Repeated cycles leave no window behind and do not accumulate GDI objects.
    const auto cycle = [&] {
        CHECK(controls.Show(ScrollshotControls::View::Start, frame, nullptr));
        CHECK(controls.Show(ScrollshotControls::View::Capture, frame, nullptr));
        controls.SetStatus(L"1 × 1", L"x", true);
        controls.SetPreview(preview);
        controls.Hide();
        controls.Destroy();
    };
    cycle();  // Warm caches that are created once.
    const int windows_before = ThreadWindowCount();
    const DWORD gdi_before = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user_before = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
    constexpr int kCycles = 40;
    for (int i = 0; i < kCycles; ++i) { cycle(); }
    const DWORD gdi_after = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user_after = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
    std::printf("Scrollshot surfaces, %d cycles: GDI %lu -> %lu, USER %lu -> %lu\n", kCycles, gdi_before, gdi_after,
                user_before, user_after);
    CHECK(ThreadWindowCount() == windows_before);
    // Windows adds up to six objects of its own about a second into the life
    // of a windowed thread and returns some later (measured over 300 cycles);
    // an object leaked per cycle would add forty.
    CHECK(gdi_after <= gdi_before + 8);
    CHECK(user_after <= user_before + 4);
}

// --- The tools island's Scrollshot button ---------------------------------------------

void ToolsIslandButton() {
    const auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) { return; }
    const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
    const RECT region{monitor.bounds.left + 300, monitor.bounds.top + 200, monitor.bounds.left + 620, monitor.bounds.top + 440};

    int requested = 0, acquisitions = 0;
    app::AppSettings settings;
    editor::ScreenshotEditor* self = nullptr;
    editor::ScreenshotEditor::Callbacks callbacks;
    callbacks.on_scrollshot = [&] { ++requested; };
    callbacks.on_prepare_image = [&] {
        ++acquisitions;
        platform::BgraImage image;
        image.Reset(320, 240);
        std::fill(image.pixels.begin(), image.pixels.end(), 0xff808080u);
        return self->SetImage(std::move(image));
    };
    editor::ScreenshotEditor editor(std::move(callbacks));
    self = &editor;
    CHECK(editor.Open(region, &settings, nullptr));

    // Command 8 of the island, between the tools and the edit commands.
    HWND scissors = ::GetDlgItem(editor.hwnd(), 2408);
    CHECK(scissors != nullptr && (::GetWindowLongPtrW(scissors, GWL_STYLE) & WS_VISIBLE) != 0);
    CHECK(WindowText(scissors) == L"Scrolling screenshot");
    RECT scissors_rect{}, last_tool{}, undo{};
    ::GetWindowRect(scissors, &scissors_rect);
    ::GetWindowRect(::GetDlgItem(editor.hwnd(), 2000 + static_cast<int>(editor::ToolId::Watermark)), &last_tool);
    ::GetWindowRect(::GetDlgItem(editor.hwnd(), 2401), &undo);
    CHECK(last_tool.right <= scissors_rect.left && scissors_rect.right <= undo.left);
    CHECK(scissors_rect.top == undo.top);  // One row: the island was widened for it.

    if (ui::CaptureExclusionSupported()) {
        CHECK(::IsWindowEnabled(scissors) != FALSE);
        // Pressing it asks for a Scrollshot and neither acquires nor fixes the image.
        ::SendMessageW(scissors, BM_CLICK, 0, 0);
        CHECK(requested == 1 && acquisitions == 0 && !editor.has_image());
        ::SendMessageW(scissors, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(4, 4));
        ::SendMessageW(scissors, WM_LBUTTONUP, 0, MAKELPARAM(4, 4));
        CHECK(acquisitions == 0 && !editor.has_image());
    } else {
        CHECK(::IsWindowEnabled(scissors) == FALSE);
    }

    // Any other control on the island fixes the screenshot on first use...
    HWND rectangle = ::GetDlgItem(editor.hwnd(), 2000 + static_cast<int>(editor::ToolId::Rectangle));
    ::SendMessageW(rectangle, BM_CLICK, 0, 0);
    CHECK(acquisitions == 1 && editor.has_image());
    // ...after which a scrolling capture is no longer offered.
    const int before = requested;
    CHECK(::IsWindowEnabled(scissors) == FALSE);
    ::SendMessageW(scissors, BM_CLICK, 0, 0);
    CHECK(requested == before);
    editor.Destroy();
}

// --- Editing a stitched image -----------------------------------------------------------

std::vector<uint32_t> Render(editor::ScreenshotEditor& editor, const RECT& bounds) {
    platform::BgraImage pixels;
    pixels.Reset(platform::RectWidth(bounds), platform::RectHeight(bounds));
    std::fill(pixels.pixels.begin(), pixels.pixels.end(), 0xff123456u);
    {
        platform::GdiImageSurface surface(pixels);
        CHECK(surface.valid());
        editor.RenderImage(surface.dc(), bounds);
        ::GdiFlush();
    }
    for (uint32_t& pixel : pixels.pixels) { pixel |= 0xff000000u; }
    return pixels.pixels;
}

void Draw(editor::ScreenshotEditor& editor, POINT from, POINT to) {
    editor.ImagePointer(WM_LBUTTONDOWN, from);
    editor.ImagePointer(WM_MOUSEMOVE, to);
    editor.ImagePointer(WM_LBUTTONUP, to);
}

void SelectTool(editor::ScreenshotEditor& editor, editor::ToolId tool) {
    HWND button = ::GetDlgItem(editor.hwnd(), 2000 + static_cast<int>(tool));
    CHECK(button != nullptr);
    ::SendMessageW(button, BM_CLICK, 0, 0);
}

bool Near(float a, float b, float tolerance = 0.02f) { return std::fabs(a - b) <= tolerance; }

void DocumentEditing() {
    const auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) { return; }
    const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
    const float scale = monitor.scale();
    // Taller than one bitmap or one composite pass may be: 8,400 rows.
    const Image document = MakeArticle(640, 8400, 77);
    const RECT area{monitor.bounds.left + 100, monitor.bounds.top + 60, monitor.bounds.left + 1000, monitor.bounds.top + 760};
    const LONG preferred_left = area.left + 50;

    app::AppSettings settings;
    int closed = 0, views = 0;
    bool exported = false;
    editor::ScreenshotEditor::Callbacks callbacks;
    callbacks.on_closed = [&](bool value) { ++closed; exported = value; };
    callbacks.on_view_changed = [&](const editor::ImageView&) { ++views; };
    editor::ScreenshotEditor editor(std::move(callbacks));

    platform::BgraImage empty;
    CHECK(!editor.OpenDocument(empty, area, preferred_left, &settings, nullptr));
    platform::BgraImage image = ToBgra(document);
    CHECK(!editor.OpenDocument(image, RECT{}, preferred_left, &settings, nullptr) && !image.empty());
    CHECK(editor.OpenDocument(image, area, preferred_left, &settings, nullptr));
    CHECK(image.empty());  // Taken, because the editor opened.
    CHECK(editor.document_mode() && editor.has_image() && views >= 1 && !editor.has_annotations());

    // Fitted to the width: narrower than the area, so shown one to one, at
    // the preferred edge, as tall as the area, from the top.
    editor::ImageView view = editor.view();
    CHECK(view.zoom == 1.0f && view.scroll_x == 0.0f && view.scroll_y == 0.0f);
    CHECK(view.viewport.left == preferred_left && view.viewport.right == preferred_left + 640);
    CHECK(view.viewport.top == area.top && view.viewport.bottom == area.bottom);
    const RECT region = editor.region();
    CHECK(::EqualRect(&region, &view.viewport));
    // The tools island belongs to the viewport: inside the work area, and
    // below the viewport on a monitor that has room to spare there.
    RECT island{};
    ::GetWindowRect(editor.hwnd(), &island);
    const bool room_below = monitor.work_area.bottom - view.viewport.bottom >= 2 * platform::RectHeight(island);
    CHECK(island.top >= monitor.work_area.top && island.bottom <= monitor.work_area.bottom);
    CHECK(!room_below || island.top >= view.viewport.bottom);
    // The Scrollshot button is not offered on a stitched image.
    CHECK(::IsWindowEnabled(::GetDlgItem(editor.hwnd(), 2408)) == FALSE);

    const int rows = static_cast<int>(platform::RectHeight(view.viewport));
    const auto shows = [&](int first_row) {
        const std::vector<uint32_t> pixels = Render(editor, editor.view().viewport);
        for (int y = 0; y < rows; ++y) {
            if (!std::equal(pixels.begin() + static_cast<std::ptrdiff_t>(y) * 640,
                            pixels.begin() + static_cast<std::ptrdiff_t>(y + 1) * 640, document.Row(first_row + y))) {
                return false;
            }
        }
        return true;
    };
    CHECK(shows(0));

    // Vertical scrolling: a wheel notch moves 96 DIP, the keys move pages and
    // jump to the ends; every position shows exactly its rows, across the
    // bands the picture is uploaded in.
    const POINT inside{view.viewport.left + 200, view.viewport.top + 300};
    editor.Wheel(-WHEEL_DELTA, false, false, inside);
    const float notch = std::round(96.0f * scale);
    CHECK(Near(editor.view().scroll_y, notch, 0.6f));
    CHECK(shows(static_cast<int>(editor.view().scroll_y)));
    editor.Wheel(WHEEL_DELTA * 3, false, false, inside);  // Above the top: clamped.
    CHECK(editor.view().scroll_y == 0.0f);
    SetControlKey(false);
    for (int page = 0; page < 6; ++page) { editor.Keyboard(WM_KEYDOWN, VK_NEXT, 0); }
    CHECK(Near(editor.view().scroll_y, 6.0f * std::round(static_cast<float>(rows) * 0.9f), 6.0f));
    CHECK(shows(static_cast<int>(editor.view().scroll_y)));  // Rows around 3,800: next to a band edge.
    editor.Keyboard(WM_KEYDOWN, VK_END, 0);
    CHECK(editor.view().scroll_y == static_cast<float>(8400 - rows));
    CHECK(shows(8400 - rows));
    editor.Keyboard(WM_KEYDOWN, VK_PRIOR, 0);
    CHECK(shows(static_cast<int>(editor.view().scroll_y)));
    editor.Keyboard(WM_KEYDOWN, VK_HOME, 0);
    CHECK(editor.view().scroll_y == 0.0f && shows(0));
    CHECK(Near(editor.view().visible_fraction(), static_cast<float>(rows) / 8400.0f, 0.001f));

    // --- Tools hit the image where the pointer is, at every position ---------
    SelectTool(editor, editor::ToolId::Rectangle);
    const auto last = [&]() -> const editor::Annotation& { return editor.document().annotations().back(); };
    const auto drawn_where_pointed = [&](POINT from, POINT to) {
        const editor::ImageView now = editor.view();
        const size_t count = editor.document().annotations().size();
        Draw(editor, from, to);
        if (editor.document().annotations().size() != count + 1) { return false; }
        const D2D1_POINT_2F a = now.ToImage(from), b = now.ToImage(to);
        return Near(last().points.front().x, a.x) && Near(last().points.front().y, a.y) &&
               Near(last().points.back().x, b.x) && Near(last().points.back().y, b.y);
    };
    view = editor.view();
    CHECK(drawn_where_pointed({view.viewport.left + 40, view.viewport.top + 50}, {view.viewport.left + 300, view.viewport.top + 200}));
    CHECK(last().points.front().x == 40.0f && last().points.front().y == 50.0f);  // One to one at the top.
    // Scrolled: a rectangle across the first composite band edge (row 2,048).
    editor.Wheel(static_cast<int>(-WHEEL_DELTA * (1900.0f / notch)), false, false, inside);
    view = editor.view();
    const float scrolled = view.scroll_y;
    CHECK(scrolled > 1700.0f && scrolled < 2000.0f);
    CHECK(drawn_where_pointed({view.viewport.left + 100, view.viewport.top + 60}, {view.viewport.left + 500, view.viewport.top + 400}));
    CHECK(Near(last().points.front().y, scrolled + 60.0f) && Near(last().points.back().y, scrolled + 400.0f));
    CHECK(last().points.front().y < 2048.0f && last().points.back().y > 2048.0f);
    // An arrow across the bitmap band edge (row 4,096).
    SelectTool(editor, editor::ToolId::Arrow);
    editor.Wheel(static_cast<int>(-WHEEL_DELTA * (2000.0f / notch)), false, false, inside);
    view = editor.view();
    CHECK(view.scroll_y < 4096.0f && view.scroll_y + static_cast<float>(rows) > 4096.0f);
    const LONG seam = view.viewport.top + static_cast<LONG>(4096.0f - view.scroll_y);
    CHECK(drawn_where_pointed({view.viewport.left + 80, seam - 120}, {view.viewport.left + 420, seam + 150}));

    // A blurred stroke across the third composite band edge (row 6,144).
    SelectTool(editor, editor::ToolId::Mosaic);
    editor.Wheel(static_cast<int>(-WHEEL_DELTA * (2000.0f / notch)), false, false, inside);
    view = editor.view();
    CHECK(view.scroll_y < 6144.0f && view.scroll_y + static_cast<float>(rows) > 6144.0f);
    const LONG third = view.viewport.top + static_cast<LONG>(6144.0f - view.scroll_y);
    CHECK(drawn_where_pointed({view.viewport.left + 120, third - 90}, {view.viewport.left + 480, third + 110}));
    CHECK(editor.has_annotations() && editor.document().annotations().size() == 4);

    // --- Export: the whole image, identical to one-pass compositing ----------
    // The document is composited in bands of 2,048 rows. Its first 8,000 rows,
    // opened as an ordinary screenshot, are composited in one pass; with the
    // same four shapes the two must agree in every pixel, band edges included.
    {
        const platform::BgraImage banded = editor.Composite();
        CHECK(banded.width == 640 && banded.height == 8400);
        const RECT whole{monitor.bounds.left, monitor.bounds.top, monitor.bounds.left + 640, monitor.bounds.top + 8000};
        editor::ScreenshotEditor* reference_self = nullptr;
        editor::ScreenshotEditor::Callbacks reference_callbacks;
        reference_callbacks.on_prepare_image = [&] { return reference_self->SetImage(ToBgra(document, 0, 8000)); };
        editor::ScreenshotEditor reference(std::move(reference_callbacks));
        reference_self = &reference;
        CHECK(reference.Open(whole, &settings, nullptr));
        for (const editor::Annotation& annotation : editor.document().annotations()) {
            const D2D1_POINT_2F a = annotation.points.front(), b = annotation.points.back();
            CHECK(a.x == std::floor(a.x) && a.y == std::floor(a.y) && b.x == std::floor(b.x) && b.y == std::floor(b.y));
            SelectTool(reference, annotation.tool);
            Draw(reference, {whole.left + static_cast<LONG>(a.x), whole.top + static_cast<LONG>(a.y)},
                 {whole.left + static_cast<LONG>(b.x), whole.top + static_cast<LONG>(b.y)});
        }
        CHECK(reference.document().annotations().size() == 4);
        const platform::BgraImage one_pass = reference.Composite();
        CHECK(one_pass.width == 640 && one_pass.height == 8000);
        int differing_rows = 0, first_difference = -1, changed_rows = 0;
        if (one_pass.height == 8000 && banded.height == 8400) {
            for (int y = 0; y < 8000; ++y) {
                if (!std::equal(one_pass.row(y), one_pass.row(y) + 640, banded.row(y))) {
                    ++differing_rows;
                    if (first_difference < 0) { first_difference = y; }
                }
                if (!std::equal(banded.row(y), banded.row(y) + 640, document.Row(y))) { ++changed_rows; }
            }
            // Past the reference and outside every shape: the stitched image itself.
            CHECK(std::equal(banded.row(8399), banded.row(8399) + 640, document.Row(8399)));
            CHECK(std::equal(banded.row(5000), banded.row(5000) + 640, document.Row(5000)));
        }
        if (first_difference >= 0) { std::fprintf(stderr, "  first differing row %d of %d\n", first_difference, differing_rows); }
        CHECK(differing_rows == 0);
        CHECK(changed_rows > 800);  // The shapes are in the export: 150 + 340 + 270 + 200 rows of them.
        reference.Destroy();
    }

    // Reduced to 50 %: the viewport narrows to the picture, tools still land
    // where the pointer is, and the hit distance is a screen distance. At the
    // end of the document, where nothing has been drawn yet.
    editor.Keyboard(WM_KEYDOWN, VK_END, 0);
    SelectTool(editor, editor::ToolId::Rectangle);
    editor.Wheel(-WHEEL_DELTA, true, false, inside);  // 1.0 -> 0.75
    editor.Wheel(-WHEEL_DELTA, true, false, inside);  // -> 0.67
    editor.Wheel(-WHEEL_DELTA, true, false, inside);  // -> 0.5
    view = editor.view();
    CHECK(view.zoom == 0.5f && platform::RectWidth(view.viewport) == 320);
    CHECK(view.viewport.left >= area.left && view.viewport.right <= area.right);
    CHECK(view.scroll_y + static_cast<float>(rows) / 0.5f >= 8399.0f);  // Still at the end.
    CHECK(drawn_where_pointed({view.viewport.left + 60, view.viewport.top + 100}, {view.viewport.left + 260, view.viewport.top + 300}));
    const D2D1_POINT_2F half_corner = last().points.front();
    {
        // 4 screen pixels from the edge is 8 image pixels at 50 %: still a hit.
        SelectTool(editor, editor::ToolId::Pointer);
        CHECK(editor.WantsPointer({view.viewport.left + 60 - 4, view.viewport.top + 200}));
        CHECK(!editor.WantsPointer({view.viewport.left + 60 - 12, view.viewport.top + 200}));
        const std::vector<uint32_t> reduced = Render(editor, view.viewport);
        CHECK(std::count(reduced.begin(), reduced.end(), 0xff123456u) == 0);  // The whole viewport is painted.
    }
    // Enlarged to 200 %: wider than the area, so it pans sideways with Shift.
    // In the untouched rows between the first two shapes.
    SetControlKey(true);
    editor.Keyboard(WM_KEYDOWN, '1', 0);  // Ctrl+1: one to one.
    SetControlKey(false);
    CHECK(editor.view().zoom == 1.0f);
    editor.Keyboard(WM_KEYDOWN, VK_HOME, 0);
    editor.Wheel(static_cast<int>(-WHEEL_DELTA * (600.0f / notch)), false, false, inside);
    CHECK(editor.view().scroll_y > 400.0f && editor.view().scroll_y < 800.0f);
    SetControlKey(true);
    editor.Keyboard(WM_KEYDOWN, VK_OEM_PLUS, 0);
    editor.Keyboard(WM_KEYDOWN, VK_OEM_PLUS, 0);
    editor.Keyboard(WM_KEYDOWN, VK_OEM_PLUS, 0);  // 1.25, 1.5, 2
    SetControlKey(false);
    view = editor.view();
    CHECK(view.zoom == 2.0f && platform::RectWidth(view.viewport) == platform::RectWidth(area));
    CHECK(view.scroll_y > 300.0f && view.scroll_y + static_cast<float>(rows) / 2.0f < 1700.0f);
    editor.Wheel(-WHEEL_DELTA, false, true, inside);
    view = editor.view();
    CHECK(view.scroll_x > 0.0f);
    SelectTool(editor, editor::ToolId::Rectangle);
    CHECK(drawn_where_pointed({view.viewport.left + 101, view.viewport.top + 51}, {view.viewport.left + 401, view.viewport.top + 351}));
    SelectTool(editor, editor::ToolId::Pointer);
    {
        // At 200 % the same 4 screen pixels are 2 image pixels: a hit; 12 are not.
        CHECK(editor.WantsPointer({view.viewport.left + 101 - 4, view.viewport.top + 200}));
        CHECK(!editor.WantsPointer({view.viewport.left + 101 - 12, view.viewport.top + 200}));
    }

    // The native text host sits on the text at the scale it is shown at.
    SelectTool(editor, editor::ToolId::Text);
    const POINT text_at{view.viewport.left + 301, view.viewport.top + 451};
    editor.ImagePointer(WM_LBUTTONDOWN, text_at);
    editor.ImagePointer(WM_LBUTTONUP, text_at);
    HWND host = ::FindWindowW(L"EasnapScreenshotEditor", L"Edit annotation text");
    CHECK(host != nullptr);
    if (host != nullptr) {
        RECT host_rect{};
        ::GetWindowRect(host, &host_rect);
        const LONG pad = ui::theme::Scaled(5, scale);
        CHECK(std::abs(host_rect.left + pad - text_at.x) <= 1 && std::abs(host_rect.top + pad - text_at.y) <= 1);
        HWND edit = ::GetWindow(host, GW_CHILD);
        CHECK(edit != nullptr);
        for (wchar_t character : std::wstring(L"Scroll")) { ::SendMessageW(edit, WM_CHAR, character, 0); }
        // The host grows with the text, scaled: twice the image-space width at 200 %.
        RECT typed{};
        ::GetWindowRect(host, &typed);
        const D2D1_RECT_F box = editor.document().annotations().back().bounds();
        CHECK(std::abs((typed.right - typed.left - 2 * pad) - static_cast<LONG>(std::ceil((box.right - box.left) * 2.0f))) <= 1);
        // Scrolling moves the host with its text.
        editor.Wheel(-WHEEL_DELTA, false, false, inside);
        RECT after{};
        ::GetWindowRect(host, &after);
        CHECK(after.top == typed.top - static_cast<LONG>(notch) && after.left == typed.left);
        editor.Wheel(WHEEL_DELTA, false, false, inside);
        SelectTool(editor, editor::ToolId::Pointer);  // Commits the text.
    }
    CHECK(editor.document().annotations().back().text == L"Scroll");
    const D2D1_POINT_2F text_point = editor.document().annotations().back().points.front();
    const D2D1_POINT_2F expected_text = view.ToImage(text_at);
    CHECK(Near(text_point.x, expected_text.x) && Near(text_point.y, expected_text.y));

    // Ctrl+0 returns to the fitted width.
    SetControlKey(true);
    editor.Keyboard(WM_KEYDOWN, '0', 0);
    SetControlKey(false);
    CHECK(editor.view().zoom == 1.0f && platform::RectWidth(editor.view().viewport) == 640);

    // What was drawn reduced and enlarged is in the export at image scale.
    {
        const platform::BgraImage final_image = editor.Composite();
        CHECK(final_image.width == 640 && final_image.height == 8400);
        if (final_image.height == 8400) {
            const int reduced_row = static_cast<int>(half_corner.y) + 100;   // Sides of the 50 % rectangle.
            const D2D1_RECT_F text_box = editor.document().annotations().back().bounds();
            const int text_row = static_cast<int>((text_box.top + text_box.bottom) / 2.0f);
            CHECK(!std::equal(final_image.row(reduced_row), final_image.row(reduced_row) + 640, document.Row(reduced_row)));
            CHECK(!std::equal(final_image.row(text_row), final_image.row(text_row) + 640, document.Row(text_row)));
            CHECK(std::equal(final_image.row(5000), final_image.row(5000) + 640, document.Row(5000)));
        }
    }
    // The unannotated picture can be handed back, for a caller that must
    // close the editor without losing it.
    platform::BgraImage taken = editor.TakeImage();
    CHECK(taken.width == 640 && taken.height == 8400 && !editor.has_image());
    CHECK(std::equal(taken.row(4096), taken.row(4096) + 640, document.Row(4096)));

    editor.Close(false);
    CHECK(closed == 1 && !exported);
    editor.Destroy();
    CHECK(!editor.document_mode());

    // An image wider than the area is fitted to its width.
    const Image wide_source = MakeArticle(1800, 600, 78);
    platform::BgraImage wide = ToBgra(wide_source);
    editor::ScreenshotEditor wide_editor({});
    CHECK(wide_editor.OpenDocument(wide, area, area.left, &settings, nullptr));
    CHECK(Near(wide_editor.view().zoom, 0.5f, 0.001f) && platform::RectWidth(wide_editor.view().viewport) == 900);
    CHECK(platform::RectHeight(wide_editor.view().viewport) == 300);  // Shorter than the area: centred.
    CHECK(wide_editor.view().viewport.top == area.top + (700 - 300) / 2);
    wide_editor.Destroy();
}

// --- Editing a very tall image: what one pointer move repaints --------------------------
//
// The editor on a stitched image 1,920 pixels wide, painted the way the
// workspace paints it: what the editor invalidates is collected, and after
// every input event its bounding rectangle is repainted, again while a paint
// left something invalid, as WM_PAINT would bring while no input waits. Five
// gestures of 120 moves each, at the fitted width, at 67 % and at 10 %, and a
// pixelating brush. Times are printed as MEASURE lines and never asserted,
// since they depend on the machine. Asserted is what holds everywhere: in the
// middle of each gesture and after it the partly repainted picture equals a
// full repaint (COMPARE lines), a gesture that ends where it began leaves the
// export as it was, moving a shape or a spotlight repaints a small part of
// the viewport, and switching the spotlight darkening on or off repaints all
// of it.

using Clock = std::chrono::steady_clock;

constexpr int kEditMoves = 120;
constexpr uint32_t kPlaneBackground = 0xff123456u;

double Milliseconds(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

double Quantile(std::vector<double> values, double fraction) {
    if (values.empty()) { return 0.0; }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5);
    return values[std::min(values.size() - 1, index)];
}

uint64_t Area(const RECT& rect) {
    return platform::IsEmptyRect(rect) ? 0 : static_cast<uint64_t>(platform::RectWidth(rect)) * platform::RectHeight(rect);
}

/// FNV-1a over the pixel values: equal pictures have equal fingerprints.
uint64_t Fingerprint(const platform::BgraImage& image) {
    uint64_t hash = 14695981039346656037ull;
    for (const uint32_t pixel : image.pixels) { hash = (hash ^ pixel) * 1099511628211ull; }
    return hash;
}

/// Out and back: 60 steps away from the start, then 60 steps back onto it.
int OutAndBack(int move) { return move < kEditMoves / 2 ? move + 1 : kEditMoves - 1 - move; }

/// The editor and what it has painted into its image plane.
class PaintedEditor {
public:
    PaintedEditor() : editor_(MakeCallbacks()) {}
    ~PaintedEditor() {
        plane_.reset();
        editor_.Destroy();
    }
    PaintedEditor(const PaintedEditor&) = delete;
    PaintedEditor& operator=(const PaintedEditor&) = delete;

    editor::ScreenshotEditor& editor() { return editor_; }
    [[nodiscard]] const RECT& viewport() const { return viewport_; }

    /// The image plane appears for the current view and is painted whole.
    void Show() {
        plane_.reset();
        viewport_ = editor_.view().viewport;
        surface_.Reset(platform::RectWidth(viewport_), platform::RectHeight(viewport_));
        std::fill(surface_.pixels.begin(), surface_.pixels.end(), kPlaneBackground);
        plane_ = std::make_unique<platform::GdiImageSurface>(surface_);
        CHECK(plane_->valid());
        damage_ = RECT{};
        editor_.RenderImage(plane_->dc(), viewport_);
        ::GdiFlush();
    }

    /// One WM_PAINT: the bounding rectangle of everything invalidated since
    /// the last one is repainted. Returns its area in desktop pixels.
    uint64_t Paint() {
        RECT local = damage_;
        damage_ = RECT{};
        ::OffsetRect(&local, -viewport_.left, -viewport_.top);
        local = platform::IntersectRects(local, RECT{0, 0, platform::RectWidth(viewport_), platform::RectHeight(viewport_)});
        if (platform::IsEmptyRect(local) || !plane_) { return 0; }
        editor_.RenderImage(plane_->dc(), viewport_, &local);
        ::GdiFlush();
        return Area(local);
    }

    struct Settled {
        uint64_t area{};  ///< Desktop pixels repainted, summed over the paints.
        int paints{};     ///< Paints until nothing was left invalid.
    };

    /// The paints a message loop delivers while no input waits: one, and
    /// another while the last one left something invalid. A paint that kept
    /// invalidating would be a loop; four are allowed.
    Settled Settle() {
        Settled settled;
        while (!platform::IsEmptyRect(damage_) && settled.paints < 4) {
            settled.area += Paint();
            ++settled.paints;
        }
        CHECK(platform::IsEmptyRect(damage_));
        return settled;
    }

    /// A pointer event and the repaints it asks for.
    void Pointer(UINT message, POINT point) {
        editor_.ImagePointer(message, point);
        Settle();
    }

    struct Comparison {
        int64_t differing{};  ///< Pixels that differ from a full repaint,
        int64_t slight{};     ///< no channel of which by more than 2 levels,
        int64_t stale{};      ///< or one channel by more than kDrawingVariance.
        int largest{};        ///< The largest difference in one channel.
        RECT where{};         ///< Bounding box of the differing pixels.
        int64_t undecided{};  ///< Pixels whose full repaint depends on what was under them.
        uint64_t repaint{};   ///< Fingerprint of the full repaint.
    };

    /// Direct2D resamples the picture and antialiases edges relative to the
    /// rectangle a paint binds, so at a zoom where image pixels fall between
    /// screen pixels a partial repaint differs from a full one by a level or
    /// two, and an antialiased edge by up to seven (measured by drawing an
    /// unchanged view in strips). A pixel left over from an earlier frame (a
    /// shape, a selection handle, the darkening) differs by far more.
    static constexpr int kDrawingVariance = 16;

    /// The image plane against a full repaint of the same view. A pixel whose
    /// full repaint depends on what was under it, such as an antialiased edge
    /// where two bands of the base bitmap meet in a reduced view, cannot be
    /// compared and is counted as undecided.
    Comparison CompareWithFullRepaint() {
        plane_.reset();  // Hands the painted pixels back to `surface_`.
        const platform::BgraImage first = FullRepaint(kPlaneBackground);
        const platform::BgraImage second = FullRepaint(0xffedcba9u);
        Comparison result;
        result.repaint = Fingerprint(first);
        const auto width = static_cast<size_t>(surface_.width);
        for (size_t i = 0; i < surface_.pixels.size() && i < first.pixels.size(); ++i) {
            if (first.pixels[i] != second.pixels[i]) {
                ++result.undecided;
            } else if (surface_.pixels[i] != first.pixels[i]) {
                ++result.differing;
                int largest = 0;
                for (int shift : {0, 8, 16}) {
                    largest = std::max(largest, std::abs(static_cast<int>((surface_.pixels[i] >> shift) & 255u) -
                                                         static_cast<int>((first.pixels[i] >> shift) & 255u)));
                }
                result.largest = std::max(result.largest, largest);
                if (largest <= 2) { ++result.slight; }
                if (largest > kDrawingVariance) { ++result.stale; }
                const LONG x = static_cast<LONG>(i % width), y = static_cast<LONG>(i / width);
                const RECT pixel{x, y, x + 1, y + 1};
                if (platform::IsEmptyRect(result.where)) {
                    result.where = pixel;
                } else {
                    ::UnionRect(&result.where, &result.where, &pixel);
                }
            }
        }
        plane_ = std::make_unique<platform::GdiImageSurface>(surface_);
        CHECK(plane_->valid());
        return result;
    }

private:
    platform::BgraImage FullRepaint(uint32_t background) {
        platform::BgraImage pixels;
        pixels.Reset(platform::RectWidth(viewport_), platform::RectHeight(viewport_));
        std::fill(pixels.pixels.begin(), pixels.pixels.end(), background);
        {
            platform::GdiImageSurface surface(pixels);
            CHECK(surface.valid());
            editor_.RenderImage(surface.dc(), viewport_);
            ::GdiFlush();
        }
        return pixels;
    }

    editor::ScreenshotEditor::Callbacks MakeCallbacks() {
        editor::ScreenshotEditor::Callbacks callbacks;
        callbacks.on_image_invalidated = [this](const RECT& damage) {
            if (platform::IsEmptyRect(damage)) { return; }
            if (platform::IsEmptyRect(damage_)) {
                damage_ = damage;
            } else {
                ::UnionRect(&damage_, &damage_, &damage);
            }
        };
        return callbacks;
    }

    RECT viewport_{};
    RECT damage_{};
    platform::BgraImage surface_;
    std::unique_ptr<platform::GdiImageSurface> plane_;
    editor::ScreenshotEditor editor_;
};

struct MoveSeries {
    std::vector<double> input;    ///< ImagePointer(WM_MOUSEMOVE), milliseconds.
    std::vector<double> paint;    ///< The repaints that follow it, milliseconds.
    std::vector<double> damaged;  ///< Desktop pixels those repaints covered.
    std::vector<double> later;    ///< Repaints after the first: what a paint invalidated.
    PaintedEditor::Settled press;  ///< The repaints after the button went down,
    double press_input{};          ///< with its input
    double press_paint{};          ///< and repaint time in milliseconds.
};

/// Presses at `start`, moves the pointer 120 times along `point_at`, each
/// move followed by the repaints it asks for, and releases at the last point.
/// `after_move` runs after the press (move -1) and each move's repaints,
/// before the release.
MoveSeries Gesture(PaintedEditor& painted, POINT start, const std::function<POINT(int)>& point_at,
                   const std::function<void(int)>& after_move = {}) {
    MoveSeries series;
    const auto pressed = Clock::now();
    painted.editor().ImagePointer(WM_LBUTTONDOWN, start);
    const auto press_handled = Clock::now();
    series.press = painted.Settle();
    series.press_input = Milliseconds(pressed, press_handled);
    series.press_paint = Milliseconds(press_handled, Clock::now());
    if (after_move) { after_move(-1); }
    POINT point = start;
    for (int move = 0; move < kEditMoves; ++move) {
        point = point_at(move);
        const auto began = Clock::now();
        painted.editor().ImagePointer(WM_MOUSEMOVE, point);
        const auto handled = Clock::now();
        const PaintedEditor::Settled settled = painted.Settle();
        const auto repainted = Clock::now();
        series.input.push_back(Milliseconds(began, handled));
        series.paint.push_back(Milliseconds(handled, repainted));
        series.damaged.push_back(static_cast<double>(settled.area));
        series.later.push_back(static_cast<double>(std::max(0, settled.paints - 1)));
        if (after_move) { after_move(move); }
    }
    painted.Pointer(WM_LBUTTONUP, point);
    return series;
}

/// The moves of `series` from `first` on.
MoveSeries From(const MoveSeries& series, size_t first) {
    MoveSeries rest;
    const auto from = static_cast<std::ptrdiff_t>(first);
    rest.input.assign(series.input.begin() + from, series.input.end());
    rest.paint.assign(series.paint.begin() + from, series.paint.end());
    rest.damaged.assign(series.damaged.begin() + from, series.damaged.end());
    rest.later.assign(series.later.begin() + from, series.later.end());
    return rest;
}

/// One MEASURE line. The fingerprints of the export and of a full repaint
/// let two builds be compared on the same machine.
void Measure(const std::string& pass, const char* gesture, const MoveSeries& series, uint64_t viewport_area,
             uint64_t export_fingerprint, const PaintedEditor::Comparison& comparison) {
    const double damaged = Quantile(series.damaged, 0.5);
    double later = 0;
    for (const double paints : series.later) { later += paints; }
    std::printf("MEASURE edit %s, %s, %zu moves: input median %.3f ms p95 %.3f ms, paint median %.3f ms p95 %.3f ms, "
                "repainted median %.0f px (%.1f%% of the viewport) max %.0f px, %.0f follow-up paints; export %016llx, "
                "repaint %016llx, %lld pixels not comparable\n",
                pass.c_str(), gesture, series.input.size(), Quantile(series.input, 0.5), Quantile(series.input, 0.95),
                Quantile(series.paint, 0.5), Quantile(series.paint, 0.95), damaged,
                100.0 * damaged / static_cast<double>(viewport_area), Quantile(series.damaged, 1.0), later,
                static_cast<unsigned long long>(export_fingerprint),
                static_cast<unsigned long long>(comparison.repaint), static_cast<long long>(comparison.undecided));
}

void ChooseOption(editor::ScreenshotEditor& editor, int identifier) {
    HWND button = ::GetDlgItem(editor.context_hwnd(), identifier);
    CHECK(button != nullptr);
    ::SendMessageW(button, BM_CLICK, 0, 0);
}

/// The gestures on a fresh editor at `target_zoom`, or fitted to the width when it is 0.
void EditPass(const platform::BgraImage& source, const RECT& area, float target_zoom) {
    app::AppSettings settings;
    PaintedEditor painted;
    editor::ScreenshotEditor& editor = painted.editor();
    platform::BgraImage image = source;
    CHECK(editor.OpenDocument(image, area, area.left, &settings, nullptr));
    if (!editor.has_image()) { return; }
    if (target_zoom > 0.0f) {
        // Ctrl+wheel around the top-left corner keeps the top of the image in
        // view. The fitted zoom lies above the target on a 1920-pixel display
        // and below it on a small one (1024 pixels on the hosted runner), so
        // the ladder is climbed in whichever direction reaches it.
        for (int step = 0; step < 16 && editor.view().zoom != target_zoom; ++step) {
            const RECT shown = editor.view().viewport;
            const int delta = editor.view().zoom > target_zoom ? -WHEEL_DELTA : WHEEL_DELTA;
            editor.Wheel(delta, true, false, POINT{shown.left, shown.top});
        }
        CHECK(editor.view().zoom == target_zoom && editor.view().scroll_y == 0.0f);
    }
    painted.Show();
    const RECT viewport = painted.viewport();
    const uint64_t viewport_area = Area(viewport);
    const LONG width = platform::RectWidth(viewport), height = platform::RectHeight(viewport);
    const float zoom = editor.view().zoom;
    char label[64]{};
    std::snprintf(label, sizeof(label), "%dx%d at %.1f%%", source.width, source.height, 100.0 * zoom);
    const std::string pass = label;
    std::printf("Edit pass %s: viewport %ldx%ld\n", label, width, height);

    // A point whose image position is not on the pixel grid, where one
    // exists: an anchor there makes a spotlight of zero size cover a pixel.
    // At 10 % every screen pixel is ten image pixels: none exists.
    const auto off_grid = [&](float x, float y) {
        const POINT first{viewport.left + static_cast<LONG>(x * static_cast<float>(width)),
                          viewport.top + static_cast<LONG>(y * static_cast<float>(height))};
        const auto fraction = [](float value) { return std::fabs(value - std::round(value)); };
        for (LONG dy = 0; dy < 6; ++dy) {
            for (LONG dx = 0; dx < 6; ++dx) {
                const POINT point{first.x + dx, first.y + dy};
                const D2D1_POINT_2F image_point = editor.view().ToImage(point);
                if (fraction(image_point.x) > 0.05f && fraction(image_point.y) > 0.05f) { return point; }
            }
        }
        return first;
    };
    const auto on_grid = [&](POINT point) {
        const D2D1_POINT_2F image_point = editor.view().ToImage(point);
        return std::fabs(image_point.x - std::round(image_point.x)) <= 0.05f ||
               std::fabs(image_point.y - std::round(image_point.y)) <= 0.05f;
    };
    // About a 480th of the viewport per move, at least one pixel.
    const POINT step{std::max<LONG>(1, width / 480), std::max<LONG>(1, height / 480)};
    const auto along = [](POINT start, POINT direction, int extra) {
        return [=](int move) {
            const int t = OutAndBack(move) + extra;
            return POINT{start.x + direction.x * t, start.y + direction.y * t};
        };
    };
    const auto unchanged = [&](const platform::BgraImage& before, const char* what) {
        const platform::BgraImage after = editor.Composite();
        const bool same = after.pixels == before.pixels;
        if (!same) { std::fprintf(stderr, "  %s: %s changed the export\n", label, what); }
        CHECK(same);
        return Fingerprint(after);
    };
    // At 10 % every screen pixel samples halfway between image pixels, and a
    // partial repaint equals a full one exactly: there nothing may differ but
    // watermark glyphs, whose rotated outlines vary with the drawing origin.
    // Elsewhere nothing may differ by more than Direct2D's drawing variance.
    const bool exact_zoom = zoom == editor::kMinimumZoom;
    bool watermark = false;
    const auto compare = [&](const char* what) {
        const PaintedEditor::Comparison comparison = painted.CompareWithFullRepaint();
        const bool exact = exact_zoom && !watermark;
        const bool same = exact ? comparison.differing == 0 : comparison.stale == 0;
        std::printf("COMPARE %s, %s: %lld pixels differ (%lld by at most 2 levels, largest %d, %lld by more than %d)%s\n",
                    label, what, static_cast<long long>(comparison.differing), static_cast<long long>(comparison.slight),
                    comparison.largest, static_cast<long long>(comparison.stale), PaintedEditor::kDrawingVariance,
                    exact ? ", none may" : "");
        if (!same) {
            std::fprintf(stderr, "  %s: %s, %lld pixels differ from a full repaint in %ld,%ld-%ld,%ld\n", label, what,
                         static_cast<long long>(comparison.differing), comparison.where.left, comparison.where.top,
                         comparison.where.right, comparison.where.bottom);
        }
        CHECK(same);
        return comparison;
    };
    // Before the release, whose full repaint would hide what partial repaints left.
    const auto compare_at = [&](int checked_move, const char* what) {
        return [=, &compare](int move) {
            if (move == checked_move) { compare(what); }
        };
    };
    // A move repaints the annotation where it was and where it is, not the
    // viewport: with these sizes, well under a tenth of it.
    const auto local = [&](const MoveSeries& series, const char* what) {
        const double most = Quantile(series.damaged, 1.0);
        const bool contained = most < 0.1 * static_cast<double>(viewport_area);
        if (!contained) { std::fprintf(stderr, "  %s: %s repainted %.0f of %llu pixels in one move\n", label, what, most, static_cast<unsigned long long>(viewport_area)); }
        CHECK(contained);
    };

    // 1. A rectangle dragged by its outline.
    SelectTool(editor, editor::ToolId::Rectangle);
    painted.Settle();
    const POINT rectangle_from = off_grid(0.10f, 0.20f), rectangle_to = off_grid(0.25f, 0.30f);
    painted.Pointer(WM_LBUTTONDOWN, rectangle_from);
    painted.Pointer(WM_MOUSEMOVE, rectangle_to);
    painted.Pointer(WM_LBUTTONUP, rectangle_to);
    SelectTool(editor, editor::ToolId::Pointer);
    painted.Settle();
    const POINT rectangle_edge{(rectangle_from.x + rectangle_to.x) / 2, rectangle_from.y};
    platform::BgraImage before = editor.Composite();
    const MoveSeries rectangle = Gesture(painted, rectangle_edge, along(rectangle_edge, step, 0),
                                         compare_at(59, "the 60th move of a rectangle"));
    uint64_t exported = unchanged(before, "dragging a rectangle back to its place");
    local(rectangle, "dragging a rectangle");
    Measure(pass, "drag rectangle", rectangle, viewport_area, exported, compare("after dragging a rectangle"));

    // 2. A spotlight drawn from an anchor, released small and undone. Off the
    //    pixel grid the press already darkens the picture; on it, the first
    //    move does.
    before = editor.Composite();
    SelectTool(editor, editor::ToolId::Spotlight);
    painted.Settle();
    const POINT anchor = off_grid(0.55f, 0.15f);
    // Off the pixel grid the zero-sized spotlight of the press already covers
    // a pixel and switches the darkening on; on it, the first move does.
    const bool at_press = !on_grid(anchor);
    const MoveSeries preview = Gesture(
        painted, anchor, along(anchor, step, 1),
        [&](int move) {
            if (move == (at_press ? -1 : 0)) { compare("the first spotlight frame"); }
            if (move == 59) { compare("the 60th move of a spotlight preview"); }
        });
    CHECK(editor.document().annotations().size() == 2 && editor.document().annotations().back().tool == editor::ToolId::Spotlight);
    SetControlKey(true);
    editor.Keyboard(WM_KEYDOWN, 'Z', 0);
    SetControlKey(false);
    painted.Settle();
    exported = unchanged(before, "a spotlight drawn and undone");
    // The press repaints the viewport anyway: it clears the selection. A
    // move that switches the darkening on repaints what it invalidated, and
    // the paint that recomputes the darkening invalidates the rest.
    CHECK(preview.press.area >= viewport_area);
    if (!at_press) { CHECK(preview.damaged.front() >= static_cast<double>(viewport_area) && preview.later.front() >= 1.0); }
    std::printf("MEASURE edit %s, spotlight preview, first frame (%s): %.3f ms (input %.3f ms, paint %.3f ms), "
                "repainted %.0f px in %d paints\n",
                label, at_press ? "the press, anchor off the pixel grid" : "the first move, anchor on the pixel grid",
                at_press ? preview.press_input + preview.press_paint : preview.input.front() + preview.paint.front(),
                at_press ? preview.press_input : preview.input.front(), at_press ? preview.press_paint : preview.paint.front(),
                at_press ? static_cast<double>(preview.press.area) : preview.damaged.front(),
                at_press ? preview.press.paints : 1 + static_cast<int>(preview.later.front()));
    // Later frames keep the darkening: only the spotlight's old and new extent.
    const MoveSeries later_frames = at_press ? preview : From(preview, 1);
    local(later_frames, "a spotlight preview");
    Measure(pass, at_press ? "spotlight preview after the press" : "spotlight preview after the first frame", later_frames,
            viewport_area, exported, compare("after a spotlight preview"));

    // A preview cancelled by the loss of the mouse takes the darkening back
    // from the whole picture.
    painted.Pointer(WM_LBUTTONDOWN, anchor);
    for (int move = 0; move < 10; ++move) { painted.Pointer(WM_MOUSEMOVE, along(anchor, step, 1)(move)); }
    editor.ImagePointer(WM_CAPTURECHANGED, anchor);
    CHECK(painted.Settle().area >= viewport_area);
    CHECK(editor.document().annotations().size() == 1);
    unchanged(before, "a cancelled spotlight preview");
    compare("after a cancelled spotlight preview");

    // 3. A committed spotlight dragged by its outline.
    SelectTool(editor, editor::ToolId::Spotlight);
    painted.Settle();
    const POINT spot_from = off_grid(0.55f, 0.45f), spot_to = off_grid(0.75f, 0.55f);
    painted.Pointer(WM_LBUTTONDOWN, spot_from);
    painted.Pointer(WM_MOUSEMOVE, spot_to);
    painted.Pointer(WM_LBUTTONUP, spot_to);
    SelectTool(editor, editor::ToolId::Pointer);
    painted.Settle();
    const POINT spot_edge{(spot_from.x + spot_to.x) / 2, spot_from.y};
    before = editor.Composite();
    const MoveSeries spotlight = Gesture(painted, spot_edge, along(spot_edge, step, 0),
                                         compare_at(59, "the 60th move of a spotlight"));
    exported = unchanged(before, "dragging a spotlight back to its place");
    local(spotlight, "dragging a spotlight");
    Measure(pass, "drag spotlight", spotlight, viewport_area, exported, compare("after dragging a spotlight"));

    // 4. A pixelated region dragged along a blur that covers three quarters
    //    of the image's height, from its first row. The blur reads what the
    //    pixelation writes, so every move recomputes the whole blur.
    SelectTool(editor, editor::ToolId::Mosaic);
    ChooseOption(editor, 2521);  // Region
    ChooseOption(editor, 2510);  // Blur
    painted.Settle();
    const POINT blur_from{off_grid(0.62f, 0.0f).x, viewport.top};
    const POINT blur_to{off_grid(0.78f, 0.0f).x,
                        viewport.top + static_cast<LONG>(0.75f * static_cast<float>(source.height) * zoom)};
    painted.Pointer(WM_LBUTTONDOWN, blur_from);
    painted.Pointer(WM_MOUSEMOVE, blur_to);
    painted.Pointer(WM_LBUTTONUP, blur_to);
    ChooseOption(editor, 2511);  // Pixelate
    painted.Settle();
    // 20 image pixels left of the blur: inside what the blur reads.
    const POINT mosaic_from = off_grid(0.40f, 0.30f);
    const POINT mosaic_to{blur_from.x - static_cast<LONG>(std::ceil(20.0f * zoom)), off_grid(0.0f, 0.40f).y};
    painted.Pointer(WM_LBUTTONDOWN, mosaic_from);
    painted.Pointer(WM_MOUSEMOVE, mosaic_to);
    painted.Pointer(WM_LBUTTONUP, mosaic_to);
    SelectTool(editor, editor::ToolId::Pointer);
    painted.Settle();
    CHECK(editor.document().annotations().size() == 4);
    const POINT mosaic_edge{(mosaic_from.x + mosaic_to.x) / 2, mosaic_from.y};
    before = editor.Composite();
    const MoveSeries mosaic = Gesture(painted, mosaic_edge, along(mosaic_edge, POINT{0, step.y}, 0),
                                      compare_at(59, "the 60th move of a pixelated region"));
    exported = unchanged(before, "dragging a pixelated region back to its place");
    Measure(pass, "drag pixelation beside a tall blur", mosaic, viewport_area, exported,
            compare("after dragging a pixelated region"));

    // 5. The rectangle again, with a watermark over the whole picture.
    SelectTool(editor, editor::ToolId::Watermark);
    ::SetWindowTextW(::GetDlgItem(editor.context_hwnd(), 2604), L"Easnap review");
    CHECK(editor.document().annotations().size() == 5 && editor.document().annotations().back().tool == editor::ToolId::Watermark);
    watermark = true;
    SelectTool(editor, editor::ToolId::Pointer);
    painted.Settle();
    before = editor.Composite();
    const MoveSeries watermarked = Gesture(painted, rectangle_edge, along(rectangle_edge, step, 0),
                                           compare_at(59, "the 60th move of a rectangle over a watermark"));
    exported = unchanged(before, "dragging a rectangle over a watermark back to its place");
    local(watermarked, "dragging a rectangle over a watermark");
    Measure(pass, "drag rectangle over a watermark", watermarked, viewport_area, exported,
            compare("after dragging a rectangle over a watermark"));
    CHECK(editor.document().annotations().size() == 5);

    // A pixelating brush drawn up and to the left: every dab moves the top-left
    // corner its blocks are aligned to, so the whole stroke changes, not only
    // the last segment. Checked before the stroke ends, then cancelled.
    SelectTool(editor, editor::ToolId::Mosaic);
    ChooseOption(editor, 2520);  // Brush; the effect is still Pixelate.
    painted.Settle();
    const POINT brush_from = off_grid(0.30f, 0.62f);
    painted.Pointer(WM_LBUTTONDOWN, brush_from);
    for (LONG move = 1; move <= 30; ++move) {
        painted.Pointer(WM_MOUSEMOVE, POINT{brush_from.x - step.x * move, brush_from.y - step.y * move});
    }
    compare("the 30th move of a pixelating brush");
    editor.ImagePointer(WM_CAPTURECHANGED, brush_from);
    painted.Settle();
    compare("after a cancelled pixelating brush");
    CHECK(editor.document().annotations().size() == 5);
}

/// `--edit-performance [rows]`: the gestures on an image of `rows` rows.
int EditPerformance(int rows) {
    const auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) {
        std::printf("No display to place the editor on: not run.\n");
        return 77;
    }
    const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
    // The area the coordinator gives a stitched image: the work area less a
    // 12 DIP margin, and 176 DIP below for the tool islands.
    const float scale = monitor.scale();
    const LONG margin = ui::theme::Scaled(12, scale);
    const RECT area{monitor.work_area.left + margin, monitor.work_area.top + margin, monitor.work_area.right - margin,
                    monitor.work_area.bottom - ui::theme::Scaled(176, scale)};
    std::printf("Monitor %ldx%ld at %.0f%%, area %ldx%ld\n", platform::RectWidth(monitor.bounds),
                platform::RectHeight(monitor.bounds), 100.0 * scale, platform::RectWidth(area), platform::RectHeight(area));
    const platform::BgraImage source = ToBgra(MakeArticle(1920, rows, 91));
    // Fitted to the width, a step of the zoom ladder at which image pixels fall
    // between screen pixels, and the smallest step.
    for (const float zoom : {0.0f, 0.67f, editor::kMinimumZoom}) { EditPass(source, area, zoom); }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Assertions name controls by their English text, whatever Windows speaks.
    easnap::strings::SetPreference(easnap::strings::LanguagePreference::English);
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc > 1 && std::string_view(argv[1]) == "--edit-performance") {
        // Up to the 48-million-pixel limit of a stitched image.
        const int rows = argc > 2 ? std::atoi(argv[2]) : 8400;
        CHECK(rows >= 1000 && rows <= 25000);
        const int result = rows >= 1000 && rows <= 25000 ? EditPerformance(rows) : 1;
        platform::GraphicsEnvironment::Get().Shutdown();
        ::CoUninitialize();
        if (result == 77) { return 77; }
        std::printf("scrollshot edit performance: %d passed, %d failed\n", passed, failed);
        return failed || result != 0 ? 1 : 0;
    }
    Policy();
    FrameAroundContent();
    OverlayPhases();
    RecordingUnavailable();
    Controls();
    ToolsIslandButton();
    DocumentEditing();
    platform::GraphicsEnvironment::Get().Shutdown();
    ::CoUninitialize();
    std::printf("scrollshot workspace: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
