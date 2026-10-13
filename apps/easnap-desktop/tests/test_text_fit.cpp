// Easnap integration tests - text fits its control in every language.
//
// Production windows are created on a private desktop once per language. For
// every button that draws a caption and every label, the text is measured
// with the control's own font and compared with the room the control has. A
// translation that is longer than its English original must widen or wrap its
// control; it must never be cut.

#include "native_test_monitor.h"
#include "scrollshot_synthetic.h"

#include "app/recording_workflow.h"
#include "editor/screenshot_editor.h"
#include "strings/strings.h"
#include "ui/native_controls.h"
#include "ui/overlay.h"
#include "ui/recording_controls.h"
#include "ui/scrollshot_controls.h"
#include "ui/settings_dialog.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace easnap;

int passed{}, failed{};
#define CHECK(...) do { if (__VA_ARGS__) { ++passed; } else { ++failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (false)

const char* g_language = "";

std::wstring TextOf(HWND window) {
    wchar_t text[512]{};
    ::GetWindowTextW(window, text, ARRAYSIZE(text));
    return text;
}

/// Checks every visible button and label below `top`. Returns how many
/// controls with text were examined.
int CheckFits(HWND top, const char* surface) {
    struct Context { const char* surface; int examined; } context{surface, 0};
    ::EnumChildWindows(top, [](HWND child, LPARAM value) -> BOOL {
        auto* context = reinterpret_cast<Context*>(value);
        if (::IsWindowVisible(child) == FALSE) { return TRUE; }
        wchar_t class_name[32]{};
        ::GetClassNameW(child, class_name, ARRAYSIZE(class_name));
        const std::wstring text = TextOf(child);
        if (text.empty()) { return TRUE; }
        RECT client{};
        ::GetClientRect(child, &client);
        if (::lstrcmpiW(class_name, L"BUTTON") == 0) {
            const int needed = ui::ButtonContentWidth(child);
            if (needed == 0) { return TRUE; }  // Icon only: the text is its accessible name.
            ++context->examined;
            if (needed > client.right) {
                ++failed;
                std::fprintf(stderr, "FAIL [%s] %s: button %d \"%ls\" needs %d px, has %ld\n", g_language,
                             context->surface, ::GetDlgCtrlID(child), text.c_str(), needed, client.right);
            } else {
                ++passed;
            }
        } else if (::lstrcmpiW(class_name, L"STATIC") == 0) {
            ++context->examined;
            HDC dc = ::CreateCompatibleDC(nullptr);
            const HGDIOBJ previous = ::SelectObject(dc, reinterpret_cast<HFONT>(::SendMessageW(child, WM_GETFONT, 0, 0)));
            RECT single{0, 0, 0, 0};
            ::DrawTextW(dc, text.c_str(), -1, &single, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            RECT wrapped{0, 0, client.right, 0};
            ::DrawTextW(dc, text.c_str(), -1, &wrapped, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            ::SelectObject(dc, previous);
            ::DeleteDC(dc);
            // One line that fits, or several lines that fit the height.
            const bool fits = single.right <= client.right ||
                              (client.bottom >= 2 * single.bottom - 2 && wrapped.bottom <= client.bottom &&
                               wrapped.right <= client.right);
            if (!fits) {
                ++failed;
                std::fprintf(stderr, "FAIL [%s] %s: label %d \"%ls\" needs %ld px in one line (or %ld px high wrapped), has %ld x %ld\n",
                             g_language, context->surface, ::GetDlgCtrlID(child), text.c_str(), single.right,
                             wrapped.bottom, client.right, client.bottom);
            } else {
                ++passed;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.examined;
}

std::vector<HWND> ThreadWindows(const wchar_t* class_name) {
    struct Query { std::vector<HWND> windows; const wchar_t* class_name; } query{{}, class_name};
    ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        auto* found = reinterpret_cast<Query*>(value);
        wchar_t name[64]{};
        ::GetClassNameW(window, name, ARRAYSIZE(name));
        if (::lstrcmpW(name, found->class_name) == 0 && ::IsWindowVisible(window)) { found->windows.push_back(window); }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.windows;
}

void RecordingIsland(const RECT& region) {
    ui::RecordingControls controls({});
    app::RecordingConfiguration configuration;
    CHECK(controls.ShowConfiguring(region, configuration, true, nullptr));
    CHECK(CheckFits(controls.hwnd(), "recording, MP4") >= 7);
    // The GIF choices replace the audio ones.
    ::SendMessageW(::GetDlgItem(controls.hwnd(), 106), BM_CLICK, 0, 0);
    CHECK(CheckFits(controls.hwnd(), "recording, GIF") >= 7);
    controls.SetError(strings::Format(IDS_RECORD_REGION_TOO_LARGE, {L"7680", L"4320"}));
    CHECK(CheckFits(controls.hwnd(), "recording, region too large") >= 8);
    for (const UINT next : {IDS_RECORD_START_AUDIO, IDS_RECORD_START_ENCODER, IDS_RECORD_START_STORAGE,
                            IDS_RECORD_START_CAPTURE, IDS_RECORD_START_INTERNAL}) {
        controls.SetError(std::wstring(strings::Get(IDS_RECORD_START_FAILED)) + L" " + strings::Get(next));
        CHECK(CheckFits(controls.hwnd(), "recording, start failure") >= 8);
    }
    controls.SetError({});
    controls.RetireWorkspace();
    controls.SetSessionState(app::SessionState::RecordingActive);
    controls.SetElapsed(36'000'000'000ull);
    CHECK(CheckFits(controls.hwnd(), "recording, active") >= 2);
    controls.SetSessionState(app::SessionState::RecordingPaused);
    CHECK(CheckFits(controls.hwnd(), "recording, paused") >= 2);
    controls.SetSessionState(app::SessionState::RecordingFinalizing);
    CHECK(CheckFits(controls.hwnd(), "recording, finishing") >= 1);
    controls.Destroy();
}

void ScrollshotIsland(const RECT& frame) {
    ui::ScrollshotControls controls({});
    CHECK(controls.Show(ui::ScrollshotControls::View::Start, frame, nullptr));
    CHECK(CheckFits(controls.hwnd(), "scrollshot, start") >= 1);
    CHECK(controls.Show(ui::ScrollshotControls::View::Capture, frame, nullptr));
    const std::wstring size = strings::Format(IDS_SCROLLSHOT_SIZE, {L"3840", L"32000"});
    for (const UINT status : {IDS_SCROLLSHOT_STATUS_SCROLL, IDS_SCROLLSHOT_STATUS_LOST, IDS_SCROLLSHOT_STATUS_LIMIT,
                              IDS_SCROLLSHOT_STATUS_CAPTURE_FAILED, IDS_SCROLLSHOT_STATUS_DISPLAY_CHANGED,
                              IDS_SCROLLSHOT_STATUS_WHEEL, IDS_SCROLLSHOT_STATUS_STOPPING,
                              IDS_SCROLLSHOT_STATUS_COPY_FAILED, IDS_SCROLLSHOT_STATUS_SAVE_FAILED,
                              IDS_SCROLLSHOT_STATUS_READY, IDS_SCROLLSHOT_STATUS_COPYING, IDS_SCROLLSHOT_STATUS_SAVING}) {
        controls.SetStatus(size, strings::Get(status), false);
        CHECK(CheckFits(controls.hwnd(), "scrollshot, status") >= 2);
    }
    controls.Destroy();
}

void EditorIslands(const RECT& region) {
    app::AppSettings settings;
    editor::ScreenshotEditor* self = nullptr;
    editor::ScreenshotEditor::Callbacks callbacks;
    callbacks.on_prepare_image = [&] {
        platform::BgraImage image;
        image.Reset(static_cast<int>(platform::RectWidth(region)), static_cast<int>(platform::RectHeight(region)));
        std::fill(image.pixels.begin(), image.pixels.end(), 0xffe0e0e0u);
        return self->SetImage(std::move(image));
    };
    editor::ScreenshotEditor editor(std::move(callbacks));
    self = &editor;
    CHECK(editor.Open(region, &settings, nullptr));
    int examined = 0;
    // Each tool brings its own options with value labels.
    for (const editor::ToolId tool : {editor::ToolId::Rectangle, editor::ToolId::Arrow, editor::ToolId::Pen,
                                      editor::ToolId::Text, editor::ToolId::Tag, editor::ToolId::Mosaic,
                                      editor::ToolId::Spotlight, editor::ToolId::Watermark}) {
        ::SendMessageW(::GetDlgItem(editor.hwnd(), 2000 + static_cast<int>(tool)), BM_CLICK, 0, 0);
        for (HWND island : ThreadWindows(L"EasnapScreenshotEditor")) { examined += CheckFits(island, "editor"); }
        // Value labels at both ends of every slider: the longest values.
        for (HWND island : ThreadWindows(L"EasnapScreenshotEditor")) {
            std::vector<HWND> sliders;
            ::EnumChildWindows(island, [](HWND child, LPARAM value) -> BOOL {
                wchar_t name[32]{};
                ::GetClassNameW(child, name, ARRAYSIZE(name));
                if (::IsWindowVisible(child) && ::lstrcmpiW(name, TRACKBAR_CLASSW) == 0) {
                    reinterpret_cast<std::vector<HWND>*>(value)->push_back(child);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&sliders));
            for (HWND slider : sliders) {
                for (const UINT limit : {TBM_GETRANGEMIN, TBM_GETRANGEMAX}) {
                    const LRESULT position = ::SendMessageW(slider, limit, 0, 0);
                    ::SendMessageW(slider, TBM_SETPOS, TRUE, position);
                    ::SendMessageW(island, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, position), reinterpret_cast<LPARAM>(slider));
                    examined += CheckFits(island, "editor, slider at its limit");
                }
            }
        }
    }
    CHECK(examined >= 6);  // The effect and watermark tools have value labels.
    editor.Destroy();
}

HWND g_checked_dialog = nullptr;

/// The Settings dialog with every folder set, so the path labels and both
/// Reset buttons are in play. Checked from inside its own message loop.
void Settings() {
    ui::SettingsDialogModel model;
    model.image_folder = L"D:\\Some\\Rather\\Long\\Folder\\For\\Screenshots";
    model.video_folder = L"D:\\Videos";
    static int examined = 0;
    examined = 0;
    ::SetTimer(nullptr, 0, 20, [](HWND, UINT, UINT_PTR id, DWORD) {
        HWND dialog = nullptr;
        ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
            wchar_t name[32]{};
            ::GetClassNameW(window, name, ARRAYSIZE(name));
            if (::lstrcmpW(name, L"#32770") == 0 && ::IsWindowVisible(window)) { *reinterpret_cast<HWND*>(value) = window; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&dialog));
        if (dialog == nullptr) { return; }
        ::KillTimer(nullptr, id);
        examined = CheckFits(dialog, "settings");
        // The hint under the shortcut is the longest text the dialog can show.
        ::SendMessageW(::GetDlgItem(dialog, ui::kSettingsShortcut), BM_CLICK, 0, 0);
        examined += CheckFits(dialog, "settings, waiting for a shortcut");
        ::SendMessageW(::GetDlgItem(dialog, ui::kSettingsShortcut), WM_KEYDOWN, VK_ESCAPE, 0);
        ::SendMessageW(dialog, WM_CLOSE, 0, 0);
    });
    CHECK(!ui::ShowSettingsDialog(nullptr, model, {}));
    CHECK(examined >= 30);
}

void ModeBar(const platform::MonitorInfo& monitor) {
    capture::DesktopSnapshot snapshot;
    snapshot.virtual_bounds = platform::VirtualScreenBounds();
    for (const auto& each : platform::EnumerateMonitors()) {
        capture::MonitorFrame frame;
        frame.desktop_rect = each.bounds;
        frame.image.Reset(static_cast<int>(platform::RectWidth(each.bounds)), static_cast<int>(platform::RectHeight(each.bounds)));
        snapshot.monitors.push_back(std::move(frame));
    }
    ui::OverlayController overlay({});
    CHECK(overlay.Show(snapshot, POINT{monitor.bounds.left + 200, monitor.bounds.top + 200}));
    int examined = 0;
    for (HWND bar : ThreadWindows(L"EasnapCaptureModes")) { examined += CheckFits(bar, "mode bar"); }
    CHECK(examined == 3);
    overlay.Destroy();
}

}  // namespace

int main() {
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    if (!monitors.empty()) {
        const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
        const RECT region{monitor.bounds.left + 300, monitor.bounds.top + 160, monitor.bounds.left + 940, monitor.bounds.top + 520};
        struct Language { strings::LanguagePreference preference; const char* name; };
        for (const Language& language : {Language{strings::LanguagePreference::English, "English"},
                                         Language{strings::LanguagePreference::Turkish, "Turkish"}}) {
            strings::SetPreference(language.preference);
            g_language = language.name;
            RecordingIsland(region);
            ScrollshotIsland(region);
            EditorIslands(region);
            ModeBar(monitor);
            Settings();
        }
    }
    platform::GraphicsEnvironment::Get().Shutdown();
    ::CoUninitialize();
    std::printf("text fit: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
