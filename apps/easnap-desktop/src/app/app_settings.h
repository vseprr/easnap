// Easnap - user-visible preferences persisted to an INI file.
#pragma once

#include <windows.h>
#include <string>

#include "app/recording_workflow.h"
#include "app/appearance_theme.h"
#include "media/media_interfaces.h"

namespace easnap::app {

/// The interface language: what Windows uses, or a fixed choice.
enum class LanguageSetting { System, English, Turkish };

[[nodiscard]] constexpr bool IsValidLanguageSetting(LanguageSetting language) noexcept {
    return language == LanguageSetting::System || language == LanguageSetting::English ||
           language == LanguageSetting::Turkish;
}

/// User-visible preferences, persisted as an INI file next to the session data.
struct AppSettings {
    /// The explicit choice. It applies when `appearance_follows_system` is off.
    AppearanceTheme appearance_theme{AppearanceTheme::Light};
    /// Light or Dark as Windows is set for applications. The default.
    bool appearance_follows_system{true};
    LanguageSetting language{LanguageSetting::System};
    UINT hotkey_modifiers{MOD_CONTROL | MOD_SHIFT};
    UINT hotkey_virtual_key{'A'};
    bool capture_cursor{false};
    media::RecordingFormat recording_format{media::RecordingFormat::Mp4};
    media::AudioSource recording_audio_source{media::AudioSource::None};
    GifExportConfiguration recording_gif{};
    /// Where screenshots and recordings are saved by default. Empty: the
    /// folder used last, and Pictures or Videos the first time.
    std::wstring image_folder;
    std::wstring video_folder;
    /// Save a screenshot into `image_folder` at once, without the dialog.
    bool quick_save_images{false};
    std::wstring last_image_directory;
    std::wstring last_video_directory;
    /// The "Easnap is ready" notice has been shown on this computer.
    bool first_run_notice_shown{false};

    /// The folder a Save dialog for an image or a recording starts in.
    [[nodiscard]] const std::wstring& ImageStartDirectory() const noexcept {
        return image_folder.empty() ? last_image_directory : image_folder;
    }
    [[nodiscard]] const std::wstring& VideoStartDirectory() const noexcept {
        return video_folder.empty() ? last_video_directory : video_folder;
    }

    [[nodiscard]] static std::wstring FilePath();
    [[nodiscard]] static AppSettings Load(const std::wstring& custom_path = {});
    bool Save(const std::wstring& custom_path = {}) const;

    /// Human readable shortcut, e.g. "Ctrl + Shift + A".
    [[nodiscard]] std::wstring HotkeyDisplayText() const;
};

}  // namespace easnap::app
