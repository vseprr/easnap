// Easnap - semantic presentation palette and shared GDI/D2D metrics.
// Source pixels and annotation colors are deliberately outside this domain.
#pragma once

#include <windows.h>
#include <d2d1.h>

#include "platform/app_messages.h"
#include "app/appearance_theme.h"

namespace easnap::ui::theme {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

/// Magenta is used as the transparency key of the overlay chrome window. No
/// Easnap colour may match it, otherwise that part of the chrome would vanish.
inline constexpr COLORREF kChromeColorKey = RGB(255, 0, 255);

/// Constant alpha applied to the dimming window (0 = invisible, 255 = opaque).
inline constexpr BYTE kDimAlpha = 122;

inline constexpr COLORREF kDim = RGB(0, 0, 0);

// Theme-independent chrome colours. They are drawn over content or next to an
// annotation colour, so neither the palette nor high contrast changes them.

/// Check mark on a selected colour swatch whose colour is light.
inline constexpr COLORREF kSwatchCheckDark = RGB(30, 30, 30);
/// Check mark on a selected colour swatch whose colour is dark.
inline constexpr COLORREF kSwatchCheckLight = RGB(255, 255, 255);
/// A swatch colour whose R+G+B exceeds this is light and takes the dark check.
inline constexpr int kSwatchCheckThreshold = 500;
/// Outer ring of the magnifier's two-tone reticle; the inner ring is kDim.
inline constexpr COLORREF kReticleLight = RGB(255, 255, 255);
/// Fill of the editor's selection handles, white in every theme.
inline constexpr COLORREF kSelectionHandleFill = RGB(255, 255, 255);

struct Palette {
    COLORREF panel, recessed, raised, border, control_border;
    COLORREF text, muted_text, accent, on_accent;
    COLORREF highlight, shadow, hover, pressed, selected, focus, disabled;
    COLORREF success, error, on_error, recording, paused;
    COLORREF screenshot_border, record_border, handle, guide;
    COLORREF timeline_track, timeline_selection, playhead, tooltip, status;
    bool high_contrast{false};
    // Derived when the palette is built, from the fields above.
    COLORREF separator{};    // Group separators: panel mixed with text; text in high contrast.
    COLORREF scroll_thumb{}; // Document scroll thumb: panel mixed with text, also in high contrast.
};

[[nodiscard]] const Palette& Current() noexcept;
[[nodiscard]] Palette NormalPalette(app::AppearanceTheme preference) noexcept;
[[nodiscard]] app::AppearanceTheme Preference() noexcept;
// Only the owning UI thread applies resources; neither operation changes a session.
void ApplyPreference(app::AppearanceTheme preference) noexcept;
void RefreshSystemColors() noexcept;
void NotifyWindows() noexcept;

[[nodiscard]] D2D1_COLOR_F D2DColor(COLORREF color, float alpha = 1.0f) noexcept;

// ---------------------------------------------------------------------------
// Metrics (device-independent pixels; multiply by the monitor DPI scale)
// ---------------------------------------------------------------------------

/// Mode buttons are as wide as their widest label needs, but not narrower.
inline constexpr float kPaletteButtonMinimumWidth = 112.0f;
inline constexpr float kPaletteButtonHeight = 36.0f;
inline constexpr float kPalettePadding = 8.0f;
inline constexpr float kPaletteTopMargin = 0.0f;
inline constexpr float kHintHeight = 36.0f;
inline constexpr float kHintBottomMargin = 52.0f;
inline constexpr float kSelectionBorder = 2.0f;
inline constexpr float kHandleSize = 9.0f;
inline constexpr float kHandleHitRadius = 10.0f;
inline constexpr float kCornerRadius = 8.0f;
inline constexpr int kMagnifierGridPixels = 25;
inline constexpr float kMagnifierCell = 4.0f;
inline constexpr float kMagnifierWidth = 152.0f;
inline constexpr float kMagnifierPadding = 4.0f;
inline constexpr float kMagnifierLineHeight = 18.0f;
inline constexpr float kMagnifierPointerGap = 18.0f;
inline constexpr float kPanelRadius = 14.0f;
inline constexpr float kControlRadius = 8.0f;
inline constexpr float kControlHeight = 36.0f;
inline constexpr float kPanelPadding = 16.0f;

// Icons (ui/icons.cpp): paths on a 24-unit grid, stroked in grid units.
inline constexpr float kIconGrid = 24.0f;         // Width and height of the icon grid.
inline constexpr float kIconStroke = 1.8f;        // Pen of every icon stroke.
inline constexpr float kIconOutlineStroke = 1.2f; // Thinner pen of the outlined T.

// Styled buttons: PaintButtonFace draws with these and ButtonContentWidth
// measures with the same values.
inline constexpr float kButtonFaceInset = 6.0f;         // Client edge to the material face.
inline constexpr float kButtonIconInset = 10.0f;        // Face edge to a leading icon.
inline constexpr float kButtonIconSize = 18.0f;         // Icon box of a button.
inline constexpr float kButtonCircularIconSize = 12.0f; // Icon box of a circular button.
inline constexpr float kButtonDropdownShift = 5.0f;     // Icon of a dropdown button moves left by this.
inline constexpr float kButtonChevronSize = 8.0f;       // Trailing chevron of a dropdown button.
inline constexpr float kButtonChevronInset = 3.0f;      // Face edge to the trailing chevron.
inline constexpr float kButtonLabelInset = 6.0f;        // Face edge to the caption.
inline constexpr float kButtonIconLabelGap = 6.0f;      // Leading icon to the caption.
inline constexpr float kButtonCheckInset = 10.0f;       // Face edge to a check or radio marker.
inline constexpr float kButtonCheckSize = 10.0f;        // Check or radio marker box.
inline constexpr float kButtonCheckLabelGap = 8.0f;     // Check or radio marker to the caption.
inline constexpr float kButtonFocusInset = 10.0f;       // Face edges to the ends of the focus underline.
inline constexpr float kSwatchRadius = 3.0f;            // Corner of a colour swatch.

// Motion, in milliseconds.
inline constexpr float kButtonSettleMs = 140.0f;   // First depth change of a new button.
inline constexpr float kButtonPressMs = 85.0f;     // Depth change to pressed.
inline constexpr float kButtonCheckMs = 160.0f;    // Depth change to checked.
inline constexpr float kButtonHoverMs = 145.0f;    // Every other depth change (hover, release).
inline constexpr UINT kAnimationTickMs = 15;       // Timer of button and island animations.
inline constexpr float kIslandFadeMs = 160.0f;     // Fade of the editor's contextual island.

// Islands and panels.
inline constexpr float kIslandGripWidth = 24.0f; // Grip of the mode, editor and Scrollshot islands.
inline constexpr float kPanelGripWidth = 28.0f;  // Grip of the recording panel and native panels.
inline constexpr int kIslandGap = 8;             // Between an island and the frame or another island.
inline constexpr int kIslandMargin = 12;         // Between an island and the work area edge.

// Custom window caption (ui/window_chrome).
inline constexpr int kCaptionHeight = 40;                       // Height of the caption.
inline constexpr float kCaptionButtonSize = 34.0f;              // Square caption button.
inline constexpr float kCaptionButtonGap = 6.0f;                // Between caption buttons and to the edge.
inline constexpr float kResizeEdge = 6.0f;                      // Resize hit band of a resizable window.
inline constexpr float kCaptionTitleInset = 20.0f;              // Left edge to the title.
inline constexpr float kCaptionTitleReserveResizable = 130.0f;  // Right of the title, three buttons.
inline constexpr float kCaptionTitleReserveFixed = 52.0f;       // Right of the title, close only.

// Owner-drawn menus (MenuThemeScope).
inline constexpr float kMenuItemHeight = 36.0f;       // Height of a command item.
inline constexpr float kMenuSeparatorHeight = 10.0f;  // Height of a separator item.
inline constexpr float kMenuTextAllowance = 72.0f;    // Item width beyond its text.
inline constexpr float kMenuSeparatorInset = 12.0f;   // Item edges to the separator line.
inline constexpr float kMenuSelectionInsetX = 6.0f;   // Item edges to the selection face, horizontally.
inline constexpr float kMenuSelectionInsetY = 4.0f;   // Item edges to the selection face, vertically.
inline constexpr float kMenuSelectionRadius = 6.0f;   // Corner of the selection face.
inline constexpr float kMenuSelectionDepth = -0.8f;   // Material depth of the selection face.
inline constexpr float kMenuLabelInset = 34.0f;       // Item left edge to the label.
inline constexpr float kMenuLabelTrailing = 24.0f;    // Label right end to the item right edge.
inline constexpr float kMenuCheckInset = 9.0f;        // Item left edge to the check mark.
inline constexpr float kMenuCheckSize = 16.0f;        // Check mark box.
inline constexpr float kMenuArrowInset = 14.0f;       // Item right edge to the submenu arrow tip.
inline constexpr float kMenuArrowSize = 3.0f;         // Half height of the submenu arrow.

/// Widest a tooltip grows before it wraps.
inline constexpr float kTooltipMaxWidth = 360.0f;

// Font roles (CreateUiFont sizes, DIP).
inline constexpr float kBodyFontSize = 13.0f;    // Control text, menus, captions (semibold), island labels.
inline constexpr float kHeadingFontSize = 15.0f; // Semibold headings, mode buttons, notices and elapsed time.
inline constexpr float kDialogFontSize = 14.0f;  // Message dialogs and Settings.
inline constexpr float kTrimFontSize = 12.5f;    // The trim window.

/// Rounds a device-independent length to whole physical pixels (minimum 1).
[[nodiscard]] int Scaled(float dip, float scale) noexcept;

// ---------------------------------------------------------------------------
// GDI drawing helpers
// ---------------------------------------------------------------------------

void FillSolid(HDC dc, const RECT& rect, COLORREF color);
void FrameSolid(HDC dc, const RECT& rect, COLORREF color, int thickness);
void FrameRounded(HDC dc, const RECT& rect, int radius, COLORREF color, int thickness);
void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF fill);
void FillRoundedBordered(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border, int thickness);
void DrawSolidLine(HDC dc, POINT from, POINT to, COLORREF color, int thickness);
void DrawPanel(HDC dc, const RECT& rect, float scale);
void DrawGrip(HDC dc, const RECT& rect, float scale);
// Cached, Gaussian-filtered masks. Depth -1 is recessed; +1 is raised.
// The enclosing material stays a uniform solid color.
void DrawMaterial(HDC dc, const RECT& rect, float radius, float scale,
                  COLORREF face, float depth = 1.0f, bool drop_shadow = false);
[[nodiscard]] bool AnimationsEnabled() noexcept;
[[nodiscard]] COLORREF Mix(COLORREF a, COLORREF b, float amount) noexcept;

/// Draws single-line text with a transparent background. `format` takes the
/// usual DT_* flags; the caller keeps ownership of `font`.
void DrawLabel(HDC dc, const RECT& rect, const wchar_t* text, COLORREF color, HFONT font, UINT format);

/// Creates a Segoe UI font at `size_dip` scaled to the monitor.
[[nodiscard]] HFONT CreateUiFont(float size_dip, float scale, bool semi_bold);

}  // namespace easnap::ui::theme
