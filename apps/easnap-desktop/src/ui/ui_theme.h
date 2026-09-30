// Easnap - semantic presentation palette and shared GDI/D2D metrics.
// Source pixels and annotation colors are deliberately outside this domain.
#pragma once

#include "platform/platform_utils.h"
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
struct Palette {
    COLORREF panel, recessed, raised, border, control_border;
    COLORREF text, muted_text, accent, on_accent;
    COLORREF highlight, shadow, hover, pressed, selected, focus, disabled;
    COLORREF success, error, on_error, recording, paused;
    COLORREF screenshot_border, record_border, handle, guide;
    COLORREF timeline_track, timeline_selection, playhead, tooltip, status;
    bool high_contrast{false};
};

[[nodiscard]] const Palette& Current() noexcept;
[[nodiscard]] Palette NormalPalette(app::AppearanceTheme preference) noexcept;
[[nodiscard]] app::AppearanceTheme Preference() noexcept;
// Only the owning UI thread applies resources; neither operation changes a session.
void ApplyPreference(app::AppearanceTheme preference) noexcept;
void RefreshSystemColors() noexcept;
void NotifyWindows() noexcept;
inline constexpr UINT kChangedMessage = WM_APP + 0x310;

[[nodiscard]] D2D1_COLOR_F D2DColor(COLORREF color, float alpha = 1.0f) noexcept;

// ---------------------------------------------------------------------------
// Metrics (device-independent pixels; multiply by the monitor DPI scale)
// ---------------------------------------------------------------------------

inline constexpr float kPaletteButtonWidth = 148.0f;
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
