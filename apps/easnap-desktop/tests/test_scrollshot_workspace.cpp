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
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace easnap;
using app::CaptureMode;
using easnap_test::synthetic::Image;
using easnap_test::synthetic::MakeArticle;
using ui::ScrollshotCommand;
using ui::ScrollshotControls;

int passed{}, failed{};
#define CHECK(...) do { if (__VA_ARGS__) { ++passed; } else { ++failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (false)

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
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
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
    struct Query { std::vector<HWND> windows; RECT monitor; } query{{}, monitor};
    ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        auto* found = reinterpret_cast<Query*>(value);
        RECT rect{};
        if (::IsWindowVisible(window) && WindowText(window) == L"Easnap outside shading" &&
            ::GetWindowRect(window, &rect) && Overlaps(rect, found->monitor)) {
            found->windows.push_back(window);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&query));
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
    overlay.ExposeScrollshotFrame(true);
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

    // Outside the frame nothing happens: no new selection, no cancellation.
    ::SendMessageW(dim, WM_LBUTTONDOWN, MK_LBUTTON, client(monitor.bounds.left + 50, monitor.bounds.top + 80));
    ::SendMessageW(dim, WM_MOUSEMOVE, MK_LBUTTON, client(monitor.bounds.left + 250, monitor.bounds.top + 180));
    ::SendMessageW(dim, WM_LBUTTONUP, 0, client(monitor.bounds.left + 250, monitor.bounds.top + 180));
    ::SendMessageW(dim, WM_RBUTTONUP, 0, client(monitor.bounds.left + 50, monitor.bounds.top + 80));
    ::SendMessageW(dim, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
    const RECT selection = overlay.selector().selection();
    CHECK(::EqualRect(&selection, &frame) && cancels == 0);

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

    // The island follows the frame while it is being adjusted.
    RECT moved = frame;
    ::OffsetRect(&moved, -200, -120);
    controls.SetFrame(moved);
    RECT followed{};
    ::GetWindowRect(island, &followed);
    CHECK(followed.left == island_rect.left - 200 && followed.top == island_rect.top - 120);
    controls.SetFrame(frame);

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
    // The tools island belongs to the viewport: below it, inside the work area.
    RECT island{};
    ::GetWindowRect(editor.hwnd(), &island);
    CHECK(island.top >= view.viewport.bottom && island.bottom <= monitor.work_area.bottom);
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

}  // namespace

int main() {
    // Assertions name controls by their English text, whatever Windows speaks.
    easnap::strings::SetPreference(easnap::strings::LanguagePreference::English);
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Policy();
    FrameAroundContent();
    OverlayPhases();
    Controls();
    ToolsIslandButton();
    DocumentEditing();
    platform::GraphicsEnvironment::Get().Shutdown();
    ::CoUninitialize();
    std::printf("scrollshot workspace: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
