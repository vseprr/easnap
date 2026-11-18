#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace easnap::ui {

/// The capture shortcut as About reports it.
struct AboutShortcut {
    /// The shortcut as the user reads it, such as Ctrl + Shift + A.
    std::wstring display_text;
    /// False when another application holds the shortcut.
    bool registered{false};
};

/// About: version, shortcut and Windows build, with Copy details and Website.
/// The shortcut is asked for again when the details are copied.
void ShowAboutDialog(HWND owner, const std::function<AboutShortcut()>& shortcut);

}  // namespace easnap::ui
