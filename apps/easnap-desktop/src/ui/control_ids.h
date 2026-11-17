// Easnap - the identifiers of controls, menu commands, timers and subclasses.
//
// Every numeric identifier a window of Easnap gives a child control, a menu
// item, a timer or a window subclass is defined here, grouped by the surface
// that owns it, so a change to a button touches one place. A constant lives in
// the namespace of the code that owns its window. The notification-area menu
// commands and the capture hotkey stay in resources/resource.h, the resource
// header of the application. The result values a caller gives ui::ShowChoice
// buttons stay with that caller.
#pragma once

#include <windows.h>

namespace easnap::ui {

/// A run of consecutive control identifiers: first, first + 1, ..., first + count - 1.
struct ControlRange {
    int first;  ///< The identifier of the first control.
    int count;  ///< How many controls the range holds.

    /// The identifier of the control at `index`.
    [[nodiscard]] constexpr int Id(int index) const noexcept { return first + index; }
    /// True when `id` is one of the range's identifiers.
    [[nodiscard]] constexpr bool Contains(int id) const noexcept { return id >= first && id < first + count; }
    /// The index of `id` in the range; meaningful only when Contains(id).
    [[nodiscard]] constexpr int Index(int id) const noexcept { return id - first; }
};

}  // namespace easnap::ui

// ---------------------------------------------------------------------------
// Screenshot editor (editor/screenshot_editor.cpp): the tools island, the
// property island (context) and the brush popup.
// ---------------------------------------------------------------------------
namespace easnap::editor {

/// Tool buttons on the tools island, one per ToolId in enum order.
inline constexpr ui::ControlRange kToolButtons{2000, 11};
/// Annotation colour swatches on the property island, one per palette colour.
inline constexpr ui::ControlRange kColorButtons{2100, 8};
/// Stroke thickness buttons on the property island.
inline constexpr ui::ControlRange kThicknessButtons{2200, 3};
/// Brush size buttons in the brush popup (the third option row).
inline constexpr ui::ControlRange kBrushSizeButtons{2300, 3};
/// Command buttons on the tools island are this base plus the editor's CommandId.
inline constexpr int kCommandButtonBase = 2400;
/// The identifier of the command button for CommandId `command`.
[[nodiscard]] constexpr int CommandButton(int command) noexcept { return kCommandButtonBase + command; }

/// Text style option: plain text.
inline constexpr int kTextPlainButton = 2500;
/// Text style option: text on a filled box.
inline constexpr int kTextFilledButton = 2501;
/// Text style option: outlined text.
inline constexpr int kTextOutlinedButton = 2502;
/// The text style options, in TextMode order.
inline constexpr ui::ControlRange kTextModeButtons{kTextPlainButton, 3};
/// Effect option: blur.
inline constexpr int kBlurButton = 2510;
/// Effect option: pixelate.
inline constexpr int kPixelateButton = 2511;
/// The effect options, in EffectMode order.
inline constexpr ui::ControlRange kEffectButtons{kBlurButton, 2};
/// Effect method option: brush (opens the brush popup).
inline constexpr int kBrushMethodButton = 2520;
/// Effect method option: region.
inline constexpr int kRegionMethodButton = 2521;
/// The effect method options, in EffectMethod order.
inline constexpr ui::ControlRange kEffectMethodButtons{kBrushMethodButton, 2};
/// Spotlight shape option: rectangle.
inline constexpr int kSpotlightRectangleButton = 2530;
/// Spotlight shape option: ellipse.
inline constexpr int kSpotlightEllipseButton = 2531;
/// Spotlight shape option: rounded rectangle.
inline constexpr int kSpotlightRoundedButton = 2532;
/// The spotlight shape options, in SpotlightShape order.
inline constexpr ui::ControlRange kSpotlightShapeButtons{kSpotlightRectangleButton, 3};

/// Font size combo box on the property island.
inline constexpr int kFontSizeCombo = 2600;
/// Effect strength slider on the property island.
inline constexpr int kEffectStrengthSlider = 2601;
/// Opacity slider on the property island.
inline constexpr int kOpacitySlider = 2602;
/// Font size slider on the property island (watermark).
inline constexpr int kFontSizeSlider = 2603;
/// The three sliders of the property island.
inline constexpr ui::ControlRange kPropertySliders{kEffectStrengthSlider, 3};
/// Watermark text field on the property island.
inline constexpr int kWatermarkTextEdit = 2604;
/// Clear button inside the watermark text field (a child of kWatermarkTextEdit).
inline constexpr int kWatermarkClearButton = 2605;
/// Value label beside the effect strength slider.
inline constexpr int kEffectStrengthValue = 2611;
/// Value label beside the opacity slider.
inline constexpr int kOpacityValue = 2612;
/// Value label beside the font size slider.
inline constexpr int kFontSizeValue = 2613;
/// Inline text field that edits a text annotation in place.
inline constexpr int kInlineTextEdit = 2700;

}  // namespace easnap::editor

// ---------------------------------------------------------------------------
// Capture overlay mode island (ui/overlay.cpp).
// ---------------------------------------------------------------------------
namespace easnap::ui {

/// The capture mode buttons, one per mode in the island's left-to-right order.
inline constexpr ControlRange kModeIslandButtons{3000, 3};

// ---------------------------------------------------------------------------
// Recording panel (ui/recording_controls.cpp).
// ---------------------------------------------------------------------------

/// Recording panel: title.
inline constexpr int kRecordingTitle = 100;
/// Recording panel: elapsed time.
inline constexpr int kRecordingElapsed = 101;
/// Recording panel: label of the audio choices.
inline constexpr int kRecordingAudioLabel = 102;
/// Recording panel: status line.
inline constexpr int kRecordingStatus = 103;
/// Recording panel: label of the format choices.
inline constexpr int kRecordingFormatLabel = 104;
/// Recording panel: MP4 format choice.
inline constexpr int kRecordingFormatMp4 = 105;
/// Recording panel: GIF format choice.
inline constexpr int kRecordingFormatGif = 106;
/// Recording panel: label of the GIF quality choices.
inline constexpr int kRecordingGifQualityLabel = 107;
/// Recording panel: GIF high quality choice.
inline constexpr int kRecordingGifHighQuality = 108;
/// Recording panel: GIF small size choice.
inline constexpr int kRecordingGifLowSize = 109;
/// Recording panel: system audio switch.
inline constexpr int kRecordingAudioSystem = 111;
/// Recording panel: microphone switch.
inline constexpr int kRecordingAudioMicrophone = 112;
/// Recording panel: include-cursor switch.
inline constexpr int kRecordingIncludeCursor = 113;
/// Recording panel: appearance button.
inline constexpr int kRecordingAppearance = 115;
/// Recording panel: pause/resume button.
inline constexpr int kRecordingPauseResume = 120;
/// Recording panel: stop button.
inline constexpr int kRecordingStop = 121;

// ---------------------------------------------------------------------------
// Scrollshot island (ui/scrollshot_controls.cpp). Start and Copy are IDOK,
// Discard is IDCANCEL: Enter and Esc reach them through dialog navigation.
// ---------------------------------------------------------------------------

/// Scrollshot island: help button.
inline constexpr int kScrollshotHelp = 200;
/// Scrollshot island: Edit button.
inline constexpr int kScrollshotEdit = 201;
/// Scrollshot island: Save button.
inline constexpr int kScrollshotSave = 202;
/// Scrollshot island: size label.
inline constexpr int kScrollshotSize = 210;
/// Scrollshot island: message label.
inline constexpr int kScrollshotMessage = 211;
/// Scrollshot island: Copy button. IDOK is taken by Start in the Start view;
/// the handler maps it by view.
inline constexpr int kScrollshotCopyAlias = IDOK;

// ---------------------------------------------------------------------------
// Trim window (ui/trim_window.cpp).
// ---------------------------------------------------------------------------

/// Trim window: play/pause button.
inline constexpr int kTrimPlayPause = 100;
/// Trim window: set-start button.
inline constexpr int kTrimSetIn = 101;
/// Trim window: set-end button.
inline constexpr int kTrimSetOut = 102;
/// Trim window: cancel-export button.
inline constexpr int kTrimCancelExport = 103;
/// Trim window: Copy button.
inline constexpr int kTrimCopy = 104;

// ---------------------------------------------------------------------------
// Settings dialog (ui/settings_dialog.cpp); tests drive it with these.
// ---------------------------------------------------------------------------

/// Settings: language choice.
inline constexpr int kSettingsLanguage = 3101;
/// Settings: appearance choice.
inline constexpr int kSettingsAppearance = 3102;
/// Settings: capture shortcut field.
inline constexpr int kSettingsShortcut = 3103;
/// Settings: hint under the shortcut field.
inline constexpr int kSettingsShortcutHint = 3104;
/// Settings: start-with-Windows switch.
inline constexpr int kSettingsStartWithWindows = 3105;
/// Settings: capture-cursor switch.
inline constexpr int kSettingsCaptureCursor = 3106;
/// Settings: image folder field.
inline constexpr int kSettingsImageFolder = 3107;
/// Settings: change the image folder.
inline constexpr int kSettingsImageChange = 3108;
/// Settings: reset the image folder.
inline constexpr int kSettingsImageReset = 3109;
/// Settings: quick-save switch.
inline constexpr int kSettingsQuickSave = 3110;
/// Settings: video folder field.
inline constexpr int kSettingsVideoFolder = 3111;
/// Settings: change the video folder.
inline constexpr int kSettingsVideoChange = 3112;
/// Settings: reset the video folder.
inline constexpr int kSettingsVideoReset = 3113;

// ---------------------------------------------------------------------------
// Menu commands.
// ---------------------------------------------------------------------------

/// Appearance menu (CreateAppearanceMenu): light.
inline constexpr UINT kAppearanceLight = 41001;
/// Appearance menu: dark.
inline constexpr UINT kAppearanceDark = 41002;
/// Appearance menu: same as Windows.
inline constexpr UINT kAppearanceSystem = 41003;

/// Pin menu: copy. A pin window also accepts its menu commands as WM_COMMAND.
inline constexpr UINT kPinCommandCopy = 0xE501;
/// Pin menu: save.
inline constexpr UINT kPinCommandSave = 0xE502;
/// Pin menu: original size.
inline constexpr UINT kPinCommandOriginalSize = 0xE503;
/// Pin menu: close this pin.
inline constexpr UINT kPinCommandClose = 0xE504;
/// Pin menu: close every pin.
inline constexpr UINT kPinCommandCloseAll = 0xE505;

}  // namespace easnap::ui

// ---------------------------------------------------------------------------
// Timers, window subclasses and tooltip tools. Timer identifiers are per
// window, subclass identifiers per window and subclass procedure; they are
// listed together so a reused value is visible.
// ---------------------------------------------------------------------------
namespace easnap::app {

/// Timer on the host window: retired windows are destroyed once no dialog of
/// theirs is running its own message loop any more.
inline constexpr UINT_PTR kTimerDeferredCleanup = 0xEA50;
/// Timer on the host window: a checked video waits until no dialog of
/// Easnap's runs its own message loop any more.
inline constexpr UINT_PTR kTimerVideoProbeRetry = 0xEA51;

}  // namespace easnap::app

namespace easnap::ui {

/// Subclass of a styled button; also its 15 ms hover-animation timer on that button.
inline constexpr UINT_PTR kButtonSubclass = 0xEA51;
/// Subclass of a styled tooltip.
inline constexpr UINT_PTR kTooltipSubclass = 0xEA52;
/// Subclass of a styled slider.
inline constexpr UINT_PTR kSliderSubclass = 0xEA53;
/// Subclass of a styled input field.
inline constexpr UINT_PTR kInputSubclass = 0xEA54;
/// Subclass of a styled progress bar.
inline constexpr UINT_PTR kProgressSubclass = 0xEA55;
/// Subclass that answers a control's accessible name.
inline constexpr UINT_PTR kAccessibleNameSubclass = 0xEA56;
/// Subclass of the owner window of a themed popup menu.
inline constexpr UINT_PTR kMenuOwnerSubclass = 0xEA57;
/// Subclass of a window with the shared caption chrome.
inline constexpr UINT_PTR kChromeSubclass = 0xEA59;
/// Subclass that paints a label's or an edit's text double-buffered.
inline constexpr UINT_PTR kBufferedTextSubclass = 0xEA5A;
/// Subclass of the settings dialog's shortcut field.
inline constexpr UINT_PTR kShortcutFieldSubclass = 0xEA70;
/// Timer on a TypingPointer's owner window that watches for pointer movement.
inline constexpr UINT_PTR kTypingPointerTimer = 0xEA70;

/// Timer on the overlay's dim window: a notice expires.
inline constexpr UINT_PTR kOverlayNoticeTimer = 1;
/// Timer on the overlay's dim window: the mode island fades.
inline constexpr UINT_PTR kOverlayModeFadeTimer = 2;
/// Timer on a capture workspace notice: the end of its life.
inline constexpr UINT_PTR kWorkspaceNoticeExpireTimer = 1;
/// Timer on a capture workspace notice: the moment a progress notice appears.
inline constexpr UINT_PTR kWorkspaceNoticeAppearTimer = 2;
/// Timer on the recording panel: status refresh.
inline constexpr UINT_PTR kRecordingStatusTimer = 1;
/// Timer on the trim window: status refresh.
inline constexpr UINT_PTR kTrimStatusTimer = 1;

}  // namespace easnap::ui

namespace easnap::editor {

/// Subclass of every editor toolbar button.
inline constexpr UINT_PTR kToolbarButtonSubclass = 1;
/// Timer on the property island: its fade-in.
inline constexpr UINT_PTR kContextFadeTimer = 0xEA61;
/// Timer on the property island: hover over the brush option and its popup.
inline constexpr UINT_PTR kBrushPopupHoverTimer = 0xEA62;
/// Timer on the property island: hover over the watermark clear button.
inline constexpr UINT_PTR kWatermarkClearHoverTimer = 0xEA63;
/// Tooltip tool on the tools island itself: a disabled button receives no
/// mouse messages, so its reason for being disabled is shown through the parent.
inline constexpr UINT_PTR kScrollshotDisabledTip = 0xEA70;

}  // namespace easnap::editor
