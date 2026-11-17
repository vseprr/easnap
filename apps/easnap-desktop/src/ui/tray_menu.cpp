#include "ui/tray_menu.h"

#include "resource.h"
#include "strings/strings.h"
#include "ui/native_controls.h"

namespace easnap::ui {

/// Shows the tray menu at the pointer; the chosen command is posted to
/// hwnd as WM_COMMAND.
void ShowTrayMenu(HWND hwnd, const TrayMenuState& state) {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    namespace strings = easnap::strings;
    // The tab in the pattern separates the label from the right-aligned shortcut.
    const std::wstring capture_item = state.hotkey_registered
                                          ? strings::Format(IDS_SHELL_TRAY_CAPTURE, {state.hotkey_text})
                                          : std::wstring(strings::Get(IDS_TRAY_CAPTURE_NO_SHORTCUT));
    const UINT capture_flags = state.idle ? MF_STRING : (MF_STRING | MF_GRAYED);

    ::AppendMenuW(menu, capture_flags, IDM_TRAY_CAPTURE, capture_item.c_str());
    // A video is opened only while idle, and one at a time (ADR-040).
    ::AppendMenuW(menu, state.can_open_video ? MF_STRING : (MF_STRING | MF_GRAYED), IDM_TRAY_OPEN_VIDEO,
                  strings::Get(IDS_TRAY_OPEN_VIDEO));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (state.capture_cursor ? MF_CHECKED : MF_UNCHECKED),
                  IDM_TRAY_CAPTURE_CURSOR, strings::Get(IDS_SHELL_TRAY_INCLUDE_CURSOR));
    if (HMENU appearance = easnap::ui::CreateAppearanceMenu(state.appearance_follows_system)) {
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(appearance), strings::Get(IDS_SHELL_TRAY_APPEARANCE));
    }
    // Settings change what a running session was built with: offered while idle.
    ::AppendMenuW(menu, capture_flags, IDM_TRAY_SETTINGS, strings::Get(IDS_TRAY_SETTINGS));
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_DATA_FOLDER, strings::Get(IDS_SHELL_TRAY_OPEN_DATA_FOLDER));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_CHECK_UPDATES, strings::Get(IDS_TRAY_CHECK_UPDATES));
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_ABOUT, strings::Get(IDS_SHELL_TRAY_ABOUT));
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, strings::Get(IDS_SHELL_TRAY_EXIT));

    POINT cursor{};
    ::GetCursorPos(&cursor);
    const HWND invoking_focus = ::GetFocus();
    // Required so the menu closes when the user clicks elsewhere.
    ::SetForegroundWindow(hwnd);
    {
        easnap::ui::PopupScope scope(invoking_focus);
        easnap::ui::MenuThemeScope menu_theme(hwnd, menu);
        const UINT command = ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                                cursor.x, cursor.y, hwnd, nullptr);
        if (command) { ::PostMessageW(hwnd, WM_COMMAND, command, 0); }
    }
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
}

}  // namespace easnap::ui
