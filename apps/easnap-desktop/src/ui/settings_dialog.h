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

// Control identifiers, for tests that drive the dialog with messages.
inline constexpr int kSettingsLanguage = 3101;
inline constexpr int kSettingsAppearance = 3102;
inline constexpr int kSettingsShortcut = 3103;
inline constexpr int kSettingsShortcutHint = 3104;
inline constexpr int kSettingsStartWithWindows = 3105;
inline constexpr int kSettingsCaptureCursor = 3106;
inline constexpr int kSettingsImageFolder = 3107;
inline constexpr int kSettingsImageChange = 3108;
inline constexpr int kSettingsImageReset = 3109;
inline constexpr int kSettingsQuickSave = 3110;
inline constexpr int kSettingsVideoFolder = 3111;
inline constexpr int kSettingsVideoChange = 3112;
inline constexpr int kSettingsVideoReset = 3113;

}  // namespace easnap::ui
