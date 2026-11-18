// Easnap - the Settings dialog.
//
// A fixed, modal dialog on the same path as the message dialog: shared window
// chrome, shared styled controls, real default and cancel buttons. It edits a
// copy of the values; the caller applies them when the dialog is accepted.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "app/app_settings.h"
#include "ui/control_ids.h"

namespace easnap::ui {

struct SettingsDialogModel {
    app::LanguageSetting language{app::LanguageSetting::System};
    bool appearance_follows_system{true};
    app::AppearanceTheme appearance_theme{app::AppearanceTheme::Light};
    UINT hotkey_modifiers{MOD_CONTROL | MOD_SHIFT};
    UINT hotkey_virtual_key{'A'};
    bool start_with_windows{false};
    bool capture_cursor{false};
    std::wstring image_folder;
    std::wstring video_folder;
    bool quick_save_images{false};
};

struct SettingsDialogCallbacks {
    /// True when the shortcut could be registered: it is free, or it is the
    /// one Easnap holds now.
    std::function<bool(UINT modifiers, UINT virtual_key)> shortcut_available;
    /// Lets the user pick a folder; empty when cancelled. `current` is where
    /// the picker starts.
    std::function<std::wstring(HWND owner, const std::wstring& current)> choose_folder;
};

/// Shows the dialog. True when it was accepted; `model` then holds the new
/// values. False leaves `model` untouched.
[[nodiscard]] bool ShowSettingsDialog(HWND owner, SettingsDialogModel& model,
                                      const SettingsDialogCallbacks& callbacks);

/// "Ctrl + Shift + A" for a shortcut.
[[nodiscard]] std::wstring ShortcutText(UINT modifiers, UINT virtual_key);

/// The system folder picker. Empty when cancelled.
[[nodiscard]] std::wstring ChooseFolder(HWND owner, const std::wstring& current, const wchar_t* title);

}  // namespace easnap::ui
