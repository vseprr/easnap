// Easnap - annotation content colours. These are part of the picture the user
// makes, not of the interface: no theme, palette or high-contrast setting
// changes them (ADR-016), so this header does not include the UI theme.
#pragma once

#include <d2d1.h>
#include <d2d1helper.h>

#include <array>

namespace easnap::editor {

/// The colours offered by the editor's swatches, left to right: red, amber,
/// green, Easnap blue, violet, white, black and gray.
constexpr std::array<D2D1_COLOR_F, 8> kAnnotationSwatchColors = {{
    {0.898f, 0.243f, 0.259f, 1.0f},  // red
    {0.976f, 0.639f, 0.157f, 1.0f},  // amber
    {0.188f, 0.686f, 0.427f, 1.0f},  // green
    {0.161f, 0.635f, 1.0f, 1.0f},    // Easnap blue
    {0.612f, 0.318f, 0.851f, 1.0f},  // violet
    {1.0f, 1.0f, 1.0f, 1.0f},        // white
    {0.0f, 0.0f, 0.0f, 1.0f},        // black
    {0.5f, 0.5f, 0.5f, 1.0f},        // gray
}};

/// Colour of a new session's annotations, "signal red". It is none of the
/// swatch colours, so no swatch is selected when a session starts.
constexpr D2D1_COLOR_F kDefaultAnnotationColor{1.0f, 0.259f, 0.286f, 1.0f};

/// A text colour whose red, green and blue all exceed this counts as white:
/// filled and outlined text and tag text then use the dark ink.
constexpr float kWhiteTextThreshold = 0.98f;
/// Glyph ink of filled and outlined text on a white colour.
inline const D2D1::ColorF kTextInkOnWhite{D2D1::ColorF::Black};
/// Glyph ink of filled and outlined text on every other colour.
inline const D2D1::ColorF kTextInk{D2D1::ColorF::White};

/// Graphite body and notch of a tag.
inline const D2D1::ColorF kTagBody{0x30343c};
/// Text of a tag.
inline const D2D1::ColorF kTagText{D2D1::ColorF::White};

/// Translucent trail of a mosaic brush stroke while it is being drawn.
inline const D2D1::ColorF kMosaicBrushTrail{0.85f, 0.89f, 0.98f, 0.45f};

}  // namespace easnap::editor
