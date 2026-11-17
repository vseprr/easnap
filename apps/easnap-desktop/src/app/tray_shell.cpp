#include "app/tray_shell.h"

// windows.h, from the header, comes before shellapi.h.
#include <shellapi.h>

#include "app/session_state.h"
#include "platform/files.h"
#include "platform/platform_utils.h"
#include "platform/startup_entry.h"
#include "resource.h"
#include "ui/about_dialog.h"
#include "ui/control_ids.h"
#include "ui/message_dialog.h"
#include "ui/native_controls.h"
#include "ui/settings_dialog.h"
#include "ui/tray_menu.h"

namespace easnap::app {
namespace {

// "Check for updates": a stable build opens the latest stable release; a
// prerelease opens the list, where newer prereleases appear too.
#if EASNAP_VERSION_IS_PRERELEASE
constexpr wchar_t kUpdatesPage[] = L"https://github.com/vseprr/easnap/releases";
#else
constexpr wchar_t kUpdatesPage[] = L"https://github.com/vseprr/easnap/releases/latest";
#endif
constexpr int kProbeHotkeyId = 2;

easnap::ui::TrayIcon g_tray;
bool g_hotkey_registered = false;
}  // namespace

ui::TrayIcon& ShellTrayIcon() { return g_tray; }

/// The tooltip says how to capture: with the shortcut, or by clicking when
/// another application holds the shortcut.
std::wstring TrayTooltip() {
    return easnap::strings::Format(g_hotkey_registered ? IDS_SHELL_TRAY_TOOLTIP : IDS_TRAY_TOOLTIP_NO_SHORTCUT,
                                   {CaptureCoordinator::Instance().settings().HotkeyDisplayText()});
}

bool RegisterCaptureHotkey(HWND hwnd) {
    const auto& settings = CaptureCoordinator::Instance().settings();
    g_hotkey_registered = ::RegisterHotKey(hwnd, IDH_EASNAP_CAPTURE, settings.hotkey_modifiers | MOD_NOREPEAT,
                                           settings.hotkey_virtual_key) != FALSE;
    if (!g_hotkey_registered) {
        easnap::platform::LogMessage(L"RegisterHotKey failed (error %lu)", ::GetLastError());
    }
    return g_hotkey_registered;
}

void UnregisterCaptureHotkey(HWND hwnd) {
    if (g_hotkey_registered) {
        ::UnregisterHotKey(hwnd, IDH_EASNAP_CAPTURE);
        g_hotkey_registered = false;
    }
}

easnap::strings::LanguagePreference ToLanguagePreference(easnap::app::LanguageSetting language) {
    switch (language) {
        case easnap::app::LanguageSetting::English: return easnap::strings::LanguagePreference::English;
        case easnap::app::LanguageSetting::Turkish: return easnap::strings::LanguagePreference::Turkish;
        case easnap::app::LanguageSetting::System: break;
    }
    return easnap::strings::LanguagePreference::System;
}

namespace {

/// The Settings dialog, and what accepting it changes outside the settings
/// file: the registered shortcut, the startup entry, language and appearance.
void ShowSettings(HWND hwnd) {
    namespace strings = easnap::strings;
    auto& coordinator = CaptureCoordinator::Instance();
    if (coordinator.state() != SessionState::Idle) {
        return;
    }
    auto& settings = coordinator.settings();
    const std::wstring executable = easnap::platform::CurrentExecutablePath();
    easnap::ui::SettingsDialogModel model;
    model.language = settings.language;
    model.appearance_follows_system = settings.appearance_follows_system;
    model.appearance_theme = settings.appearance_theme;
    model.hotkey_modifiers = settings.hotkey_modifiers;
    model.hotkey_virtual_key = settings.hotkey_virtual_key;
    model.start_with_windows = easnap::platform::StartupEntryEnabled(executable);
    model.capture_cursor = settings.capture_cursor;
    model.image_folder = settings.image_folder;
    model.video_folder = settings.video_folder;
    model.quick_save_images = settings.quick_save_images;
    const bool started_with_windows = model.start_with_windows;

    easnap::ui::SettingsDialogCallbacks callbacks;
    // Registering under a second identifier tells whether the combination is
    // free without giving up the shortcut that works now.
    callbacks.shortcut_available = [hwnd](UINT modifiers, UINT key) {
        if (::RegisterHotKey(hwnd, kProbeHotkeyId, modifiers | MOD_NOREPEAT, key) == FALSE) {
            return false;
        }
        ::UnregisterHotKey(hwnd, kProbeHotkeyId);
        return true;
    };
    callbacks.choose_folder = [](HWND owner, const std::wstring& current) {
        return easnap::ui::ChooseFolder(owner, current, easnap::strings::Get(IDS_SETTINGS_FOLDER_PICK_TITLE));
    };
    // The shortcut is given up while the dialog is open. Windows hands a
    // registered combination to its owner and never to the window that wants
    // to read it, and Easnap's own is the one most likely to be pressed here.
    UnregisterCaptureHotkey(hwnd);
    if (!easnap::ui::ShowSettingsDialog(nullptr, model, callbacks) || coordinator.state() != SessionState::Idle) {
        RegisterCaptureHotkey(hwnd);
        g_tray.SetTooltip(TrayTooltip());
        return;
    }

    settings.language = model.language;
    strings::SetPreference(ToLanguagePreference(settings.language));
    settings.appearance_follows_system = model.appearance_follows_system;
    settings.appearance_theme = model.appearance_theme;
    coordinator.RefreshAppearance();
    settings.capture_cursor = model.capture_cursor;
    settings.image_folder = model.image_folder;
    settings.video_folder = model.video_folder;
    settings.quick_save_images = model.quick_save_images;
    const UINT previous_modifiers = settings.hotkey_modifiers;
    const UINT previous_key = settings.hotkey_virtual_key;
    settings.hotkey_modifiers = model.hotkey_modifiers;
    settings.hotkey_virtual_key = model.hotkey_virtual_key;
    if (!RegisterCaptureHotkey(hwnd) &&
        (previous_modifiers != settings.hotkey_modifiers || previous_key != settings.hotkey_virtual_key)) {
        // Taken between the check and now: keep what worked before.
        settings.hotkey_modifiers = previous_modifiers;
        settings.hotkey_virtual_key = previous_key;
        RegisterCaptureHotkey(hwnd);
    }
    const bool startup_refused = model.start_with_windows != started_with_windows &&
                                 !easnap::platform::SetStartupEntry(model.start_with_windows, executable);
    const bool saved = coordinator.PersistSettings();
    g_tray.SetTooltip(TrayTooltip());
    easnap::platform::LogMessage(L"Settings changed");
    if (startup_refused) {
        // The next visit to Settings shows the entry as it really is.
        easnap::platform::LogMessage(L"The startup entry could not be changed");
    }
    if (!saved) {
        // Said here instead of in a notification. Marked first: the dialog's
        // own message loop would otherwise show the notification as well.
        coordinator.MarkSettingsFailureTold();
        easnap::ui::ShowErrorMessage(
            nullptr, strings::Get(startup_refused ? IDS_SETTINGS_STARTUP_AND_SAVE_FAILED : IDS_SETTINGS_SAVE_FAILED));
    } else if (startup_refused) {
        easnap::ui::ShowErrorMessage(nullptr, strings::Get(IDS_SETTINGS_STARTUP_FAILED));
    }
}

void OpenDataFolder() {
    const std::wstring directory = easnap::platform::AppDataDirectory();
    if (directory.empty()) {
        return;
    }
    if (!easnap::platform::EnsureDirectory(directory)) {
        // A file may hold the folder's name: there is nothing to open.
        easnap::platform::LogMessage(L"The data folder could not be created (error %lu)", ::GetLastError());
        return;
    }
    ::ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/// Opens the tray menu with the coordinator's current state.
void OpenTrayMenu(HWND hwnd) {
    auto& coordinator = CaptureCoordinator::Instance();
    easnap::ui::TrayMenuState state;
    state.hotkey_registered = g_hotkey_registered;
    state.hotkey_text = coordinator.settings().HotkeyDisplayText();
    state.idle = coordinator.state() == SessionState::Idle;
    state.can_open_video = coordinator.CanOpenVideo();
    state.capture_cursor = coordinator.settings().capture_cursor;
    state.appearance_follows_system = coordinator.settings().appearance_follows_system;
    easnap::ui::ShowTrayMenu(hwnd, state);
}

void OnTrayCommand(HWND hwnd, UINT command) {
    auto& coordinator = CaptureCoordinator::Instance();
    switch (command) {
        case IDM_TRAY_CAPTURE:
            coordinator.OnGlobalHotkey();
            break;
        case IDM_TRAY_OPEN_VIDEO:
            coordinator.OpenVideo();
            break;
        case IDM_TRAY_CAPTURE_CURSOR:
            coordinator.settings().capture_cursor = !coordinator.settings().capture_cursor;
            coordinator.PersistSettings();
            break;
        case IDM_TRAY_OPEN_DATA_FOLDER:
            OpenDataFolder();
            break;
        case IDM_TRAY_SETTINGS:
            ShowSettings(hwnd);
            break;
        case IDM_TRAY_CHECK_UPDATES:
            // The browser opens the releases page. Easnap itself asks nobody.
            ::ShellExecuteW(nullptr, L"open", kUpdatesPage, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_TRAY_ABOUT:
            easnap::ui::ShowAboutDialog(hwnd, [] {
                return easnap::ui::AboutShortcut{CaptureCoordinator::Instance().settings().HotkeyDisplayText(), g_hotkey_registered};
            });
            break;
        case IDM_TRAY_EXIT:
            if (coordinator.ConfirmExit()) { ::DestroyWindow(hwnd); }
            break;
        default:
            break;
    }
}

}  // namespace

/// Says once per run that the settings cannot be saved: in a notification, or
/// in a dialog when there is no icon to carry one. A notification the shell
/// refused is tried again after the next failed save.
void TellSettingsNotSaved() {
    namespace strings = easnap::strings;
    auto& coordinator = CaptureCoordinator::Instance();
    if (g_tray.ShowBalloon(kProductName, strings::Get(IDS_SETTINGS_NOT_SAVED_NOTICE))) {
        coordinator.MarkSettingsFailureTold();
    } else if (!g_tray.added()) {
        // Marked first: the dialog's own message loop may handle another notice.
        coordinator.MarkSettingsFailureTold();
        easnap::ui::ShowErrorMessage(nullptr, strings::Get(IDS_SETTINGS_SAVE_FAILED));
    }
}

/// The notification icon's own messages: a click captures, a right-click opens
/// the menu.
LRESULT OnTrayIconMessage(HWND hwnd, LPARAM lparam) {
    auto& coordinator = CaptureCoordinator::Instance();
    switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
            coordinator.OnGlobalHotkey();
            break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            // Version 4 icons report one right-click as both messages;
            // the second arrives inside the first menu's modal loop.
            if (!easnap::ui::PopupActive()) { OpenTrayMenu(hwnd); }
            break;
        default:
            break;
    }
    return 0;
}

/// A tray menu command, including the appearance submenu's.
LRESULT OnTrayMenuCommand(HWND hwnd, WPARAM wparam) {
    auto& coordinator = CaptureCoordinator::Instance();
    if (LOWORD(wparam) == easnap::ui::kAppearanceLight || LOWORD(wparam) == easnap::ui::kAppearanceDark) {
        coordinator.SetAppearanceTheme(LOWORD(wparam) == easnap::ui::kAppearanceDark
                                           ? easnap::app::AppearanceTheme::Dark
                                           : easnap::app::AppearanceTheme::Light);
        return 0;
    }
    if (LOWORD(wparam) == easnap::ui::kAppearanceSystem) {
        coordinator.SetAppearanceFollowsSystem();
        return 0;
    }
    OnTrayCommand(hwnd, LOWORD(wparam));
    return 0;
}

}  // namespace easnap::app
