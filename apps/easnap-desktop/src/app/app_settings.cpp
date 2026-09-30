// Easnap - user-visible preferences implementation.
#include "app/app_settings.h"

#include <cwchar>

#include "app/recording_workflow.h"
#include "platform/platform_utils.h"

namespace easnap::app {
namespace {

constexpr const wchar_t* kSettingsSection = L"Easnap";

void AppendModifier(std::wstring& text, const wchar_t* name) {
    if (!text.empty()) {
        text += L" + ";
    }
    text += name;
}

std::wstring VirtualKeyName(UINT virtual_key) {
    const UINT scan_code = ::MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    wchar_t buffer[64]{};
    // Bit 16..23 carry the scan code; bit 25 asks for a side-agnostic name.
    if (::GetKeyNameTextW(static_cast<LONG>(scan_code << 16) | (1L << 25), buffer, ARRAYSIZE(buffer)) > 0) {
        return buffer;
    }
    return std::wstring(1, static_cast<wchar_t>(virtual_key));
}

}  // namespace

std::wstring AppSettings::FilePath() {
    const std::wstring directory = platform::AppDataDirectory();
    if (directory.empty()) {
        return {};
    }
    return directory + L"\\settings.ini";
}

AppSettings AppSettings::Load(const std::wstring& custom_path) {
    AppSettings settings;
    const std::wstring path = custom_path.empty() ? FilePath() : custom_path;
    if (path.empty()) {
        return settings;
    }

    settings.hotkey_modifiers = ::GetPrivateProfileIntW(kSettingsSection, L"HotkeyModifiers",
                                                        static_cast<INT>(settings.hotkey_modifiers), path.c_str());
    wchar_t appearance[32]{};
    ::GetPrivateProfileStringW(kSettingsSection, L"AppearanceTheme", L"Light",
                               appearance, ARRAYSIZE(appearance), path.c_str());
    // Exact values only: unknown, truncated and legacy values fall back independently.
    settings.appearance_theme = ::wcscmp(appearance, L"Dark") == 0
                                    ? AppearanceTheme::Dark : AppearanceTheme::Light;
    settings.hotkey_virtual_key = ::GetPrivateProfileIntW(kSettingsSection, L"HotkeyVirtualKey",
                                                          static_cast<INT>(settings.hotkey_virtual_key), path.c_str());
    settings.capture_cursor = ::GetPrivateProfileIntW(kSettingsSection, L"CaptureCursor",
                                                       settings.capture_cursor ? 1 : 0, path.c_str()) != 0;
    const auto loaded_format = static_cast<media::RecordingFormat>(
        ::GetPrivateProfileIntW(kSettingsSection, L"RecordingFormat",
                                static_cast<INT>(settings.recording_format),
                                path.c_str()));
    settings.recording_format = IsSupportedRecordingFormat(loaded_format)
                                    ? loaded_format
                                    : media::RecordingFormat::Mp4;
    const int audio_val = ::GetPrivateProfileIntW(
        kSettingsSection, L"RecordingAudioSource",
        static_cast<INT>(settings.recording_audio_source), path.c_str());
    const auto loaded_audio = static_cast<media::AudioSource>(audio_val);
    if (IsSupportedRecordingAudioSource(loaded_audio)) {
        settings.recording_audio_source = loaded_audio;
    } else {
        settings.recording_audio_source = media::AudioSource::None;
    }
    GifExportConfiguration loaded_gif;
    const int preset_value = ::GetPrivateProfileIntW(
        kSettingsSection, L"GifPreset", -1, path.c_str());
    if (preset_value >= 0) {
        loaded_gif.preset = static_cast<GifExportPreset>(preset_value);
    }
    // Legacy GifQuality/GifMaximumFrameRate keys represented hidden encoder
    // details, not the new product preset. Missing GifPreset therefore migrates
    // unambiguously to the unchanged High quality default.
    settings.recording_gif = IsValidGifExportConfiguration(loaded_gif)
                                 ? loaded_gif
                                 : GifExportConfiguration{};
    settings.close_editor_after_copy = ::GetPrivateProfileIntW(kSettingsSection, L"CloseEditorAfterCopy",
                                                               settings.close_editor_after_copy ? 1 : 0,
                                                               path.c_str()) != 0;

    wchar_t buffer[MAX_PATH]{};
    ::GetPrivateProfileStringW(kSettingsSection, L"LastSaveDirectory", L"", buffer, ARRAYSIZE(buffer), path.c_str());
    settings.last_save_directory = buffer;

    // A shortcut without modifiers would swallow a plain key globally; refuse it.
    if (settings.hotkey_modifiers == 0 || settings.hotkey_virtual_key == 0) {
        settings.hotkey_modifiers = MOD_CONTROL | MOD_SHIFT;
        settings.hotkey_virtual_key = 'A';
    }
    return settings;
}

bool AppSettings::Save(const std::wstring& custom_path) const {
    if (!IsValidAppearanceTheme(appearance_theme) ||
        !IsSupportedRecordingFormat(recording_format) ||
        !IsSupportedRecordingAudioSource(recording_audio_source) ||
        !IsValidGifExportConfiguration(recording_gif)) {
        return false;
    }
    const std::wstring path = custom_path.empty() ? FilePath() : custom_path;
    if (path.empty()) {
        return false;
    }
    if (!platform::EnsureDirectory(platform::DirectoryOf(path))) {
        return false;
    }

    const auto write_int = [&path](const wchar_t* key, int value) {
        wchar_t text[16]{};
        ::_itow_s(value, text, 10);
        return ::WritePrivateProfileStringW(kSettingsSection, key, text, path.c_str()) != FALSE;
    };

    bool ok = write_int(L"HotkeyModifiers", static_cast<int>(hotkey_modifiers));
    ok = ::WritePrivateProfileStringW(kSettingsSection, L"AppearanceTheme",
        appearance_theme == AppearanceTheme::Dark ? L"Dark" : L"Light", path.c_str()) != FALSE && ok;
    ok = write_int(L"HotkeyVirtualKey", static_cast<int>(hotkey_virtual_key)) && ok;
    ok = write_int(L"CaptureCursor", capture_cursor ? 1 : 0) && ok;
    ok = write_int(L"RecordingFormat", static_cast<int>(recording_format)) && ok;
    ok = write_int(L"RecordingAudioSource", static_cast<int>(recording_audio_source)) && ok;
    ok = write_int(L"GifPreset", static_cast<int>(recording_gif.preset)) && ok;
    for (const wchar_t* legacy_key : {L"GifOutputWidth", L"GifOutputHeight",
                                      L"GifMaximumFrameRate", L"GifQuality"}) {
        ok = ::WritePrivateProfileStringW(kSettingsSection, legacy_key, nullptr,
                                          path.c_str()) != FALSE &&
             ok;
    }
    ok = write_int(L"CloseEditorAfterCopy", close_editor_after_copy ? 1 : 0) && ok;
    ok = ::WritePrivateProfileStringW(kSettingsSection, L"LastSaveDirectory", last_save_directory.c_str(),
                                      path.c_str()) != FALSE &&
         ok;
    return ok;
}

std::wstring AppSettings::HotkeyDisplayText() const {
    std::wstring text;
    if ((hotkey_modifiers & MOD_CONTROL) != 0) {
        AppendModifier(text, L"Ctrl");
    }
    if ((hotkey_modifiers & MOD_SHIFT) != 0) {
        AppendModifier(text, L"Shift");
    }
    if ((hotkey_modifiers & MOD_ALT) != 0) {
        AppendModifier(text, L"Alt");
    }
    if ((hotkey_modifiers & MOD_WIN) != 0) {
        AppendModifier(text, L"Win");
    }
    AppendModifier(text, VirtualKeyName(hotkey_virtual_key).c_str());
    return text;
}

}  // namespace easnap::app
