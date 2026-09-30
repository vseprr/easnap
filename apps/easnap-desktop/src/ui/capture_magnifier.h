// Frozen-pixel sampling and presentation policy for the native capture overlay.
#pragma once

#include <optional>
#include <span>
#include <string>

#include "platform/platform_utils.h"

namespace easnap::ui {

enum class ColorRepresentation { Rgb, Hex };

// Top-down BGRA pixels, addressed in physical virtual-desktop coordinates.
// DPI affects chrome only. Missing/mismatched pixels are unavailable, never
// stretched or replaced with live screen reads.
[[nodiscard]] std::optional<COLORREF> SampleDesktopPixel(
    std::span<const uint32_t> pixels, int width, int height,
    const RECT& desktop_bounds, POINT cursor) noexcept;
[[nodiscard]] std::wstring FormatCaptureColor(COLORREF color, ColorRepresentation representation);

// A bare Shift toggles on release; repeats and modified chords cannot toggle.
class CaptureColorReadout {
public:
    void OnKeyDown(WPARAM key, bool repeated, bool control, bool alt) noexcept;
    bool OnKeyUp(WPARAM key) noexcept;
    void CancelPress() noexcept { shift_candidate_ = false; }
    [[nodiscard]] ColorRepresentation representation() const noexcept { return representation_; }

private:
    ColorRepresentation representation_{ColorRepresentation::Rgb};
    bool shift_candidate_{false};
};

// Tries cursor-adjacent quadrants and clamps to this monitor. The toolbar's
// entire panel is reserved, including its padding and the gap between modes.
[[nodiscard]] RECT PlaceCaptureMagnifier(
    POINT cursor, const RECT& monitor, SIZE size, LONG gap, const RECT& toolbar) noexcept;

}  // namespace easnap::ui
