#pragma once

#include "ui/control_ids.h"
#include "ui/icons.h"
#include "platform/paint_buffer.h"

#include "platform/image_output.h"
#include "ui/ui_theme.h"

#include <functional>
#include <memory>
#include <vector>

namespace easnap::app {
struct AppSettings;
} // namespace easnap::app

namespace easnap::ui {

enum class ButtonRole { Standard,
                        Primary,
                        Destructive,
                        Success };
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
/// Everything a styled button's picture depends on at one moment: its style,
/// the native BUTTON state and caption, and the animated depth.
struct ButtonPaintState {
    ButtonStyle style;
    RECT client{};                 // Client rectangle of the button, in pixels.
    float scale{1};                // Monitor DPI scale of the button.
    bool window_enabled{true};     // IsWindowEnabled of the button.
    bool pressed{false};           // BST_PUSHED.
    bool checked{false};           // BST_CHECKED.
    bool focused{false};           // The button has the keyboard focus.
    bool focus_cues_hidden{false}; // UISF_HIDEFOCUS of the button's UI state.
    bool hot{false};               // The pointer is over the button.
    float depth{1};                // Current material depth of the animation.
    LONG_PTR type{BS_PUSHBUTTON};  // BS_TYPEMASK part of the window style.
    const wchar_t* text{L""};      // Caption; not drawn for icon-only buttons and swatches.
    HFONT font{};                  // Caption font (WM_GETFONT).
};
/// Paints a styled button into `dc`: the one painter of every styled BUTTON.
void PaintButtonFace(HDC dc, const ButtonPaintState& state);

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

/// Hides the pointer while the user types, when "Hide pointer while typing"
/// is on, until the pointer really moves. Windows shows the pointer it hid
/// again at the next move, resize or region change of any window, and a
/// text box that grows with its text makes one at every key.
class TypingPointer final {
public:
    TypingPointer() = default;
    ~TypingPointer() { Restore(); }
    TypingPointer(const TypingPointer&) = delete;
    TypingPointer& operator=(const TypingPointer&) = delete;
    /// After a typed character: hides the pointer if the setting asks for it.
    /// `timer_owner` receives kTypingPointerTimer to watch for movement.
    void Typed(HWND timer_owner);
    /// From the owner's WM_TIMER with kTypingPointerTimer: shows the pointer once it moved.
    void Poll();
    /// Shows the pointer again: typing ended, the input lost the focus or its
    /// window closes.
    void Restore();
    [[nodiscard]] bool hidden() const noexcept { return hidden_; }

private:
    HWND timer_owner_{};
    POINT origin_{};
    bool hidden_{false};
};

// Subclasses the real BUTTON, retaining its original role, radio/check behavior,
// focus, enabled state and Windows accessibility provider.
[[nodiscard]] bool StyleButton(HWND button, ButtonStyle style = {});
void UpdateButtonStyle(HWND button, ButtonStyle style);
/// The client width a styled button needs to show its whole caption, in
/// pixels of its window; 0 for a button that draws no caption. Layout uses
/// it, so a caption that is longer in another language widens its button
/// instead of being cut.
[[nodiscard]] int ButtonContentWidth(HWND button);

/// One row of a surface's button table: everything that is fixed about a
/// styled button, so that its icon, caption or tooltip is changed in one place.
struct StyledButtonRow {
    int id{0};                       // Control identifier; 0 for none.
    DWORD window_style{0};           // BS_*, WS_TABSTOP and WS_GROUP bits.
    UINT caption{0};                 // String id of the window text, which is also the accessible name.
    ButtonStyle style{};             // Look in the first (or only) state.
    UINT tooltip{0};                 // String id of the tooltip; 0 for a button without one.
    Icon alternate_icon{Icon::None}; // Icon in the second state of a two-state button.
    UINT alternate_caption{0};       // Caption in the second state; 0 when it does not change.
};
/// The look of `row` in its second state: its style with the alternate icon.
[[nodiscard]] constexpr ButtonStyle AlternateStyle(const StyledButtonRow& row) noexcept {
    ButtonStyle style = row.style;
    if (row.alternate_icon != Icon::None) { style.icon = row.alternate_icon; }
    return style;
}
/// The caption string id of `row` in the first or the second state.
[[nodiscard]] constexpr UINT RowCaption(const StyledButtonRow& row, bool alternate) noexcept {
    return alternate && row.alternate_caption != 0 ? row.alternate_caption : row.caption;
}
/// Where a new button goes in its parent's client area, in pixels.
struct ButtonPlacement {
    int x{0}, y{0}, width{1}, height{1};
};
/// Creates the BUTTON `row` describes as a child of `parent`, with
/// `extra_style` (such as WS_VISIBLE) besides WS_CHILD and the row's bits, in
/// the first or the `alternate` state, and styles it. nullptr on failure.
[[nodiscard]] HWND CreateStyledButton(HWND parent, const StyledButtonRow& row, DWORD extra_style,
                                      ButtonPlacement placement = {}, bool alternate = false);
/// Registers the tooltip of `row`, if it has one, for `button` with `tooltip`;
/// `owner` is the window that receives the tooltip's notifications.
void AddButtonTooltip(HWND tooltip, HWND owner, HWND button, const StyledButtonRow& row);
[[nodiscard]] bool StyleSlider(HWND slider);
[[nodiscard]] bool StyleInput(HWND input);
[[nodiscard]] bool StyleLabel(HWND label);
void RoundWindow(HWND window, float radius = 8.0f);
/// True on Windows builds that can keep a window out of every screen capture
/// (10.0.19041 and later). Earlier builds show such a window as a black box.
[[nodiscard]] bool CaptureExclusionSupported();
/// True when, in addition, every visible window of this process is excluded,
/// apart from windows marked as screen content.
[[nodiscard]] bool CanCaptureVisibleWindows();
/// Marks a window whose pixels belong in captures, such as a pinned image
/// (ADR-038): `CanCaptureVisibleWindows` does not wait for its exclusion.
void MarkAsScreenContent(HWND window);
/// Removes the mark; called before the window is destroyed.
void RemoveScreenContentMark(HWND window) noexcept;
[[nodiscard]] bool IsScreenContent(HWND window) noexcept;
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
/// Paints the uniform panel of a native window (dialogs, Settings, trim) in WM_PAINT.
void PaintNativePanel(HWND window, float scale);
[[nodiscard]] LRESULT ControlBackground(WPARAM dc, COLORREF text);
[[nodiscard]] bool ActivateFocusedButton(MSG& message, HWND parent);

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
/// The image Save dialog's text in the active language.
[[nodiscard]] platform::SaveImageDialogText SaveImageDialogLabels();
/// Where a finished image is saved.
struct ImageSaveDestination {
    std::wstring path;
    /// Straight into the screenshots folder, without the dialog.
    bool quick{false};
};
/// Where to save a finished image as Settings ask: a new name in the
/// screenshots folder with quick save, otherwise the place chosen in the Save
/// dialog; nothing when the dialog was cancelled. `allow_quick` false always
/// asks: after a quick save that failed, the dialog offers every other place.
/// `before_dialog` runs just before the dialog opens, for windows that must
/// not cover it. Without settings the dialog opens in its own folder. The
/// file is written afterwards, on a worker (platform/image_export.h).
[[nodiscard]] std::optional<ImageSaveDestination> ChooseImageDestination(
    HWND owner, const app::AppSettings* settings, bool allow_quick, const std::function<void()>& before_dialog = {});
/// A save to `destination` succeeded: the folder chosen in the dialog is
/// where the next dialog opens.
void RecordImageSaved(app::AppSettings* settings, const ImageSaveDestination& destination);
/// Asks the window under the pointer for its cursor again, when it belongs
/// to this thread: a window that starts or ends busy work shows it at once.
void RefreshCursor();
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
