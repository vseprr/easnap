#pragma once

#include "ui/paint_buffer.h"

#include "ui/ui_theme.h"

#include <vector>

namespace easnap::ui {

// Geometry icons shared by GDI native buttons. No font-dependent pictographs.
enum class Icon { None, Record, Pause, Play, Stop, SystemAudio, Microphone, Cursor,
                  Copy, Save, Discard, Appearance, Sun, Moon, Pen, Arrow,
                  Rectangle, Ellipse, Text, Mosaic, Undo, Redo, Clear, Check, Line, Droplet, Tag, Spotlight, Watermark,
                  TextFill, TextOutline, Hand, Region, RoundedRectangle, ChevronDown, Minimize, Maximize,
                  Scissors, Help };
enum class ButtonRole { Standard, Primary, Destructive, Success };
struct ButtonStyle {
    Icon icon{Icon::None};
    ButtonRole role{ButtonRole::Standard};
    bool icon_only{false};
    bool swatch{false};
    COLORREF content_color{0}; // Annotation data, never a theme token.
    int sample{0}; // 1 = stroke width, 2 = mosaic footprint.
    float sample_size{0};
    bool segmented{false};
    bool preserve_disabled_visual{false}; // Keep a fading surface visually stable.
    bool hide_focus_line{false};
    bool dropdown{false}; // One icon button with an inset trailing chevron.
    bool circular{false}; // Round compact action with room for a soft drop shadow.
};

// Native controls can draw synchronously from input/state messages, outside
// WM_PAINT. Suspend that drawing while retaining USER32 input and accessibility.
class NativeRedrawGuard final {
public:
    explicit NativeRedrawGuard(HWND window, bool enabled = true);
    ~NativeRedrawGuard();
    NativeRedrawGuard(const NativeRedrawGuard&) = delete;
    NativeRedrawGuard& operator=(const NativeRedrawGuard&) = delete;
    static void VisibilityChanged(HWND window, bool visible);
private:
    HWND window_{};
    bool visible_{true};
};

inline constexpr int kWindowChromeHeight = 40;
[[nodiscard]] bool StyleWindowChrome(HWND window, bool resizable);

void DrawIcon(HDC dc, const RECT& rect, Icon icon, COLORREF color, float scale);
// Subclasses the real BUTTON, retaining its original role, radio/check behavior,
// focus, enabled state and Windows accessibility provider.
[[nodiscard]] bool StyleButton(HWND button, ButtonStyle style = {});
void UpdateButtonStyle(HWND button, ButtonStyle style);
/// The client width a styled button needs to show its whole caption, in
/// pixels of its window; 0 for a button that draws no caption. Layout uses
/// it, so a caption that is longer in another language widens its button
/// instead of being cut.
[[nodiscard]] int ButtonContentWidth(HWND button);
[[nodiscard]] bool StyleSlider(HWND slider);
[[nodiscard]] bool StyleInput(HWND input);
[[nodiscard]] bool StyleLabel(HWND label);
void RoundWindow(HWND window, float radius = 8.0f);
/// True on Windows builds that can keep a window out of every screen capture
/// (10.0.19041 and later). Earlier builds show such a window as a black box.
[[nodiscard]] bool CaptureExclusionSupported();
/// True when, in addition, every visible window of this process is excluded.
[[nodiscard]] bool CanCaptureVisibleWindows();
/// Keeps a top-level window out of every screen capture. A window that is not
/// layered becomes layered at full opacity first: on Windows 10 a plain
/// excluded window that is shown again or moved over content that is not
/// redrawn leaves a black hole of its shape in Desktop Duplication and in a
/// GDI blit of the screen, and a layered one does not (ADR-008).
[[nodiscard]] bool ExcludeFromCapture(HWND window);
void DrawSeparator(HDC dc, const RECT& rect, float scale);
[[nodiscard]] bool DrawComboItem(const DRAWITEMSTRUCT& item);
void SetAccessibleName(HWND control, const wchar_t* name);
[[nodiscard]] bool StyleProgress(HWND progress);
void StyleTooltip(HWND tooltip);
void PaintNativePanel(HWND window, float scale, bool grip = false);
[[nodiscard]] LRESULT ControlBackground(WPARAM dc, COLORREF text);
[[nodiscard]] bool ActivateFocusedButton(MSG& message, HWND parent);

inline constexpr UINT kAppearanceLight = 41001;
inline constexpr UINT kAppearanceDark = 41002;
inline constexpr UINT kAppearanceSystem = 41003;
/// Same as Windows, Light, Dark. `follows_system` marks the first one.
[[nodiscard]] HMENU CreateAppearanceMenu(bool follows_system);

// Native menu/dialog semantics stay with USER32; only owned presentation changes.
class MenuThemeScope final {
public:
    MenuThemeScope(HWND owner, HMENU menu);
    ~MenuThemeScope();
    MenuThemeScope(const MenuThemeScope&) = delete;
    MenuThemeScope& operator=(const MenuThemeScope&) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
};
int ShowMessage(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags);
/// The image Save dialog's text in the active language.
[[nodiscard]] platform::SaveImageDialogText SaveImageDialogLabels();
/// One button of `ShowChoice`. The value is what the call returns for it.
struct DialogChoice {
    int value{0};
    const wchar_t* label{nullptr};
    bool destructive{false};
};
/// A question whose answers are named after what they do. `default_value`
/// has the focus; Esc and the close button answer `cancel_value`. Returns the
/// chosen value, or `cancel_value` when the dialog could not be shown.
int ShowChoice(HWND owner, const wchar_t* text, const wchar_t* title, const std::vector<DialogChoice>& choices,
               int default_value, int cancel_value);
/// "Really lose this?" - `confirm` is the destructive answer, `keep` is the
/// default and the answer to Esc. True only when `confirm` was chosen.
[[nodiscard]] bool ConfirmDestructive(HWND owner, const wchar_t* text, const wchar_t* title, const wchar_t* confirm,
                                      const wchar_t* keep);
inline void ShowErrorMessage(HWND owner,const wchar_t* text) { ShowMessage(owner,text,L"Easnap",MB_OK|MB_ICONWARNING); }

// Covers native menu input and capture exclusion; scope also used by the tray.
class PopupScope final {
public:
    explicit PopupScope(HWND invoker);
    ~PopupScope();
    PopupScope(const PopupScope&) = delete;
    PopupScope& operator=(const PopupScope&) = delete;
private:
    HWND invoker_{};
    HHOOK hook_{};
};
[[nodiscard]] bool PopupActive() noexcept;
[[nodiscard]] bool ConsumePopupInput(UINT message, WPARAM key = 0) noexcept;
// The owning window handles kAppearanceLight/kAppearanceDark in WM_COMMAND,
// including native accessibility activation of a menu item.
void ShowAppearanceMenu(HWND invoker);

} // namespace easnap::ui
