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

/// Private-profile files are read and written in the system code page unless
/// they start with a UTF-16 byte order mark. Folder names need Unicode, so an
/// existing file is converted once and a new one starts with the mark.
bool EnsureUnicodeFile(const std::wstring& path) {
    const HANDLE existing = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring text;
    if (existing != INVALID_HANDLE_VALUE) {
        std::string bytes;
        char buffer[4096];
        DWORD read = 0;
        while (::ReadFile(existing, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) {
            bytes.append(buffer, read);
        }
        ::CloseHandle(existing);
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
            static_cast<unsigned char>(bytes[1]) == 0xFE) {
            return true;
        }
        const int length = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        text.resize(static_cast<size_t>(length));
        ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
    } else if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const wchar_t mark = 0xFEFF;
    DWORD written = 0;
    bool ok = ::WriteFile(file, &mark, sizeof(mark), &written, nullptr) != FALSE;
    ok = ok && ::WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr) != FALSE;
    ::CloseHandle(file);
    return ok;
}

std::wstring ReadText(const std::wstring& path, const wchar_t* key) {
    wchar_t buffer[1024]{};
    ::GetPrivateProfileStringW(kSettingsSection, key, L"", buffer, ARRAYSIZE(buffer), path.c_str());
    return buffer;
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
    ::GetPrivateProfileStringW(kSettingsSection, L"AppearanceTheme", L"System",
                               appearance, ARRAYSIZE(appearance), path.c_str());
    // Exact values only: unknown, truncated and legacy values fall back
    // independently. A file without the key follows Windows.
    settings.appearance_theme = ::wcscmp(appearance, L"Dark") == 0
                                    ? AppearanceTheme::Dark : AppearanceTheme::Light;
    settings.appearance_follows_system = ::wcscmp(appearance, L"System") == 0;
    const std::wstring language = ReadText(path, L"Language");
    settings.language = language == L"English" ? LanguageSetting::English
                        : language == L"Turkish" ? LanguageSetting::Turkish : LanguageSetting::System;
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
    settings.image_folder = ReadText(path, L"ImageFolder");
    settings.video_folder = ReadText(path, L"VideoFolder");
    settings.quick_save_images =
        ::GetPrivateProfileIntW(kSettingsSection, L"QuickSaveImages", 0, path.c_str()) != 0;
    settings.last_image_directory = ReadText(path, L"LastImageDirectory");
    settings.last_video_directory = ReadText(path, L"LastVideoDirectory");
    // Up to 1.0 one directory was remembered for images and recordings alike.
    const std::wstring legacy_directory = ReadText(path, L"LastSaveDirectory");
    if (settings.last_image_directory.empty()) { settings.last_image_directory = legacy_directory; }
    if (settings.last_video_directory.empty()) { settings.last_video_directory = legacy_directory; }
    settings.first_run_notice_shown =
        ::GetPrivateProfileIntW(kSettingsSection, L"FirstRunNoticeShown", 0, path.c_str()) != 0;

    // A shortcut without modifiers would swallow a plain key globally; refuse it.
    if (settings.hotkey_modifiers == 0 || settings.hotkey_virtual_key == 0) {
        settings.hotkey_modifiers = MOD_CONTROL | MOD_SHIFT;
        settings.hotkey_virtual_key = 'A';
    }
    return settings;
}

bool AppSettings::Save(const std::wstring& custom_path) const {
    if (!IsValidAppearanceTheme(appearance_theme) || !IsValidLanguageSetting(language) ||
        !IsSupportedRecordingFormat(recording_format) ||
        !IsSupportedRecordingAudioSource(recording_audio_source) ||
        !IsValidGifExportConfiguration(recording_gif)) {
        return false;
    }
    const std::wstring path = custom_path.empty() ? FilePath() : custom_path;
    if (path.empty()) {
        return false;
    }
    if (!platform::EnsureDirectory(platform::DirectoryOf(path)) || !EnsureUnicodeFile(path)) {
        return false;
    }
    const auto write_text = [&path](const wchar_t* key, const std::wstring& value) {
        return ::WritePrivateProfileStringW(kSettingsSection, key, value.c_str(), path.c_str()) != FALSE;
    };

    const auto write_int = [&path](const wchar_t* key, int value) {
        wchar_t text[16]{};
        ::_itow_s(value, text, 10);
        return ::WritePrivateProfileStringW(kSettingsSection, key, text, path.c_str()) != FALSE;
    };

    bool ok = write_int(L"HotkeyModifiers", static_cast<int>(hotkey_modifiers));
    ok = write_text(L"AppearanceTheme", appearance_follows_system ? L"System"
                    : appearance_theme == AppearanceTheme::Dark ? L"Dark" : L"Light") && ok;
    ok = write_text(L"Language", language == LanguageSetting::English ? L"English"
                    : language == LanguageSetting::Turkish ? L"Turkish" : L"System") && ok;
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
    ok = write_text(L"ImageFolder", image_folder) && ok;
    ok = write_text(L"VideoFolder", video_folder) && ok;
    ok = write_int(L"QuickSaveImages", quick_save_images ? 1 : 0) && ok;
    ok = write_text(L"LastImageDirectory", last_image_directory) && ok;
    ok = write_text(L"LastVideoDirectory", last_video_directory) && ok;
    ok = write_int(L"FirstRunNoticeShown", first_run_notice_shown ? 1 : 0) && ok;
    // Keys of earlier versions: one shared directory, and a switch that
    // nothing ever read.
    for (const wchar_t* retired_key : {L"LastSaveDirectory", L"CloseEditorAfterCopy"}) {
        ok = ::WritePrivateProfileStringW(kSettingsSection, retired_key, nullptr, path.c_str()) != FALSE && ok;
    }
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
