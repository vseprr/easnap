// Easnap unit tests - production session transition rules.

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/app_settings.h"
#include "app/recording_workflow.h"
#include "app/session_transitions.h"

using easnap::app::AppSettings;
using easnap::app::IsSessionTransitionLegal;
using easnap::app::SessionState;
using easnap::media::AudioSource;
using easnap::media::RecordingFormat;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

void TestIdentityAlwaysLegal() {
    for (std::size_t value = 0; value < easnap::app::kSessionStateCount; ++value) {
        const auto state = static_cast<SessionState>(value);
        CHECK(IsSessionTransitionLegal(state, state));
    }
}

void TestSessionEntryAndCancellation() {
    CHECK(easnap::app::kSessionTransitionCount == 35);
    CHECK(IsSessionTransitionLegal(SessionState::Idle, SessionState::RegionSelecting));
    CHECK(IsSessionTransitionLegal(SessionState::RegionSelecting, SessionState::Idle));
    CHECK(!IsSessionTransitionLegal(SessionState::Idle, SessionState::RecordingActive));
}

void TestScreenshotBranch() {
    CHECK(IsSessionTransitionLegal(SessionState::ScreenshotEditing, SessionState::RecordingConfiguring));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingConfiguring, SessionState::ScreenshotEditing));
    CHECK(IsSessionTransitionLegal(SessionState::RegionSelecting, SessionState::ScreenshotEditing));
    CHECK(IsSessionTransitionLegal(SessionState::ScreenshotEditing, SessionState::ScreenshotExporting));
    CHECK(IsSessionTransitionLegal(SessionState::ScreenshotEditing, SessionState::Idle));
    CHECK(IsSessionTransitionLegal(SessionState::ScreenshotExporting, SessionState::Idle));
    CHECK(IsSessionTransitionLegal(SessionState::ScreenshotExporting, SessionState::ScreenshotEditing));
    CHECK(!IsSessionTransitionLegal(SessionState::ScreenshotEditing, SessionState::RecordingActive));
    CHECK(!IsSessionTransitionLegal(SessionState::ScreenshotExporting, SessionState::RegionSelecting));
}

void TestRecordingBranch() {
    CHECK(IsSessionTransitionLegal(SessionState::RegionSelecting, SessionState::RecordingConfiguring));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingConfiguring, SessionState::RecordingActive));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingConfiguring, SessionState::Idle));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingActive, SessionState::RecordingPaused));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingPaused, SessionState::RecordingActive));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingActive, SessionState::RecordingFinalizing));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingPaused, SessionState::RecordingFinalizing));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingFinalizing, SessionState::Idle));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingFinalizing, SessionState::RecordingTrimming));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingTrimming, SessionState::Idle));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingTrimming, SessionState::Exporting));
    CHECK(IsSessionTransitionLegal(SessionState::Exporting, SessionState::RecordingTrimming));
    CHECK(IsSessionTransitionLegal(SessionState::Exporting, SessionState::Idle));
    CHECK(!IsSessionTransitionLegal(SessionState::RecordingActive, SessionState::Idle));
    CHECK(!IsSessionTransitionLegal(SessionState::RecordingPaused, SessionState::Idle));
    CHECK(!IsSessionTransitionLegal(SessionState::RecordingFinalizing, SessionState::RecordingActive));
    CHECK(!IsSessionTransitionLegal(SessionState::Exporting, SessionState::RecordingActive));
}

void TestScrollshotBranch() {
    // The mode can be chosen before or after a region exists, and switched
    // with the other two modes while the frame is provisional.
    CHECK(IsSessionTransitionLegal(SessionState::RegionSelecting, SessionState::ScrollshotConfiguring));
    CHECK(IsSessionTransitionLegal(SessionState::ScreenshotEditing, SessionState::ScrollshotConfiguring));
    CHECK(IsSessionTransitionLegal(SessionState::ScrollshotConfiguring, SessionState::ScreenshotEditing));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingConfiguring, SessionState::ScrollshotConfiguring));
    CHECK(IsSessionTransitionLegal(SessionState::ScrollshotConfiguring, SessionState::RecordingConfiguring));
    CHECK(IsSessionTransitionLegal(SessionState::ScrollshotConfiguring, SessionState::Idle));
    // Start, and the tools island's button that skips the Start step.
    CHECK(IsSessionTransitionLegal(SessionState::ScrollshotConfiguring, SessionState::ScrollshotCapturing));
    CHECK(IsSessionTransitionLegal(SessionState::ScreenshotEditing, SessionState::ScrollshotCapturing));
    // Capturing ends in review with the image kept, or with the session.
    CHECK(IsSessionTransitionLegal(SessionState::ScrollshotCapturing, SessionState::ScrollshotReview));
    CHECK(IsSessionTransitionLegal(SessionState::ScrollshotCapturing, SessionState::Idle));
    CHECK(IsSessionTransitionLegal(SessionState::ScrollshotReview, SessionState::Idle));
    CHECK(IsSessionTransitionLegal(SessionState::ScrollshotReview, SessionState::ScreenshotEditing));
    // An edited Scrollshot returns to review when the displays change.
    CHECK(IsSessionTransitionLegal(SessionState::ScreenshotEditing, SessionState::ScrollshotReview));

    // The frame is locked once capture started, and a stopped capture does
    // not resume. Recording and Scrollshot never run into each other.
    CHECK(!IsSessionTransitionLegal(SessionState::ScrollshotCapturing, SessionState::ScrollshotConfiguring));
    CHECK(!IsSessionTransitionLegal(SessionState::ScrollshotCapturing, SessionState::ScreenshotEditing));
    CHECK(!IsSessionTransitionLegal(SessionState::ScrollshotCapturing, SessionState::RecordingConfiguring));
    CHECK(!IsSessionTransitionLegal(SessionState::ScrollshotReview, SessionState::ScrollshotCapturing));
    CHECK(!IsSessionTransitionLegal(SessionState::ScrollshotReview, SessionState::ScrollshotConfiguring));
    CHECK(!IsSessionTransitionLegal(SessionState::ScrollshotReview, SessionState::RecordingConfiguring));
    CHECK(!IsSessionTransitionLegal(SessionState::Idle, SessionState::ScrollshotCapturing));
    CHECK(!IsSessionTransitionLegal(SessionState::Idle, SessionState::ScrollshotConfiguring));
    CHECK(!IsSessionTransitionLegal(SessionState::RegionSelecting, SessionState::ScrollshotCapturing));
    CHECK(!IsSessionTransitionLegal(SessionState::RecordingActive, SessionState::ScrollshotCapturing));
    CHECK(!IsSessionTransitionLegal(SessionState::ScreenshotExporting, SessionState::ScrollshotReview));
}

void TestRecordingControlPolicy() {
    const auto configuring =
        easnap::app::RecordingControlsForState(SessionState::RecordingConfiguring);
    CHECK(configuring.can_start);
    CHECK(configuring.can_cancel);
    CHECK(!configuring.can_pause);
    CHECK(!configuring.can_resume);
    CHECK(!configuring.can_stop);

    const auto active =
        easnap::app::RecordingControlsForState(SessionState::RecordingActive);
    CHECK(active.can_pause);
    CHECK(active.can_stop);
    CHECK(!active.can_resume);

    const auto paused =
        easnap::app::RecordingControlsForState(SessionState::RecordingPaused);
    CHECK(paused.can_resume);
    CHECK(paused.can_stop);
    CHECK(!paused.can_pause);

    const auto finalizing =
        easnap::app::RecordingControlsForState(SessionState::RecordingFinalizing);
    CHECK(!finalizing.can_start);
    CHECK(!finalizing.can_cancel);
    CHECK(!finalizing.can_pause);
    CHECK(!finalizing.can_resume);
    CHECK(!finalizing.can_stop);
}

void TestSupportedRecordingAudioChoices() {
    CHECK(easnap::app::kSupportedRecordingAudioSources.size() == 4);
    CHECK(easnap::app::IsSupportedRecordingAudioSource(
        easnap::media::AudioSource::None));
    CHECK(easnap::app::IsSupportedRecordingAudioSource(
        easnap::media::AudioSource::SystemAudio));
    CHECK(easnap::app::IsSupportedRecordingAudioSource(
        easnap::media::AudioSource::Microphone));
    CHECK(easnap::app::IsSupportedRecordingAudioSource(
        easnap::media::AudioSource::Both));
    CHECK(!easnap::app::IsSupportedRecordingAudioSource(
        static_cast<easnap::media::AudioSource>(99)));
}

void TestRecordingFormatsAndCapabilities() {
    CHECK(easnap::app::kSupportedRecordingFormats.size() == 2);
    CHECK(easnap::app::IsSupportedRecordingFormat(RecordingFormat::Mp4));
    CHECK(easnap::app::IsSupportedRecordingFormat(RecordingFormat::Gif));
    CHECK(!easnap::app::IsSupportedRecordingFormat(
        static_cast<RecordingFormat>(99)));

    const auto mp4 =
        easnap::app::CapabilitiesForRecordingFormat(RecordingFormat::Mp4);
    CHECK(mp4.supports_audio);
    CHECK(mp4.supports_cursor);
    CHECK(!mp4.requires_final_conversion);
    CHECK(!mp4.shows_gif_quality);
    const auto gif =
        easnap::app::CapabilitiesForRecordingFormat(RecordingFormat::Gif);
    CHECK(!gif.supports_audio);
    CHECK(gif.supports_cursor);
    CHECK(gif.requires_final_conversion);
    CHECK(gif.shows_gif_quality);

    for (AudioSource source : easnap::app::kSupportedRecordingAudioSources) {
        easnap::app::RecordingConfiguration configuration;
        configuration.format = RecordingFormat::Mp4;
        configuration.mp4_audio = source;
        CHECK(easnap::app::EffectiveAudioSource(configuration) == source);
        configuration.format = RecordingFormat::Gif;
        CHECK(easnap::app::EffectiveAudioSource(configuration) ==
              AudioSource::None);
        configuration.format = RecordingFormat::Mp4;
        CHECK(configuration.mp4_audio == source);
    }

    easnap::app::RecordingConfiguration configuration;
    configuration.format = RecordingFormat::Gif;
    configuration.mp4_audio = AudioSource::Both;
    configuration.include_cursor = true;
    const auto options = easnap::app::MakeRecordingOptions(
        RECT{0, 0, 640, 480}, configuration);
    CHECK(options.format == RecordingFormat::Gif);
    CHECK(options.audio == AudioSource::None);
    CHECK(options.include_cursor);
    CHECK(options.gif_fps == 15);
    CHECK(options.gif_width == 0);
    CHECK(options.gif_height == 0);
    CHECK(options.gif_palette_colors == 256);
    CHECK(options.gif_dither);
    CHECK(!options.gif_low_size && !options.gif_fixed_rgb332);
    CHECK(!options.gif_sparse_transparency);
    CHECK(!options.gif_distributed_cadence);

    configuration.gif.preset = easnap::app::GifExportPreset::LowSize;
    const auto low_options = easnap::app::MakeRecordingOptions(
        RECT{0, 0, 640, 480}, configuration);
    CHECK(low_options.gif_fps == 12);
    CHECK(low_options.gif_width == 0);
    CHECK(low_options.gif_height == 0);
    CHECK(low_options.gif_palette_colors == 256);
    CHECK(low_options.gif_dither);
    CHECK(low_options.gif_low_size && !low_options.gif_fixed_rgb332);
    CHECK(low_options.gif_sparse_transparency);
    CHECK(low_options.gif_distributed_cadence);
    configuration.format = RecordingFormat::Mp4;
    configuration.format = RecordingFormat::Gif;
    CHECK(configuration.gif.preset == easnap::app::GifExportPreset::LowSize);
    configuration.gif.preset = easnap::app::GifExportPreset::HighQuality;
    CHECK(easnap::app::MakeRecordingOptions(RECT{0, 0, 640, 480},
                                            configuration)
              .gif_fps == 15);
}

void TestRecordingFormatSettingsPersistence() {
    const std::wstring temp_path =
        (std::filesystem::temp_directory_path() /
         L"easnap_test_settings_format.ini")
            .wstring();
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);

    AppSettings settings;
    settings.recording_format = RecordingFormat::Gif;
    settings.recording_audio_source = AudioSource::Both;
    settings.recording_gif.preset = easnap::app::GifExportPreset::LowSize;
    CHECK(settings.Save(temp_path));
    const AppSettings loaded = AppSettings::Load(temp_path);
    CHECK(loaded.recording_format == RecordingFormat::Gif);
    CHECK(loaded.recording_audio_source == AudioSource::Both);
    CHECK(loaded.recording_gif.preset == easnap::app::GifExportPreset::LowSize);

    settings.recording_gif.preset = easnap::app::GifExportPreset::HighQuality;
    CHECK(settings.Save(temp_path));
    CHECK(AppSettings::Load(temp_path).recording_gif.preset ==
          easnap::app::GifExportPreset::HighQuality);

    ::WritePrivateProfileStringW(L"Easnap", L"RecordingFormat", L"99",
                                 temp_path.c_str());
    ::WritePrivateProfileStringW(L"Easnap", L"GifPreset", L"99",
                                 temp_path.c_str());
    const AppSettings corrupt = AppSettings::Load(temp_path);
    CHECK(corrupt.recording_format == RecordingFormat::Mp4);
    CHECK(corrupt.recording_gif.preset ==
          easnap::app::GifExportPreset::HighQuality);
    CHECK(corrupt.recording_audio_source == AudioSource::Both);

    ::WritePrivateProfileStringW(L"Easnap", L"GifPreset", nullptr,
                                 temp_path.c_str());
    ::WritePrivateProfileStringW(L"Easnap", L"GifQuality", L"1",
                                 temp_path.c_str());
    const AppSettings legacy = AppSettings::Load(temp_path);
    CHECK(legacy.recording_gif.preset ==
          easnap::app::GifExportPreset::HighQuality);

    std::filesystem::remove(temp_path, ec);
}

void TestAppearanceSettingsPersistence() {
    using easnap::app::AppearanceTheme;
    const auto path = (std::filesystem::temp_directory_path() /
        (L"easnap_theme_" + std::to_wstring(::GetCurrentProcessId()) + L".ini")).wstring();
    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(AppSettings::Load(path).appearance_theme == AppearanceTheme::Light);
    AppSettings accepted;
    accepted.recording_audio_source = AudioSource::Both;
    accepted.recording_format = RecordingFormat::Gif;
    accepted.recording_gif.preset = easnap::app::GifExportPreset::LowSize;
    accepted.appearance_theme = AppearanceTheme::Dark;
    accepted.appearance_follows_system = false;  // An explicit choice.
    CHECK(accepted.Save(path));
    CHECK(AppSettings::Load(path).appearance_theme == AppearanceTheme::Dark);
    CHECK(!AppSettings::Load(path).appearance_follows_system);
    for (const wchar_t* value : {L"Light", L"dark", L"99", L"", L"System", static_cast<const wchar_t*>(nullptr)}) {
        CHECK(::WritePrivateProfileStringW(L"Easnap", L"AppearanceTheme", value, path.c_str()) != FALSE);
        const auto loaded = AppSettings::Load(path);
        CHECK(loaded.appearance_theme == AppearanceTheme::Light);
        // Only the exact word, or a file without the key, follows Windows.
        CHECK(loaded.appearance_follows_system == (value == nullptr || ::wcscmp(value, L"System") == 0));
        CHECK(loaded.recording_audio_source == AudioSource::Both);
        CHECK(loaded.recording_format == RecordingFormat::Gif);
        CHECK(loaded.recording_gif.preset == easnap::app::GifExportPreset::LowSize);
    }
    accepted.appearance_theme = static_cast<AppearanceTheme>(99);
    CHECK(!accepted.Save(path));
    std::filesystem::remove(path, error);
}

/// Language, folders and quick save; a Unicode file; keys of earlier versions.
void TestSettingsLanguageAndFolders() {
    using easnap::app::LanguageSetting;
    const auto path = (std::filesystem::temp_directory_path() /
        (L"easnap_settings_" + std::to_wstring(::GetCurrentProcessId()) + L".ini")).wstring();
    std::error_code error;
    std::filesystem::remove(path, error);

    // Nothing saved yet: Windows decides language and appearance.
    const AppSettings fresh = AppSettings::Load(path);
    CHECK(fresh.language == LanguageSetting::System && fresh.appearance_follows_system);
    CHECK(fresh.image_folder.empty() && fresh.video_folder.empty() && !fresh.quick_save_images);
    CHECK(!fresh.first_run_notice_shown);

    AppSettings chosen;
    chosen.language = LanguageSetting::Turkish;
    chosen.image_folder = L"D:\\G\u00F6r\u00FCnt\u00FCler\\\u30B9\u30AF\u30EA\u30FC\u30F3";  // Turkish and Japanese letters.
    chosen.video_folder = L"D:\\Kay\u0131tlar";
    chosen.quick_save_images = true;
    chosen.last_image_directory = L"C:\\Last images";
    chosen.last_video_directory = L"C:\\Last videos";
    chosen.first_run_notice_shown = true;
    CHECK(chosen.Save(path));
    const AppSettings loaded = AppSettings::Load(path);
    CHECK(loaded.language == LanguageSetting::Turkish);
    CHECK(loaded.image_folder == chosen.image_folder && loaded.video_folder == chosen.video_folder);
    CHECK(loaded.quick_save_images && loaded.first_run_notice_shown);
    CHECK(loaded.last_image_directory == L"C:\\Last images" && loaded.last_video_directory == L"C:\\Last videos");
    // A set folder is where a Save dialog starts; otherwise the one used last.
    CHECK(loaded.ImageStartDirectory() == chosen.image_folder && loaded.VideoStartDirectory() == chosen.video_folder);
    AppSettings unset = loaded;
    unset.image_folder.clear();
    CHECK(unset.ImageStartDirectory() == L"C:\\Last images");
    {
        // The file is UTF-16, so every folder name survives.
        std::ifstream file(path, std::ios::binary);
        char mark[2]{};
        file.read(mark, 2);
        CHECK(static_cast<unsigned char>(mark[0]) == 0xFF && static_cast<unsigned char>(mark[1]) == 0xFE);
    }
    for (const wchar_t* value : {L"English", L"Turkish", L"turkish", L"", L"Deutsch"}) {
        CHECK(::WritePrivateProfileStringW(L"Easnap", L"Language", value, path.c_str()) != FALSE);
        const LanguageSetting language = AppSettings::Load(path).language;
        CHECK(language == (::wcscmp(value, L"English") == 0 ? LanguageSetting::English
                           : ::wcscmp(value, L"Turkish") == 0 ? LanguageSetting::Turkish : LanguageSetting::System));
    }
    chosen.language = static_cast<LanguageSetting>(7);
    CHECK(!chosen.Save(path));

    // A file written by 1.0: system code page, one directory for everything,
    // and a switch nothing ever read.
    std::filesystem::remove(path, error);
    {
        std::ofstream legacy(path, std::ios::binary);
        legacy << "[Easnap]\r\nHotkeyModifiers=6\r\nHotkeyVirtualKey=65\r\nCloseEditorAfterCopy=1\r\n"
                  "LastSaveDirectory=C:\\Old place\r\nAppearanceTheme=Dark\r\n";
    }
    AppSettings migrated = AppSettings::Load(path);
    CHECK(migrated.last_image_directory == L"C:\\Old place" && migrated.last_video_directory == L"C:\\Old place");
    CHECK(migrated.appearance_theme == easnap::app::AppearanceTheme::Dark && !migrated.appearance_follows_system);
    CHECK(migrated.hotkey_modifiers == 6 && migrated.hotkey_virtual_key == 65);
    CHECK(migrated.Save(path));
    wchar_t retired[64]{};
    ::GetPrivateProfileStringW(L"Easnap", L"LastSaveDirectory", L"<gone>", retired, 64, path.c_str());
    CHECK(std::wstring(retired) == L"<gone>");
    ::GetPrivateProfileStringW(L"Easnap", L"CloseEditorAfterCopy", L"<gone>", retired, 64, path.c_str());
    CHECK(std::wstring(retired) == L"<gone>");
    const AppSettings again = AppSettings::Load(path);
    CHECK(again.last_image_directory == L"C:\\Old place" && again.appearance_theme == easnap::app::AppearanceTheme::Dark);
    CHECK(again.hotkey_modifiers == 6 && again.hotkey_virtual_key == 65);
    std::filesystem::remove(path, error);
}

void TestAudioSourceSettingsPersistence() {
    const std::wstring temp_path =
        (std::filesystem::temp_directory_path() / L"easnap_test_settings_audio.ini").wstring();
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);

    // 1. Default should be AudioSource::None
    AppSettings default_settings;
    CHECK(default_settings.recording_audio_source == AudioSource::None);

    // 2. Persist AudioSource::Both and load back
    default_settings.recording_audio_source = AudioSource::Both;
    CHECK(default_settings.Save(temp_path));
    AppSettings loaded_both = AppSettings::Load(temp_path);
    CHECK(loaded_both.recording_audio_source == AudioSource::Both);

    // 3. Persist AudioSource::SystemAudio and load back
    loaded_both.recording_audio_source = AudioSource::SystemAudio;
    CHECK(loaded_both.Save(temp_path));
    AppSettings loaded_system = AppSettings::Load(temp_path);
    CHECK(loaded_system.recording_audio_source == AudioSource::SystemAudio);

    // 4. Persist AudioSource::Microphone and load back
    loaded_system.recording_audio_source = AudioSource::Microphone;
    CHECK(loaded_system.Save(temp_path));
    AppSettings loaded_mic = AppSettings::Load(temp_path);
    CHECK(loaded_mic.recording_audio_source == AudioSource::Microphone);

    // 5. Persist AudioSource::None and load back
    loaded_mic.recording_audio_source = AudioSource::None;
    CHECK(loaded_mic.Save(temp_path));
    AppSettings loaded_none = AppSettings::Load(temp_path);
    CHECK(loaded_none.recording_audio_source == AudioSource::None);

    // 6. Corrupt or unsupported value in INI must safely fall back to AudioSource::None
    ::WritePrivateProfileStringW(L"Easnap", L"RecordingAudioSource", L"999", temp_path.c_str());
    AppSettings loaded_corrupt = AppSettings::Load(temp_path);
    CHECK(loaded_corrupt.recording_audio_source == AudioSource::None);

    std::filesystem::remove(temp_path, ec);
}

void TestBothRecordingTransitions() {
    // Both is a supported recording audio source
    CHECK(easnap::app::IsSupportedRecordingAudioSource(AudioSource::Both));

    // Full recording transition path with Both
    CHECK(IsSessionTransitionLegal(SessionState::RecordingConfiguring, SessionState::RecordingActive));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingActive, SessionState::RecordingPaused));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingPaused, SessionState::RecordingActive));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingActive, SessionState::RecordingFinalizing));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingFinalizing, SessionState::RecordingTrimming));
    CHECK(IsSessionTransitionLegal(SessionState::RecordingTrimming, SessionState::Idle));

    // Illegal transitions remain strictly forbidden
    CHECK(!IsSessionTransitionLegal(SessionState::RecordingConfiguring, SessionState::RecordingPaused));
    CHECK(!IsSessionTransitionLegal(SessionState::RecordingActive, SessionState::Idle));
    CHECK(!IsSessionTransitionLegal(SessionState::RecordingPaused, SessionState::Idle));
    CHECK(!IsSessionTransitionLegal(SessionState::RecordingFinalizing, SessionState::RecordingActive));
}

void TestRecordingRegionAndElapsedFormatting() {
    const RECT even{10, 20, 210, 120};
    const RECT even_result =
        easnap::app::MakeRecordingRegionEncoderCompatible(even);
    CHECK(even_result.left == 10);
    CHECK(even_result.top == 20);
    CHECK(even_result.right == 210);
    CHECK(even_result.bottom == 120);

    const RECT odd{10, 20, 211, 123};
    const RECT odd_result =
        easnap::app::MakeRecordingRegionEncoderCompatible(odd);
    CHECK(odd_result.left == 10);
    CHECK(odd_result.top == 20);
    CHECK(odd_result.right == 210);
    CHECK(odd_result.bottom == 122);

    CHECK(easnap::app::FormatRecordingElapsed(0) == L"00:00:00");
    CHECK(easnap::app::FormatRecordingElapsed(65ULL * 10'000'000ULL) ==
          L"00:01:05");
    CHECK(easnap::app::FormatRecordingElapsed(3'661ULL * 10'000'000ULL) ==
          L"01:01:01");
}

}  // namespace

/// What exiting would lose, for every state: the table the exit question and
/// the shutdown block reason are chosen from.
void TestPendingWork() {
    using easnap::app::PendingWork;
    using easnap::app::PendingWorkFor;
    struct Row { SessionState state; PendingWork plain; PendingWork with_edits; };
    const Row rows[] = {
        {SessionState::Idle, PendingWork::None, PendingWork::None},
        {SessionState::RegionSelecting, PendingWork::None, PendingWork::None},
        {SessionState::ScreenshotEditing, PendingWork::None, PendingWork::Image},
        {SessionState::ScreenshotExporting, PendingWork::None, PendingWork::None},
        {SessionState::RecordingConfiguring, PendingWork::None, PendingWork::None},
        {SessionState::RecordingActive, PendingWork::Recording, PendingWork::Recording},
        {SessionState::RecordingPaused, PendingWork::Recording, PendingWork::Recording},
        {SessionState::RecordingFinalizing, PendingWork::Recording, PendingWork::Recording},
        {SessionState::RecordingTrimming, PendingWork::UnsavedRecording, PendingWork::UnsavedRecording},
        {SessionState::Exporting, PendingWork::Export, PendingWork::Export},
        {SessionState::ScrollshotConfiguring, PendingWork::None, PendingWork::None},
        {SessionState::ScrollshotCapturing, PendingWork::Image, PendingWork::Image},
        {SessionState::ScrollshotReview, PendingWork::Image, PendingWork::Image},
    };
    CHECK(sizeof(rows) / sizeof(rows[0]) == easnap::app::kSessionStateCount);
    for (const Row& row : rows) {
        CHECK(PendingWorkFor(row.state, false) == row.plain);
        CHECK(PendingWorkFor(row.state, true) == row.with_edits);
    }
}

/// What H.264 can hold: 36,864 macroblocks and 8,688 pixels on a side.
void TestRecordingRegionFitsH264() {
    using easnap::app::RecordingRegionFitsH264;
    CHECK(RecordingRegionFitsH264(RECT{0, 0, 1920, 1080}));
    CHECK(RecordingRegionFitsH264(RECT{-1920, 0, 1920, 1080}));   // Two displays side by side.
    CHECK(RecordingRegionFitsH264(RECT{0, 0, 3840, 2160}));
    CHECK(RecordingRegionFitsH264(RECT{0, 0, 4096, 2304}));       // Exactly the limit.
    CHECK(!RecordingRegionFitsH264(RECT{0, 0, 4096, 2320}));
    CHECK(RecordingRegionFitsH264(RECT{0, 0, 7680, 1080}));       // Wide, but few enough macroblocks.
    CHECK(!RecordingRegionFitsH264(RECT{0, 0, 7680, 2160}));      // Two 4K displays.
    CHECK(RecordingRegionFitsH264(RECT{0, 0, 16, 8688}));
    CHECK(!RecordingRegionFitsH264(RECT{0, 0, 16, 8690}));
    CHECK(!RecordingRegionFitsH264(RECT{0, 0, 8704, 16}));
    CHECK(!RecordingRegionFitsH264(RECT{0, 0, 0, 100}) && !RecordingRegionFitsH264(RECT{10, 10, 5, 5}));
}

int main() {
    TestPendingWork();
    TestRecordingRegionFitsH264();
    TestIdentityAlwaysLegal();
    TestSessionEntryAndCancellation();
    TestScreenshotBranch();
    TestRecordingBranch();
    TestScrollshotBranch();
    TestRecordingControlPolicy();
    TestSupportedRecordingAudioChoices();
    TestRecordingFormatsAndCapabilities();
    TestRecordingFormatSettingsPersistence();
    TestAudioSourceSettingsPersistence();
    TestAppearanceSettingsPersistence();
    TestSettingsLanguageAndFolders();
    TestBothRecordingTransitions();
    TestRecordingRegionAndElapsedFormatting();

    if (g_fail == 0) {
        std::printf("PASSED  %d/%d tests\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
