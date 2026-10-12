// Easnap integration tests - questions asked before work is lost.
//
// Production dialogs and the production editor, driven with messages on a
// private desktop: the dialog whose buttons are named after what they do, the
// tracking of dialogs that run their own message loop, and the editor asking
// before annotations or a stitched image are discarded.

#include "native_test_monitor.h"
#include "scrollshot_synthetic.h"

#include "editor/screenshot_editor.h"
#include "strings/strings.h"
#include "ui/native_controls.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace easnap;

int passed{}, failed{};
#define CHECK(...) do { if (__VA_ARGS__) { ++passed; } else { ++failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (false)

std::wstring WindowText(HWND window) {
    wchar_t text[256]{};
    ::GetWindowTextW(window, text, ARRAYSIZE(text));
    return text;
}

HWND FindDialog() {
    HWND dialog = nullptr;
    ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        wchar_t name[32]{};
        ::GetClassNameW(window, name, ARRAYSIZE(name));
        if (::lstrcmpW(name, L"#32770") == 0 && ::IsWindowVisible(window)) {
            *reinterpret_cast<HWND*>(value) = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&dialog));
    return dialog;
}

/// Runs `inside` once from the message loop of the dialog that is shown next.
std::function<void(HWND)> g_inside;
int g_dialogs_seen = 0;
void OnNextDialog(std::function<void(HWND)> inside) {
    g_inside = std::move(inside);
    ::SetTimer(nullptr, 0, 20, [](HWND, UINT, UINT_PTR id, DWORD) {
        HWND dialog = FindDialog();
        if (dialog == nullptr) { return; }  // Not shown yet: the timer fires again.
        ::KillTimer(nullptr, id);
        ++g_dialogs_seen;
        auto inside = std::move(g_inside);
        g_inside = nullptr;
        if (inside) { inside(dialog); }
    });
}

/// Push buttons of a dialog, left to right.
std::vector<HWND> Buttons(HWND dialog) {
    std::vector<HWND> buttons;
    for (HWND child = ::GetWindow(dialog, GW_CHILD); child != nullptr; child = ::GetWindow(child, GW_HWNDNEXT)) {
        wchar_t name[16]{};
        ::GetClassNameW(child, name, ARRAYSIZE(name));
        if (::lstrcmpiW(name, L"BUTTON") == 0 && ::GetDlgCtrlID(child) > 0) { buttons.push_back(child); }
    }
    std::sort(buttons.begin(), buttons.end(), [](HWND a, HWND b) {
        RECT ra{}, rb{};
        ::GetWindowRect(a, &ra);
        ::GetWindowRect(b, &rb);
        return ra.left < rb.left;
    });
    return buttons;
}

// --- Dialogs ---------------------------------------------------------------------

void Dialogs() {
    CHECK(!platform::ModalLoopActive());

    // Three named answers: in the given order, the default focused, nothing
    // clipped, all inside the dialog.
    const std::vector<ui::DialogChoice> choices{
        {100, L"Save..."}, {101, L"Delete", true}, {102, L"A label that is longer than a standard button"}};
    OnNextDialog([&](HWND dialog) {
        CHECK(platform::ModalLoopActive());
        CHECK(WindowText(dialog) == L"Unsaved recording");
        const std::vector<HWND> buttons = Buttons(dialog);
        CHECK(buttons.size() == 3);
        RECT client{};
        ::GetClientRect(dialog, &client);
        ::MapWindowPoints(dialog, nullptr, reinterpret_cast<POINT*>(&client), 2);
        LONG previous_right = client.left;
        for (size_t i = 0; i < buttons.size() && i < choices.size(); ++i) {
            CHECK(::GetDlgCtrlID(buttons[i]) == choices[i].value && WindowText(buttons[i]) == choices[i].label);
            RECT rect{};
            ::GetWindowRect(buttons[i], &rect);
            CHECK(rect.left >= previous_right && rect.right <= client.right);
            previous_right = rect.right;
            HDC dc = ::GetDC(buttons[i]);
            const HGDIOBJ old = ::SelectObject(dc, reinterpret_cast<HFONT>(::SendMessageW(buttons[i], WM_GETFONT, 0, 0)));
            SIZE text{};
            ::GetTextExtentPoint32W(dc, choices[i].label, ::lstrlenW(choices[i].label), &text);
            ::SelectObject(dc, old);
            ::ReleaseDC(buttons[i], dc);
            CHECK(rect.right - rect.left >= text.cx);  // The label fits its button.
        }
        CHECK(::GetDlgCtrlID(::GetFocus()) == 100);
        if (buttons.size() == 3) { ::SendMessageW(buttons[1], BM_CLICK, 0, 0); }
    });
    CHECK(ui::ShowChoice(nullptr, L"Easnap found a recording.", L"Unsaved recording", choices, 100, 102) == 101);
    CHECK(!platform::ModalLoopActive());

    // Esc and the close button give the cancel answer, whatever has the focus.
    OnNextDialog([](HWND dialog) { ::SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0); });
    CHECK(ui::ShowChoice(nullptr, L"Text", L"Title", choices, 100, 102) == 102);
    OnNextDialog([](HWND dialog) { ::SendMessageW(dialog, WM_CLOSE, 0, 0); });
    CHECK(ui::ShowChoice(nullptr, L"Text", L"Title", choices, 101, 102) == 102);
    CHECK(ui::ShowChoice(nullptr, L"Text", L"Title", {}, 1, 77) == 77);  // Nothing to choose from.

    // "Really lose this?": the keeping answer is the default and the answer to Esc.
    OnNextDialog([](HWND dialog) {
        const std::vector<HWND> buttons = Buttons(dialog);
        CHECK(buttons.size() == 2);
        if (buttons.size() == 2) {
            CHECK(WindowText(buttons[0]) == L"Discard" && WindowText(buttons[1]) == L"Keep");
            CHECK(::GetFocus() == buttons[1]);
            CHECK((::SendMessageW(dialog, DM_GETDEFID, 0, 0) & 0xffff) == IDNO);
        }
        ::SendMessageW(dialog, WM_CLOSE, 0, 0);
    });
    CHECK(!ui::ConfirmDestructive(nullptr, L"Discard this screenshot?", L"Discard screenshot", L"Discard", L"Keep"));
    OnNextDialog([](HWND dialog) { ::SendMessageW(::GetDlgItem(dialog, IDNO), BM_CLICK, 0, 0); });
    CHECK(!ui::ConfirmDestructive(nullptr, L"Discard this screenshot?", L"Discard screenshot", L"Discard", L"Keep"));
    OnNextDialog([](HWND dialog) { ::SendMessageW(::GetDlgItem(dialog, IDYES), BM_CLICK, 0, 0); });
    CHECK(ui::ConfirmDestructive(nullptr, L"Discard this screenshot?", L"Discard screenshot", L"Discard", L"Keep"));

    // The standard buttons take their labels from the string table.
    OnNextDialog([](HWND dialog) {
        const std::vector<HWND> buttons = Buttons(dialog);
        CHECK(buttons.size() == 2 && WindowText(buttons[0]) == L"Yes" && WindowText(buttons[1]) == L"No");
        CHECK(::GetDlgCtrlID(::GetFocus()) == IDNO);
        ::SendMessageW(::GetDlgItem(dialog, IDYES), BM_CLICK, 0, 0);
    });
    CHECK(ui::ShowMessage(nullptr, L"Question?", L"Easnap", MB_YESNO | MB_DEFBUTTON2) == IDYES);
    strings::SetPreference(strings::LanguagePreference::Turkish);
    OnNextDialog([](HWND dialog) {
        const std::vector<HWND> buttons = Buttons(dialog);
        CHECK(buttons.size() == 2 && WindowText(buttons[0]) == L"Tamam" && WindowText(buttons[1]) == L"İptal");
        ::SendMessageW(dialog, WM_CLOSE, 0, 0);
    });
    CHECK(ui::ShowMessage(nullptr, L"Soru?", L"Easnap", MB_OKCANCEL) == IDCANCEL);
    strings::SetPreference(strings::LanguagePreference::English);
    CHECK(g_dialogs_seen == 8);
}

// --- The editor asks before it discards -----------------------------------------------

platform::BgraImage Picture(int width, int height) {
    const easnap_test::synthetic::Image source = easnap_test::synthetic::MakeArticle(width, height, 31);
    platform::BgraImage image;
    image.Reset(width, height);
    std::copy(source.pixels.begin(), source.pixels.end(), image.pixels.begin());
    return image;
}

void Draw(editor::ScreenshotEditor& editor, POINT from, POINT to) {
    editor.ImagePointer(WM_LBUTTONDOWN, from);
    editor.ImagePointer(WM_MOUSEMOVE, to);
    editor.ImagePointer(WM_LBUTTONUP, to);
}

void EditorDiscard() {
    const auto monitors = platform::EnumerateMonitors();
    CHECK(!monitors.empty());
    if (monitors.empty()) { return; }
    const platform::MonitorInfo monitor = test::SelectedTestMonitor() ? *test::SelectedTestMonitor() : monitors.front();
    const RECT region{monitor.bounds.left + 200, monitor.bounds.top + 150, monitor.bounds.left + 600, monitor.bounds.top + 450};

    app::AppSettings settings;
    int closed = 0, asked = 0;
    bool answer = false;
    editor::ScreenshotEditor* self = nullptr;
    std::function<void()> while_asking;
    editor::ScreenshotEditor::Callbacks callbacks;
    callbacks.on_closed = [&](bool) { ++closed; };
    callbacks.on_prepare_image = [&] { return self->SetImage(Picture(400, 300)); };
    callbacks.on_confirm_discard = [&] {
        ++asked;
        if (while_asking) { while_asking(); }
        return answer;
    };
    const auto rectangle = [&](editor::ScreenshotEditor& editor) {
        ::SendMessageW(::GetDlgItem(editor.hwnd(), 2000 + static_cast<int>(editor::ToolId::Rectangle)), BM_CLICK, 0, 0);
    };

    // An untouched screenshot can be taken again: Esc closes without a question.
    {
        editor::ScreenshotEditor editor(callbacks);
        self = &editor;
        CHECK(editor.Open(region, &settings, nullptr));
        rectangle(editor);  // Fixes the image; draws nothing.
        CHECK(editor.has_image() && !editor.has_annotations());
        editor.Keyboard(WM_KEYDOWN, VK_ESCAPE, 0);
        CHECK(asked == 0 && closed == 1 && !editor.visible());
        editor.Destroy();
    }

    // With something drawn, every way out asks first.
    {
        editor::ScreenshotEditor editor(callbacks);
        self = &editor;
        closed = 0;
        CHECK(editor.Open(region, &settings, nullptr));
        rectangle(editor);
        Draw(editor, {region.left + 40, region.top + 40}, {region.left + 200, region.top + 160});
        CHECK(editor.has_annotations());
        ::SendMessageW(::GetDlgItem(editor.hwnd(), 2000 + static_cast<int>(editor::ToolId::Pointer)), BM_CLICK, 0, 0);
        answer = false;
        editor.Keyboard(WM_KEYDOWN, VK_ESCAPE, 0);  // Clears the selection the drawing left, if there is one.
        asked = 0;
        const size_t drawn = editor.document().annotations().size();
        CHECK(closed == 0 && editor.visible());

        // While the question is open the workspace neither draws nor asks again.
        while_asking = [&] {
            rectangle(editor);
            Draw(editor, {region.left + 220, region.top + 40}, {region.left + 380, region.top + 160});
            editor.Keyboard(WM_KEYDOWN, VK_ESCAPE, 0);
            ::SendMessageW(editor.hwnd(), WM_CLOSE, 0, 0);
        };
        editor.Keyboard(WM_KEYDOWN, VK_ESCAPE, 0);
        CHECK(asked == 1 && closed == 0 && editor.visible());
        CHECK(editor.document().annotations().size() == drawn);
        while_asking = nullptr;

        ::SendMessageW(editor.hwnd(), WM_CLOSE, 0, 0);                              // A close request.
        ::SendMessageW(::GetDlgItem(editor.hwnd(), 2406), BM_CLICK, 0, 0);            // The Discard button.
        ::SendMessageW(editor.hwnd(), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);        // Esc from a child control.
        CHECK(asked == 4 && closed == 0 && editor.visible() && editor.has_annotations());

        answer = true;
        editor.Keyboard(WM_KEYDOWN, VK_ESCAPE, 0);
        CHECK(asked == 5 && closed == 1 && !editor.visible());
        editor.Destroy();
    }

    // A stitched image cannot be taken again: it asks even with nothing drawn.
    {
        editor::ScreenshotEditor editor(callbacks);
        self = &editor;
        closed = asked = 0;
        platform::BgraImage tall = Picture(400, 2400);
        const RECT area{monitor.bounds.left + 100, monitor.bounds.top + 60, monitor.bounds.left + 900, monitor.bounds.top + 660};
        CHECK(editor.OpenDocument(tall, area, area.left, &settings, nullptr));
        CHECK(editor.document_mode() && !editor.has_annotations());
        answer = false;
        editor.Keyboard(WM_KEYDOWN, VK_ESCAPE, 0);
        CHECK(asked == 1 && closed == 0 && editor.visible() && editor.has_image());
        answer = true;
        editor.Keyboard(WM_KEYDOWN, VK_ESCAPE, 0);
        CHECK(asked == 2 && closed == 1);
        editor.Destroy();
    }

    // Without a listener nothing is asked and Esc closes, as it did before.
    {
        editor::ScreenshotEditor::Callbacks plain;
        plain.on_closed = [&](bool) { ++closed; };
        plain.on_prepare_image = [&] { return self->SetImage(Picture(400, 300)); };
        editor::ScreenshotEditor editor(std::move(plain));
        self = &editor;
        closed = asked = 0;
        CHECK(editor.Open(region, &settings, nullptr));
        rectangle(editor);
        Draw(editor, {region.left + 40, region.top + 40}, {region.left + 200, region.top + 160});
        ::SendMessageW(editor.hwnd(), WM_CLOSE, 0, 0);
        CHECK(asked == 0 && closed == 1);
        editor.Destroy();
    }
}

}  // namespace

int main() {
    easnap::strings::SetPreference(easnap::strings::LanguagePreference::English);
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Dialogs();
    EditorDiscard();
    platform::GraphicsEnvironment::Get().Shutdown();
    ::CoUninitialize();
    std::printf("data safety: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
