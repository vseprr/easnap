#include "native_test_monitor.h"
// Production workspace policy + real editor/configuration integration.
// A private window station/desktop gives these tests their own OS clipboard.
#include "ui/capture_workspace.h"
#include "strings/strings.h"
#include "ui/overlay.h"
#include "ui/recording_controls.h"
#include "editor/screenshot_editor.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <future>
#include <thread>

namespace {
using namespace easnap;
using app::CaptureMode;
int passed{}, failed{};
#define CHECK(...) do { if (__VA_ARGS__) { ++passed; } else { ++failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (false)

class PrivateDesktop {
public:
    bool Open() {
        old_station_ = ::GetProcessWindowStation();
        old_desktop_ = ::GetThreadDesktop(::GetCurrentThreadId());
        station_ = ::CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
        if (!station_ || !::SetProcessWindowStation(station_)) { return false; }
        desktop_ = ::CreateDesktopW(L"EasnapWorkspaceTests", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
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

void Policy() {
    ui::CaptureWorkspace workspace;
    CHECK(workspace.mode() == CaptureMode::Screenshot && !workspace.editing());
    CHECK(workspace.magnifier_visible() && workspace.accent() == ui::theme::Current().screenshot_border);
    workspace.SetMode(CaptureMode::Record);
    CHECK(!workspace.editing() && workspace.accent() == ui::theme::Current().record_border);
    CHECK(!workspace.quick_return_armed());
    workspace.SetMode(CaptureMode::Screenshot);
    workspace.EnterEditing();
    CHECK(workspace.screenshot_visible() && !workspace.magnifier_visible());
    // Presentation movement makes no editor-feature call.
    workspace.SetMode(CaptureMode::Record);
    CHECK(workspace.quick_return_armed() && workspace.recording_visible());
    CHECK(!workspace.DocumentCommitted()); // Other modes cannot fire it.
    workspace.SetMode(CaptureMode::Screenshot);
    workspace.EditorFeatureUsed();
    CHECK(workspace.quick_return_armed());
    CHECK(workspace.DocumentCommitted());
    CHECK(workspace.automatic_attempt_consumed() && !workspace.quick_return_armed());
    CHECK(!workspace.DocumentCommitted()); // Failure cannot repeat an attempt.
    workspace.SetMode(CaptureMode::Record); workspace.SetMode(CaptureMode::Screenshot);
    CHECK(!workspace.DocumentCommitted());
    workspace.Reset(); workspace.EnterEditing(); workspace.EditorFeatureUsed();
    workspace.SetMode(CaptureMode::Record); workspace.SetMode(CaptureMode::Screenshot);
    CHECK(!workspace.quick_return_armed() && !workspace.DocumentCommitted());
    CHECK(workspace.screenshot_fixed() && !workspace.can_adjust_selection() && !workspace.can_switch_mode());
    CHECK(workspace.mode() == CaptureMode::Screenshot);
    workspace.Reset(); workspace.EnterEditing();
    CHECK(!workspace.screenshot_fixed() && workspace.can_adjust_selection() && workspace.can_switch_mode());
    workspace.SetMode(CaptureMode::Record);
    CHECK(!workspace.ConsumeQuickReturn());
    workspace.SetMode(CaptureMode::Screenshot);
    CHECK(workspace.ConsumeQuickReturn() && !workspace.ConsumeQuickReturn());
}

void PointerPolicy() {
    using Target = ui::WorkspacePointerTarget;
    const auto metrics = ui::WorkspacePointerMetrics::Native();
    CHECK(metrics.double_time == ::GetDoubleClickTime());
    CHECK(metrics.double_width == ::GetSystemMetrics(SM_CXDOUBLECLK));
    const POINT a{-100, 80}, b{a.x + metrics.drag_width + 1, a.y + metrics.drag_height + 1};
    ui::WorkspacePointerSequence input;
    input.Down(a, 10, false, Target::Content, metrics); CHECK(!input.Up(a));
    input.Down(a, 10 + metrics.double_time, true, Target::Content, metrics);
    CHECK(input.Up(a)); // Inclusive system time boundary.
    input.Down(a, 10, false, Target::Content, metrics); CHECK(!input.Up(a));
    input.Down(a, 11 + metrics.double_time, true, Target::Content, metrics); CHECK(!input.Up(a));
    for (Target target : {Target::Creation, Target::Handle, Target::Excluded}) {
        input.Cancel(); input.Down(a, 10, false, target, metrics); CHECK(!input.Up(a));
        input.Down(a, 11, true, target, metrics); CHECK(!input.Up(a));
    }
    input.Cancel(); input.Down(a, 10, false, Target::Content, metrics); CHECK(!input.Up(a));
    input.Down(a, 11, true, Target::Content, metrics); input.Move(b);
    CHECK(input.dragged()); CHECK(!input.Up(a)); // Moving back cannot restore eligibility.
    input.Cancel(); input.Down(a, 10, false, Target::Content, metrics); input.Move(b); CHECK(!input.Up(b));
    input.Down(a, 11, true, Target::Content, metrics); CHECK(!input.Up(a));
    input.Cancel(); input.Down(a, 10, false, Target::Content, metrics); CHECK(!input.Up(a));
    input.Down({a.x + metrics.double_width, a.y}, 11, true, Target::Content, metrics);
    CHECK(!input.Up({a.x + metrics.double_width, a.y}));
    input.Cancel(); input.Down(a, MAXDWORD - 10, false, Target::Content, metrics); CHECK(!input.Up(a));
    input.Down(a, 5, true, Target::Content, metrics); CHECK(input.Up(a)); // Native timestamp wrap.
    input.Cancel(); input.Down(a, 10, false, Target::Annotation, metrics); CHECK(!input.Up(a));
    input.Down(a, 11, true, Target::Annotation, metrics); CHECK(input.Up(a));
    input.Cancel(); input.Down(a, 10, false, Target::Annotation, metrics); CHECK(!input.Up(a));
    input.Down(a, 11, true, Target::Annotation, metrics); input.Move(b); CHECK(!input.Up(a));
    input.Cancel(); input.Down(a, 10, false, Target::Content, metrics); CHECK(!input.Up(a));
    input.Down(a, 11, true, Target::Annotation, metrics); CHECK(!input.Up(a));
    CHECK(ui::ModeToolbarOpacity(0) == 255);
    BYTE previous = 255;
    for (uint64_t elapsed = 1; elapsed <= ui::kModeToolbarFadeDurationMs; ++elapsed) {
        const BYTE opacity = ui::ModeToolbarOpacity(elapsed);
        CHECK(opacity <= previous); previous = opacity;
    }
    CHECK(previous == 0 && ui::ModeToolbarOpacity(UINT64_MAX) == 0);
}

void SelectTool(editor::ScreenshotEditor& editor, editor::ToolId tool) {
    HWND button = ::GetDlgItem(editor.hwnd(), 2000 + static_cast<int>(tool));
    CHECK(button && (::GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE));
    ::SendMessageW(button, BM_CLICK, 0, 0);
}

void Draw(editor::ScreenshotEditor& editor, POINT from, POINT to) {
    editor.ImagePointer(WM_LBUTTONDOWN, from);
    editor.ImagePointer(WM_MOUSEMOVE, to);
    editor.ImagePointer(WM_LBUTTONUP, to);
}

void EditorAndCompletion() {
    const RECT source_bounds{-200, -100, 440, 380};
    const RECT region{-120, -40, 200, 180};
    platform::BgraImage source;
    source.Reset(640, 480);
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            source.row(y)[x] = 0xff000000u | (static_cast<uint32_t>(x % 255) << 16) |
                (static_cast<uint32_t>(y % 255) << 8) | static_cast<uint32_t>((x + y) % 255);
        }
    }
    source.row(200)[300] = 0xffff00ffu;
    platform::BgraImage expected;
    CHECK(platform::CropImage(source, RECT{80, 60, 400, 280}, expected));
    app::AppSettings settings;
    ui::CaptureWorkspace workspace;
    int closed{}, attempts{}, acquisitions{};
    bool copied{}, allow_capture = true;
    editor::ScreenshotEditor* active{};
    editor::ScreenshotEditor::Callbacks callbacks;
    callbacks.on_feature_used = [&] { workspace.EditorFeatureUsed(); };
    callbacks.on_closed = [&](bool success) { ++closed; copied = success; };
    callbacks.on_prepare_image = [&] {
        ++acquisitions;
        if (!allow_capture) { return false; }
        RECT local = active->region(); ::OffsetRect(&local, -source_bounds.left, -source_bounds.top);
        platform::BgraImage pixels;
        return platform::CropImage(source, local, pixels) && active->SetImage(std::move(pixels));
    };
    callbacks.on_document_committed = [&] {
        if (workspace.DocumentCommitted()) {
            ++attempts;
            if (!active->completing()) { active->CompleteCopy(); }
        }
    };
    editor::ScreenshotEditor editor(callbacks);
    active = &editor;
    const auto open = [&] {
        workspace.Reset(); workspace.EnterEditing(); closed = attempts = acquisitions = 0;
        CHECK(editor.Open(region, &settings, nullptr));
        CHECK(!editor.has_image() && editor.Composite().empty());
    };
    open();
    const RECT expanded{-130, -50, 210, 190};
    editor.SetRegion(expanded);
    const RECT provisional = editor.region(); CHECK(::EqualRect(&expanded, &provisional) != FALSE);
    editor.SetRegion(region);
    ::SetWindowPos(editor.hwnd(), nullptr, 200, 250, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ::ValidateRect(editor.hwnd(), nullptr);
    RECT moving = region; ::OffsetRect(&moving, 5, 7);
    editor.SetRegion(moving, true);
    CHECK(!::GetUpdateRect(editor.hwnd(), nullptr, FALSE));
    RECT resizing = moving; resizing.right += 18; resizing.bottom += 12;
    editor.SetRegion(resizing, true);
    const RECT live_region = editor.region(); CHECK(::EqualRect(&live_region, &resizing));
    CHECK(!::GetUpdateRect(editor.hwnd(), nullptr, FALSE) && !editor.has_image());
    editor.SetRegion(resizing, false); // Release/capture loss with unchanged coordinates.
    // This private window station has no visible compositor/update region;
    // the live-desktop frame test verifies the scheduled paint on release.
    ::ValidateRect(editor.hwnd(), nullptr);
    editor.SetRegion(resizing, false);
    CHECK(!::GetUpdateRect(editor.hwnd(), nullptr, FALSE));
    editor.SetRegion(region);
    ::UpdateWindow(editor.hwnd());
    CHECK(acquisitions == 0 && !editor.has_image() && !workspace.screenshot_fixed());
    workspace.SetMode(CaptureMode::Record); editor.Hide();
    workspace.SetMode(CaptureMode::Screenshot); editor.Show();
    CHECK(workspace.quick_return_armed() && !editor.has_image());
    // Real native toolbar action is the first acquisition, not Editing entry.
    SelectTool(editor, editor::ToolId::Rectangle);
    CHECK(acquisitions == 1 && editor.has_image() && workspace.screenshot_fixed());
    CHECK(workspace.quick_return_armed() && attempts == 0);
    workspace.SetMode(CaptureMode::Record);
    CHECK(workspace.mode() == CaptureMode::Screenshot);
    editor.SetRegion(expanded);
    CHECK(editor.Composite().pixels == expected.pixels);

    platform::BgraImage rendered; rendered.Reset(320, 220);
    {
        platform::GdiImageSurface surface(rendered);
        editor.RenderImage(surface.dc(), region);
    }
    CHECK(rendered.pixels == expected.pixels);
    const auto monitor = platform::MonitorForRect(region);
    for (UINT dpi : {96u, 120u, 144u, 192u, monitor.dpi}) {
        RECT proposed{monitor.work_area.right - 30, monitor.work_area.bottom - 30,
                      monitor.work_area.right + 600, monitor.work_area.bottom + 120};
        // The suggested DPI rectangle can cross onto an adjacent monitor.
        // Verify containment in that destination, not the original monitor.
        MONITORINFO destination{sizeof(destination)};
        CHECK(::GetMonitorInfoW(::MonitorFromRect(&proposed,MONITOR_DEFAULTTONEAREST),&destination));
        ::SendMessageW(editor.hwnd(), WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&proposed));
        RECT placement{}; ::GetWindowRect(editor.hwnd(), &placement);
        CHECK(placement.left >= destination.rcWork.left && placement.right <= destination.rcWork.right);
        CHECK(placement.top >= destination.rcWork.top && placement.bottom <= destination.rcWork.bottom);
        CHECK(editor.Composite().pixels == expected.pixels && acquisitions == 1);
    }
    editor.ImagePointer(WM_LBUTTONDOWN, {-100, -20});
    editor.ImagePointer(WM_MOUSEMOVE, {140, 130});
    CHECK(attempts == 0 && editor.revision() == 0);
    CHECK(editor.Composite().pixels == expected.pixels);
    editor.ImagePointer(WM_LBUTTONUP, {140, 130});
    CHECK(attempts == 1 && closed == 1 && copied && !editor.visible());
    CHECK(::IsClipboardFormatAvailable(CF_DIB));
    CHECK(::IsClipboardFormatAvailable(::RegisterClipboardFormatW(L"PNG")));
    CHECK(::OpenClipboard(nullptr));
    HGLOBAL dib = ::GetClipboardData(CF_DIB);
    const auto* header = static_cast<const BITMAPINFOHEADER*>(::GlobalLock(dib));
    CHECK(header && header->biWidth == 320 && std::abs(header->biHeight) == 220);
    if (header) { ::GlobalUnlock(dib); }
    ::CloseClipboard();
    editor.Destroy();

    open();
    SelectTool(editor, editor::ToolId::Rectangle);
    workspace.SetMode(CaptureMode::Record);
    CHECK(!workspace.quick_return_armed() && workspace.screenshot_fixed());
    for (int i = 0; i < 12; ++i) { Draw(editor, {-100 + i, -20 + i}, {120 + i, 120 + i}); }
    CHECK(editor.revision() == 12 && attempts == 0 && closed == 0 && acquisitions == 1);
    const auto annotated = editor.Composite();
    CHECK(annotated.pixels != expected.pixels && annotated.pixels.front() == expected.pixels.front());
    std::array<BYTE, 256> keys{}; keys[VK_CONTROL] = 0x80; ::SetKeyboardState(keys.data());
    editor.Keyboard(WM_KEYDOWN, 'Z', 0); editor.Keyboard(WM_KEYDOWN, 'Y', 0);
    keys.fill(0); ::SetKeyboardState(keys.data());
    CHECK(editor.Composite().pixels == annotated.pixels);
    editor.SetRegion(expanded);
    CHECK(editor.Composite().pixels == annotated.pixels);
    CHECK(editor.CompleteCopy() && closed == 1 && !editor.visible());
    editor.Destroy();

    // No tool use: cancel acquires nothing; explicit copy acquires once.
    open();
    editor.Keyboard(WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(acquisitions == 0 && closed == 1 && !copied);
    editor.Destroy();
    open();
    CHECK(editor.CompleteCopy() && acquisitions == 1 && closed == 1 && copied);
    editor.Destroy();

    // Failed acquisition keeps the provisional frame editable and retryable.
    open(); allow_capture = false;
    SelectTool(editor, editor::ToolId::Rectangle);
    CHECK(acquisitions == 1 && !editor.has_image() && !workspace.screenshot_fixed());
    allow_capture = true;
    SelectTool(editor, editor::ToolId::Rectangle);
    CHECK(acquisitions == 2 && editor.has_image() && workspace.screenshot_fixed());
    editor.Destroy();

    // Real clipboard contention on the isolated station consumes one attempt.
    open();
    workspace.SetMode(CaptureMode::Record); workspace.SetMode(CaptureMode::Screenshot);
    SelectTool(editor, editor::ToolId::Ellipse);
    std::promise<bool> locked; auto locked_result = locked.get_future();
    std::promise<void> release; auto release_signal = release.get_future();
    std::thread holder([&] {
        HWND owner = ::CreateWindowExW(0, L"STATIC", L"clipboard lock", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        const bool opened = ::OpenClipboard(owner) != FALSE;
        locked.set_value(opened); release_signal.wait();
        if (opened) { ::CloseClipboard(); }
        ::DestroyWindow(owner);
    });
    CHECK(locked_result.get());
    Draw(editor, {-80, 0}, {100, 100});
    CHECK(attempts == 1 && editor.visible() && closed == 0 && editor.revision() == 1);
    release.set_value(); holder.join();
    Draw(editor, {-70, 10}, {110, 110});
    CHECK(attempts == 1 && editor.revision() == 2 && editor.visible());
    CHECK(editor.CompleteCopy() && closed == 1 && !editor.visible());
    editor.Destroy();

    // Enter, explicit Copy and cancellation use their actual text commit rules.
    for (int completion : {0, 1, 2}) {
        open();
        workspace.SetMode(CaptureMode::Record); workspace.SetMode(CaptureMode::Screenshot);
        SelectTool(editor, editor::ToolId::Text);
        editor.ImagePointer(WM_LBUTTONDOWN, {-80, 0});
        editor.Keyboard(WM_CHAR, 'B', 0);
        CHECK(!editor.document().annotations().empty() && attempts == 0);
        if (completion == 0) {
            editor.Keyboard(WM_CHAR, '\r', 0);
            CHECK(editor.visible() && attempts==0); // Return inserts a paragraph.
            CHECK(editor.CompleteCopy());
        }
        else if (completion == 1) { CHECK(editor.CompleteCopy()); }
        else { ::SendMessageW(editor.hwnd(), WM_CLOSE, 0, 0); }
        CHECK(closed == 1 && !editor.visible() && copied == (completion != 2));
        CHECK(attempts == (completion != 2 ? 1 : 0));
        editor.Destroy();
    }
}

// Click the existing brush-size controls at 96 DPI with an unwrapped island.
void LockedToolCompletion() {
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    if (monitors.empty()) { return; }
    const auto bounds = monitors.front().bounds;
    const RECT region{bounds.left + 200, bounds.top + 200, bounds.left + 360, bounds.top + 320};
    const POINT point{region.left + 70, region.top + 60};
    capture::DesktopSnapshot snapshot;
    snapshot.virtual_bounds = platform::VirtualScreenBounds();
    app::AppSettings settings;
    for (editor::ToolId tool : {editor::ToolId::Pointer, editor::ToolId::Pen, editor::ToolId::Arrow,
            editor::ToolId::Rectangle, editor::ToolId::Ellipse, editor::ToolId::Text, editor::ToolId::Mosaic}) {
        int attempts{}, acquisitions{};
        bool copied{};
        editor::ScreenshotEditor* active{};
        ui::OverlayController* host{};
        ui::OverlayController::Callbacks overlay_callbacks;
        overlay_callbacks.uses_annotation_tool = [&] { return active && active->uses_annotation_tool(); };
        overlay_callbacks.on_image_pointer = [&](UINT message, POINT pt) { active->ImagePointer(message, pt); };
        overlay_callbacks.on_screenshot_complete = [&] { ++attempts; copied = active->CompleteCopy(); };
        ui::OverlayController overlay(overlay_callbacks); host = &overlay;
        editor::ScreenshotEditor::Callbacks editor_callbacks;
        editor_callbacks.on_prepare_image = [&] {
            ++acquisitions;
            platform::BgraImage pixels; pixels.Reset(160, 120);
            std::fill(pixels.pixels.begin(), pixels.pixels.end(), 0xff4080c0);
            if (!active->SetImage(std::move(pixels))) { return false; }
            host->FixScreenshot();
            return true;
        };
        editor_callbacks.on_closed = [&](bool) { overlay.Hide(); };
        editor::ScreenshotEditor editor(editor_callbacks); active = &editor;
        CHECK(overlay.Show(snapshot, point));
        overlay.selector().SetSelection(region); overlay.EnterEditing();
        CHECK(editor.Open(region, &settings, overlay.owner_window()));
        SelectTool(editor, tool);
        CHECK(acquisitions == 1 && overlay.workspace().screenshot_fixed() && !overlay.selector().enabled());
        POINT local = point; ::ScreenToClient(overlay.owner_window(), &local);
        const LPARAM location = MAKELPARAM(local.x, local.y);
        const HWND input = overlay.owner_window();
        ::SendMessageW(input, WM_LBUTTONDOWN, 0, location);
        ::SendMessageW(input, WM_LBUTTONUP, 0, location);
        ::SendMessageW(input, WM_LBUTTONDBLCLK, 0, location);
        const LPARAM dragged = MAKELPARAM(local.x + 25, local.y + 15);
        ::SendMessageW(input, WM_MOUSEMOVE, MK_LBUTTON, dragged);
        ::SendMessageW(input, WM_LBUTTONUP, 0, dragged);
        CHECK(attempts == 0 && editor.visible());
        const RECT unchanged = overlay.selector().selection();
        CHECK(::EqualRect(&region, &unchanged));

        std::promise<bool> clipboard_locked;
        std::promise<void> release_clipboard;
        auto released = release_clipboard.get_future();
        std::thread holder;
        if (tool == editor::ToolId::Rectangle) {
            holder = std::thread([&] {
                HWND owner = ::CreateWindowExW(0, L"STATIC", L"clipboard lock", WS_POPUP, 0, 0, 1, 1,
                    nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
                const bool opened = ::OpenClipboard(owner) != FALSE;
                clipboard_locked.set_value(opened); released.wait();
                if (opened) { ::CloseClipboard(); }
                ::DestroyWindow(owner);
            });
            CHECK(clipboard_locked.get_future().get());
        }
        ::SendMessageW(input, WM_LBUTTONDOWN, 0, location);
        ::SendMessageW(input, WM_LBUTTONUP, 0, location);
        CHECK(attempts == 0 && editor.visible());
        const auto expected = editor.Composite(); // Preserve the tool's ordinary first-click behavior.
        ::SendMessageW(input, WM_LBUTTONDBLCLK, 0, location);
        CHECK(attempts == 0);
        ::SendMessageW(input, WM_LBUTTONUP, 0, location);
        if (holder.joinable()) {
            CHECK(attempts == 1 && !copied && editor.visible() && overlay.workspace().screenshot_fixed());
            CHECK(editor.Composite().pixels == expected.pixels);
            release_clipboard.set_value(); holder.join();
            ::SendMessageW(input, WM_LBUTTONDOWN, 0, location);
            ::SendMessageW(input, WM_LBUTTONUP, 0, location);
            ::SendMessageW(input, WM_LBUTTONDBLCLK, 0, location);
            ::SendMessageW(input, WM_LBUTTONUP, 0, location);
        }
        CHECK(attempts == (tool == editor::ToolId::Rectangle ? 2 : 1) && copied && !editor.visible());
        CHECK(::IsClipboardFormatAvailable(CF_DIB));
        CHECK(::IsClipboardFormatAvailable(::RegisterClipboardFormatW(L"PNG")));
        if (::OpenClipboard(nullptr)) {
            HGLOBAL dib = ::GetClipboardData(CF_DIB);
            const auto* header = static_cast<const BITMAPINFOHEADER*>(::GlobalLock(dib));
            CHECK(header && header->biWidth == 160 && std::abs(header->biHeight) == 120 && header->biBitCount == 32);
            if (header && header->biWidth == 160 && std::abs(header->biHeight) == 120 && header->biBitCount == 32) {
                const auto* pixels = reinterpret_cast<const uint32_t*>(reinterpret_cast<const BYTE*>(header) + header->biSize);
                for (int y = 0; y < 120; ++y) {
                    const auto* row = pixels + (header->biHeight < 0 ? y : 119 - y) * 160;
                    CHECK(std::equal(row, row + 160, expected.row(y), [](uint32_t a, uint32_t b) {
                        return (a & 0xffffff) == (b & 0xffffff);
                    }));
                }
            }
            if (header) { ::GlobalUnlock(dib); }
            ::CloseClipboard();
        } else { CHECK(false); }
        editor.Destroy(); overlay.Destroy();
    }

    // Empty toolbar/info space and the native grip fix the image. Failed
    // acquisition stays retryable, and cancellation still works without pixels.
    for (bool grip : {false, true}) {
        bool capture_allowed = false;
        int acquisitions{};
        editor::ScreenshotEditor* active{};
        editor::ScreenshotEditor::Callbacks callbacks;
        callbacks.on_prepare_image = [&] {
            ++acquisitions;
            if (!capture_allowed) { return false; }
            platform::BgraImage pixels; pixels.Reset(160, 120);
            return active->SetImage(std::move(pixels));
        };
        editor::ScreenshotEditor editor(callbacks); active = &editor;
        CHECK(editor.Open(region, &settings, nullptr));
        RECT client{}; ::GetClientRect(editor.hwnd(), &client);
        auto press = [&] {
            ::SendMessageW(editor.hwnd(), grip ? WM_NCLBUTTONDBLCLK : WM_LBUTTONDOWN,
                grip ? HTCAPTION : 0, MAKELPARAM(client.right - 2, client.bottom - 2));
        };
        press(); CHECK(acquisitions == 1 && !editor.has_image());
        capture_allowed = true;
        press(); CHECK(acquisitions == 2 && editor.has_image());
        press(); CHECK(acquisitions == 2);
        editor.Destroy();
    }
}

void SelectBrush(editor::ScreenshotEditor& editor, int index) {
    SelectTool(editor, editor::ToolId::Mosaic);
    ::SendMessageW(::GetDlgItem(editor.context_hwnd(),2520),BM_CLICK,0,0);
    HWND popup=::FindWindowW(L"EasnapScreenshotEditor",L"Brush sizes");
    CHECK(popup && (::GetWindowLongPtrW(popup,GWL_STYLE)&WS_VISIBLE));
    HWND button = ::GetDlgItem(popup, 2300 + index);
    CHECK(button && (::GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE));
    ::SendMessageW(button, BM_CLICK, 0, 0);
}

void MosaicAndDirtyRendering() {
    const RECT region{0, 0, 1920, 1080};
    platform::BgraImage source; source.Reset(1920, 1080);
    uint32_t noise = 17;
    for (auto& pixel : source.pixels) {
        noise = noise * 1664525u + 1013904223u;
        pixel = 0xff000000u | (noise & 0x00ffffffu);
    }
    editor::ScreenshotEditor* active{};
    HDC live_dc{};
    int repaints{};
    uint64_t damaged_pixels{};
    editor::ScreenshotEditor::Callbacks callbacks;
    callbacks.on_image_invalidated = [&](const RECT& damage) {
        if (live_dc) {
            ++repaints;
            damaged_pixels += static_cast<uint64_t>(platform::RectWidth(damage)) * platform::RectHeight(damage);
            active->RenderImage(live_dc, region, &damage);
        }
    };
    editor::ScreenshotEditor editor(callbacks); active = &editor;
    CHECK(editor.Open(region, nullptr, nullptr)); CHECK(editor.SetImage(source));
    RECT placement{0, 100, 1100, 200};
    ::SendMessageW(editor.hwnd(), WM_DPICHANGED, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&placement));
    // Native size choice uses real production command routing.
    SelectBrush(editor, 0);
    Draw(editor, {250, 250}, {330, 280});
    const auto first = editor.Composite();
    CHECK(first.pixels != source.pixels);
    SelectBrush(editor, 2);
    Draw(editor, {100, 100}, {140, 150}); // Earlier origin + larger brush.
    const auto second = editor.Composite();
    CHECK(second.pixels != first.pixels);
    for (int y = 210; y < 315; ++y) {
        CHECK(std::equal(first.row(y) + 215, first.row(y) + 370, second.row(y) + 215));
    }
    std::array<BYTE, 256> keys{}; keys[VK_CONTROL] = 0x80; ::SetKeyboardState(keys.data());
    editor.Keyboard(WM_KEYDOWN, 'Z', 0); CHECK(editor.Composite().pixels == first.pixels);
    editor.Keyboard(WM_KEYDOWN, 'Y', 0); CHECK(editor.Composite().pixels == second.pixels);
    keys.fill(0); ::SetKeyboardState(keys.data());
    SelectBrush(editor, 0); Draw(editor, {250, 250}, {330, 280});
    const auto overlap = editor.Composite();
    CHECK(overlap.pixels != second.pixels); // Overlap still increases redaction.
    for (int y = 60; y < 190; ++y) {
        CHECK(std::equal(second.row(y) + 60, second.row(y) + 185, overlap.row(y) + 60));
    }
    // Ordinary vector edits cannot perturb derived mosaic pixels.
    SelectTool(editor, editor::ToolId::Rectangle);
    Draw(editor, {700, 700}, {800, 800});
    const auto vector_edit = editor.Composite();
    for (int y = 60; y < 315; ++y) {
        CHECK(std::equal(overlap.row(y) + 60, overlap.row(y) + 370, vector_edit.row(y) + 60));
    }
    SelectTool(editor, editor::ToolId::Pen);
    platform::BgraImage incremental; incremental.Reset(1920, 1080);
    const auto started = std::chrono::steady_clock::now();
    {
        platform::GdiImageSurface surface(incremental);
        editor.RenderImage(surface.dc(), region);
        live_dc = surface.dc();
        editor.ImagePointer(WM_LBUTTONDOWN, {900, 400});
        for (LONG i = 1; i <= 240; ++i) { editor.ImagePointer(WM_MOUSEMOVE, {900 + i, 400 + i % 12}); }
        live_dc = nullptr;
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    CHECK(repaints > 200 && damaged_pixels < static_cast<uint64_t>(1920 * 1080) * 10);
    platform::BgraImage full; full.Reset(1920, 1080);
    { platform::GdiImageSurface surface(full); editor.RenderImage(surface.dc(), region); }
    CHECK(full.pixels == incremental.pixels); // Partial repaint has no trails or stale pixels.
    editor.ImagePointer(WM_LBUTTONUP, {1140, 400});
    CHECK(editor.Composite().pixels != vector_edit.pixels);
    std::printf("Dirty render: %d updates, %.2f ms, %.1f%% of full-frame pixel work\n", repaints, elapsed_ms,
        100.0 * damaged_pixels / (static_cast<double>(repaints) * 1920 * 1080));
    // Expanding, reversing and shrinking shapes must erase their old preview,
    // including arrow heads, with the same pixels as a full repaint.
    for (int tool : {2, 3, 4}) { // Existing Arrow, Rectangle, Ellipse toolbar slots.
        SelectTool(editor, static_cast<editor::ToolId>(tool));
        {
            platform::GdiImageSurface surface(incremental);
            editor.RenderImage(surface.dc(), region);
            live_dc = surface.dc();
            editor.ImagePointer(WM_LBUTTONDOWN, {600, 400});
            for (POINT point : {POINT{1000, 650}, POINT{350, 250}, POINT{700, 450}}) {
                editor.ImagePointer(WM_MOUSEMOVE, point);
            }
            live_dc = nullptr;
        }
        { platform::GdiImageSurface surface(full); editor.RenderImage(surface.dc(), region); }
        CHECK(full.pixels == incremental.pixels);
        editor.ImagePointer(WM_LBUTTONUP, {700, 450});
    }
    editor.Destroy();
}

void RecordingRoundTrip() {
    capture::DesktopSnapshot snapshot;
    snapshot.virtual_bounds = platform::VirtualScreenBounds();
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    if (monitors.empty()) { return; }
    for (const auto& monitor : monitors) {
        capture::MonitorFrame frame;
        frame.desktop_rect = monitor.bounds;
        frame.image.Reset(platform::RectWidth(monitor.bounds), platform::RectHeight(monitor.bounds));
        snapshot.monitors.push_back(std::move(frame));
    }
    ui::OverlayController overlay({});
    const RECT bounds = test::SelectedTestMonitor() ? test::SelectedTestMonitor()->bounds:monitors.front().bounds;
    const POINT center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    CHECK(overlay.Show(snapshot, center));
    overlay.SetMode(CaptureMode::Record);
    overlay.EnterEditing();
    int starts{};
    ui::RecordingControls::Callbacks callbacks;
    callbacks.on_start = [&](app::RecordingConfiguration) { ++starts; };
    ui::RecordingControls controls(callbacks);
    app::RecordingConfiguration initial;
    initial.format = media::RecordingFormat::Mp4;
    initial.mp4_audio = media::AudioSource::Both;
    initial.include_cursor = true;
    const RECT region{40, 100, 500, 440};
    CHECK(controls.ShowConfiguring(region, initial, true, overlay.panel_owner_window()));
    HWND panel = controls.hwnd();
    // Activation of either workspace surface must not raise its input/shading
    // planes above the configuration window and intercept its controls.
    const auto panel_on_top = [&] {
        RECT start{}; ::GetWindowRect(::GetDlgItem(panel, IDOK), &start);
        const POINT hit{(start.left+start.right)/2, (start.top+start.bottom)/2};
        HWND target=::WindowFromPoint(hit);
        return target==panel || ::IsChild(panel,target);
    };
    CHECK(panel_on_top());
    const HWND modes=::FindWindowW(L"EasnapCaptureModes",nullptr);
    for(HWND activated : {modes,overlay.owner_window()}) {
        ::SetWindowPos(activated,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        ::SetActiveWindow(activated);
        CHECK(panel_on_top());
    }
    ::SetActiveWindow(panel);
    ::SetFocus(::GetDlgItem(panel, IDOK));
    const HWND focus = ::GetFocus();
    const HWND surface = overlay.owner_window();
    CHECK(::SendMessageW(surface, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(panel),
        MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_NOACTIVATE);
    POINT local = center; ::ScreenToClient(surface, &local);
    for (UINT message : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP}) {
        ::SendMessageW(surface, message, 0, MAKELPARAM(local.x, local.y));
        CHECK(::GetActiveWindow() == panel && ::GetFocus() == focus);
    }
    const RECT unchanged = overlay.selector().selection();
    CHECK(::EqualRect(&bounds, &unchanged) && starts == 0 && overlay.workspace().recording_visible());
    ::SendMessageW(::GetDlgItem(panel, 106), BM_CLICK, 0, 0); // Existing GIF radio.
    ::SendMessageW(::GetDlgItem(panel, 109), BM_CLICK, 0, 0); // Existing Low preset.
    controls.Hide();
    CHECK(controls.ShowConfiguring(region, initial, true, overlay.panel_owner_window()));
    CHECK(controls.hwnd() == panel && starts == 0);
    auto selected = controls.configuration();
    CHECK(selected.format == media::RecordingFormat::Gif && selected.gif.preset == app::GifExportPreset::LowSize);
    ::SendMessageW(::GetDlgItem(panel, 105), BM_CLICK, 0, 0);
    selected = controls.configuration();
    CHECK(selected.format == media::RecordingFormat::Mp4);
    CHECK(selected.mp4_audio == media::AudioSource::Both && selected.include_cursor);
    CHECK(::IsWindowVisible(::GetDlgItem(panel, 111)) && ::IsWindowVisible(::GetDlgItem(panel, 112)));
    // Two real toggles must represent every backend route, retaining it through GIF.
    for (const auto expected : {media::AudioSource::Microphone, media::AudioSource::None,
                               media::AudioSource::SystemAudio, media::AudioSource::Both}) {
        const int toggle = expected == media::AudioSource::Microphone || expected == media::AudioSource::SystemAudio ? 111 : 112;
        ::SendMessageW(::GetDlgItem(panel, toggle), BM_CLICK, 0, 0);
        CHECK(controls.configuration().mp4_audio == expected);
        ::SendMessageW(::GetDlgItem(panel, 106), BM_CLICK, 0, 0);
        CHECK(app::EffectiveAudioSource(controls.configuration()) == media::AudioSource::None);
        CHECK(!::IsWindowVisible(::GetDlgItem(panel, 111)) && !::IsWindowVisible(::GetDlgItem(panel, 112)));
        ::SendMessageW(::GetDlgItem(panel, 105), BM_CLICK, 0, 0);
        CHECK(controls.configuration().mp4_audio == expected);
    }
    RECT panel_before{}, panel_after{}; ::GetWindowRect(panel, &panel_before);
    ::ValidateRect(panel, nullptr);
    ::EnumChildWindows(panel, [](HWND child, LPARAM) -> BOOL {
        ::ValidateRect(child, nullptr); return TRUE;
    }, 0);
    const RECT moved_region{60, 120, 530, 470};
    for (int i = 0; i < 40; ++i) { controls.SetRegion(i % 2 ? moved_region : region); }
    ::GetWindowRect(panel, &panel_after);
    CHECK(::EqualRect(&panel_before, &panel_after) && !::GetUpdateRect(panel, nullptr, FALSE));
    bool child_damage{};
    ::EnumChildWindows(panel, [](HWND child, LPARAM context) -> BOOL {
        if (::GetUpdateRect(child, nullptr, FALSE)) { *reinterpret_cast<bool*>(context) = true; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&child_damage));
    CHECK(!child_damage);
    MSG outside{nullptr, WM_KEYDOWN, VK_RETURN, 0, 0, {0, 0}};
    CHECK(!controls.ProcessDialogMessage(outside) && starts == 0);
    auto visible_boundaries = [] {
        int count{};
        ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM context) -> BOOL {
            wchar_t name[64]{}; ::GetClassNameW(window, name, 64);
            if (std::wstring_view(name) == L"EasnapRecordingBoundary" && ::IsWindowVisible(window)) {
                ++*reinterpret_cast<int*>(context);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&count));
        return count;
    };
    CHECK(visible_boundaries() == 0); // Workspace owns configuration selection chrome.
    controls.RetireWorkspace();
    CHECK(visible_boundaries() == 4); // Live recording restores its existing boundary.
    ::SendMessageW(::GetDlgItem(panel, IDOK), BM_CLICK, 0, 0);
    CHECK(starts == 1); // Only the real Start command invokes the recording callback.
    controls.Destroy();
    overlay.Destroy();
}
// Optional live-desktop benchmark. Uses the production input, editor and native
// paint paths; no sleeps, simulated work, capture backend or clipboard changes.
// Timing is reported, never used as a machine-dependent correctness threshold.
void FramePerformance() {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    if (monitors.empty()) { return; }
    capture::DesktopSnapshot snapshot;
    snapshot.virtual_bounds = platform::VirtualScreenBounds();
    for (const auto& monitor : monitors) {
        capture::MonitorFrame frame;
        frame.desktop_rect = monitor.bounds;
        frame.image.Reset(platform::RectWidth(monitor.bounds), platform::RectHeight(monitor.bounds));
        snapshot.monitors.push_back(std::move(frame));
    }
    const RECT bounds = test::SelectedTestMonitor() ? test::SelectedTestMonitor()->bounds:monitors.front().bounds;
    const LONG width = platform::RectWidth(bounds), height = platform::RectHeight(bounds);
    const RECT initial{bounds.left + width / 4, bounds.top + height / 3,
                       bounds.left + width * 3 / 4, bounds.top + height * 2 / 3};
    const POINT center{(initial.left + initial.right) / 2, (initial.top + initial.bottom) / 2};
    app::AppSettings settings;
    editor::ScreenshotEditor editor({});
    ui::RecordingControls controls({});
    ui::OverlayController::Callbacks callbacks;
    int region_changes{};
    ui::OverlayController* active_overlay{};
    bool first_open = true;
    double first_editor_open_ms{};
    callbacks.on_editing_changed = [&](CaptureMode mode, const RECT& region) {
        if (!first_open || mode != CaptureMode::Screenshot) { return; }
        const auto started = std::chrono::steady_clock::now();
        CHECK(editor.Open(region, &settings, active_overlay->owner_window()));
        first_editor_open_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        first_open = false;
    };
    callbacks.on_region_changed = [&](const RECT& region) {
        ++region_changes;
        editor.SetRegion(region, active_overlay && active_overlay->selector().interacting());
        controls.SetRegion(region);
    };
    ui::OverlayController overlay(callbacks);
    active_overlay = &overlay;
    const auto show_started = std::chrono::steady_clock::now();
    CHECK(overlay.Show(snapshot, center));
    const double first_overlay_show_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - show_started).count();
    std::vector<HWND> planes;
    ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM context) -> BOOL {
        wchar_t name[80]{}; ::GetClassNameW(window, name, 80);
        if (std::wstring_view(name) == L"EasnapOverlayDim" ||
            std::wstring_view(name) == L"EasnapOverlayChrome") {
            reinterpret_cast<std::vector<HWND>*>(context)->push_back(window);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&planes));
    auto paint = [&] {
        for (HWND plane : planes) {
            if (::GetUpdateRect(plane, nullptr, FALSE)) { ::SendMessageW(plane, WM_PAINT, 0, 0); }
        }
        if (editor.visible()) { ::UpdateWindow(editor.hwnd()); }
        if (controls.visible()) {
            ::RedrawWindow(controls.hwnd(), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
        ::GdiFlush();
    };
    const HWND input = overlay.owner_window();
    auto send = [&](UINT message, POINT point, WPARAM flags = 0) {
        ::ScreenToClient(input, &point);
        ::SendMessageW(input, message, flags, MAKELPARAM(point.x, point.y));
    };
    send(WM_LBUTTONDOWN, {initial.left, initial.top});
    const auto drag_started = std::chrono::steady_clock::now();
    send(WM_MOUSEMOVE, {initial.right, initial.bottom}, MK_LBUTTON);
    paint();
    const double first_drag_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - drag_started).count();
    const auto release_started = std::chrono::steady_clock::now();
    send(WM_LBUTTONUP, {initial.right, initial.bottom});
    paint();
    ::RedrawWindow(editor.hwnd(), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    ::GdiFlush();
    const double first_release_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - release_started).count();
    CHECK(!first_open && editor.visible() && overlay.workspace().screenshot_visible());
    const RECT first_region = overlay.selector().selection();
    CHECK(::EqualRect(&first_region, &initial));
    std::printf("First native selection: overlay %.2f ms, first drag+paint %.2f ms, "
        "release+editor+paint %.2f ms (editor.Open %.2f ms)\n",
        first_overlay_show_ms, first_drag_ms, first_release_ms, first_editor_open_ms);
    paint();
    const DWORD resources = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
    for (bool record : {false, true}) {
        if (record) {
            editor.Hide(); overlay.SetMode(CaptureMode::Record);
            CHECK(controls.ShowConfiguring(initial, {}, true));
        }
        for (bool resize : {false, true}) {
            overlay.selector().SetSelection(initial); overlay.RegionChanged(); paint();
            const POINT start = resize ? POINT{initial.right, initial.bottom} : center;
            send(WM_LBUTTONDOWN, start);
            std::vector<double> timings;
            uint64_t dirty_pixels{};
            double input_ms{}, paint_ms{};
            const int before_changes = region_changes;
            POINT point{};
            for (int i = 0; i < 160; ++i) {
                const LONG delta = 12 + (i < 80 ? i : 159 - i);
                point = {start.x + delta, start.y + delta / 2};
                const auto begin = std::chrono::steady_clock::now();
                send(WM_MOUSEMOVE, point, MK_LBUTTON);
                const auto input_end = std::chrono::steady_clock::now();
                for (HWND plane : planes) {
                    HRGN damage = ::CreateRectRgn(0, 0, 0, 0);
                    if (::GetUpdateRgn(plane, damage, FALSE) > NULLREGION) {
                        const DWORD bytes = ::GetRegionData(damage, 0, nullptr);
                        std::vector<DWORD> storage((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
                        auto* data = reinterpret_cast<RGNDATA*>(storage.data());
                        if (::GetRegionData(damage, bytes, data)) {
                            const auto* rects = reinterpret_cast<const RECT*>(data->Buffer);
                            for (DWORD n = 0; n < data->rdh.nCount; ++n) {
                                dirty_pixels += static_cast<uint64_t>(rects[n].right - rects[n].left) *
                                    static_cast<uint64_t>(rects[n].bottom - rects[n].top);
                            }
                        }
                    }
                    ::DeleteObject(damage);
                }
                const auto paint_begin = std::chrono::steady_clock::now();
                paint();
                const auto end = std::chrono::steady_clock::now();
                const double input_time = std::chrono::duration<double, std::milli>(input_end - begin).count();
                const double paint_time = std::chrono::duration<double, std::milli>(end - paint_begin).count();
                input_ms += input_time; paint_ms += paint_time;
                timings.push_back(input_time + paint_time);
            }
            if (!record) { CHECK(!::GetUpdateRect(editor.hwnd(), nullptr, FALSE)); }
            send(WM_LBUTTONUP, point);
            if (!record && resize) { CHECK(::GetUpdateRect(editor.hwnd(), nullptr, FALSE)); }
            CHECK(region_changes == before_changes + 160); // 159 distinct positions + gesture completion.
            paint();
            RECT expected = initial;
            if (resize) { expected.right += 12; expected.bottom += 6; }
            else { ::OffsetRect(&expected, 12, 6); }
            const RECT actual = overlay.selector().selection();
            CHECK(::EqualRect(&expected, &actual));
            CHECK(!overlay.selector().interacting() && !editor.has_image());
            std::sort(timings.begin(), timings.end());
            std::printf("Frame %s %s: %ldx%ld, 160 updates, input %.2f ms, paint %.2f ms, "
                "median %.3f ms, p95 %.3f ms, dirty %llu pixels, callbacks %d\n",
                record ? "Record" : "Screenshot", resize ? "resize" : "move", width, height,
                input_ms, paint_ms, timings[80], timings[152],
                static_cast<unsigned long long>(dirty_pixels), region_changes - before_changes);
        }
    }
    // Release coordinates must be applied even without a final WM_MOUSEMOVE.
    // Capture loss must also finish the gesture and flush the duplicate readout.
    controls.Hide(); overlay.SetMode(CaptureMode::Screenshot); editor.Show();
    for (bool cancel : {false, true}) {
        overlay.selector().SetSelection(initial); overlay.RegionChanged(); paint();
        const POINT start{initial.right, initial.bottom};
        send(WM_LBUTTONDOWN, start);
        send(WM_MOUSEMOVE, {start.x + 20, start.y + 10}, MK_LBUTTON);
        CHECK(!::GetUpdateRect(editor.hwnd(), nullptr, FALSE));
        if (cancel) { ::ReleaseCapture(); }
        else { send(WM_LBUTTONUP, {start.x + 23, start.y + 13}); }
        RECT expected = initial;
        expected.right += cancel ? 20 : 23; expected.bottom += cancel ? 10 : 13;
        const RECT actual = overlay.selector().selection(), editor_region = editor.region();
        CHECK(::EqualRect(&expected, &actual) && ::EqualRect(&actual, &editor_region));
        CHECK(!overlay.selector().interacting() && ::GetUpdateRect(editor.hwnd(), nullptr, FALSE));
        paint();
    }
    std::printf("Frame GDI objects: warm %lu, final %lu\n", resources,
        ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS));
    controls.Destroy(); editor.Destroy(); overlay.Destroy();
}
} // namespace

int main(int argc, char** argv) {
    // Assertions name controls by their English text, whatever Windows speaks.
    easnap::strings::SetPreference(easnap::strings::LanguagePreference::English);
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    if (argc > 1 && std::string_view(argv[1]) == "--frame-performance") {
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        FramePerformance();
        platform::GraphicsEnvironment::Get().Shutdown();
        ::CoUninitialize();
        std::printf("Frame performance: %d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    Policy(); PointerPolicy(); RecordingRoundTrip();
    std::thread clipboard_tests([] {
        PrivateDesktop desktop;
        if (!desktop.Open()) {
            std::fprintf(stderr, "Private clipboard desktop creation failed: %lu\n", ::GetLastError());
            ++failed;
            return;
        }
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        EditorAndCompletion(); LockedToolCompletion(); MosaicAndDirtyRendering();
        platform::GraphicsEnvironment::Get().Shutdown();
        ::CoUninitialize();
        ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
            wchar_t name[64]{};
            ::GetClassNameW(window, name, 64);
            if (std::wstring_view(name) == L"EasnapCaptureCopiedNotice") { ::DestroyWindow(window); }
            return TRUE;
        }, 0);
    });
    clipboard_tests.join();
    std::printf("Capture workspace: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

