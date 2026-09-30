// Easnap - user-visible preferences persisted to an INI file.
#pragma once

#include <windows.h>
#include <string>

#include "app/recording_workflow.h"
#include "app/appearance_theme.h"
#include "media/media_interfaces.h"

namespace easnap::app {

/// User-visible preferences, persisted as an INI file next to the session data.
struct AppSettings {
    AppearanceTheme appearance_theme{AppearanceTheme::Light};
    UINT hotkey_modifiers{MOD_CONTROL | MOD_SHIFT};
    UINT hotkey_virtual_key{'A'};
    bool capture_cursor{false};
    media::RecordingFormat recording_format{media::RecordingFormat::Mp4};
    media::AudioSource recording_audio_source{media::AudioSource::None};
    GifExportConfiguration recording_gif{};
    bool close_editor_after_copy{true};
    std::wstring last_save_directory;

    [[nodiscard]] static std::wstring FilePath();
    [[nodiscard]] static AppSettings Load(const std::wstring& custom_path = {});
    bool Save(const std::wstring& custom_path = {}) const;

    /// Human readable shortcut, e.g. "Ctrl + Shift + A".
    [[nodiscard]] std::wstring HotkeyDisplayText() const;
};

}  // namespace easnap::app
