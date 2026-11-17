#pragma once

#include <windows.h>

#include <string>

#include "app/app_settings.h"
#include "strings/strings.h"
#include "ui/tray_icon.h"

// The notification-area shell: the tray icon, the capture shortcut, the tray
// menu's commands, Settings and About. It runs on the host window's thread.
namespace easnap::app {

/// The product name: the host window's title and the title of notifications.
inline constexpr wchar_t kProductName[] = L"Easnap";

/// The notification-area icon, owned for the process's lifetime.
[[nodiscard]] ui::TrayIcon& ShellTrayIcon();

/// The tooltip says how to capture: with the shortcut, or by clicking when
/// another application holds the shortcut.
std::wstring TrayTooltip();

/// Registers the capture shortcut from the settings; false when another
/// application holds it.
bool RegisterCaptureHotkey(HWND hwnd);
/// Gives up the capture shortcut when it is registered.
void UnregisterCaptureHotkey(HWND hwnd);
/// The string module's preference for a language setting.
easnap::strings::LanguagePreference ToLanguagePreference(easnap::app::LanguageSetting language);

/// Says once per run that the settings cannot be saved: in a notification, or
/// in a dialog when there is no icon to carry one. A notification the shell
/// refused is tried again after the next failed save.
void TellSettingsNotSaved();

/// The notification icon's own messages: a click captures, a right-click opens
/// the menu.
LRESULT OnTrayIconMessage(HWND hwnd, LPARAM lparam);

/// A tray menu command, including the appearance submenu's.
LRESULT OnTrayMenuCommand(HWND hwnd, WPARAM wparam);

}  // namespace easnap::app
