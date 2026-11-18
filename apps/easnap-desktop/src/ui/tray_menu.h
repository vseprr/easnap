#pragma once

#include <windows.h>

#include <string>

namespace easnap::ui {

/// What the tray menu shows, read when it opens.
struct TrayMenuState {
    /// Whether the capture item names the shortcut.
    bool hotkey_registered{false};
    /// The shortcut the capture item names.
    std::wstring hotkey_text;
    /// Capture and Settings are offered while no session runs.
    bool idle{false};
    /// "Open video" is offered while one can be opened (ADR-040).
    bool can_open_video{false};
    /// The check mark of "Include cursor".
    bool capture_cursor{false};
    /// The appearance submenu marks its first entry, which follows Windows.
    bool appearance_follows_system{false};
};

/// Shows the tray menu at the pointer; the chosen command is posted to
/// hwnd as WM_COMMAND.
void ShowTrayMenu(HWND hwnd, const TrayMenuState& state);

}  // namespace easnap::ui
