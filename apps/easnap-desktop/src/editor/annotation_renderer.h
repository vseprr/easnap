// Easnap - how annotations are drawn with Direct2D, shared by the editor's
// preview and its export.
#pragma once

#include <algorithm>

#include <d2d1.h>

#include "editor/annotation_document.h"

namespace easnap::editor {

/// Selection chrome in device-independent pixels; it keeps its size on screen
/// at every zoom. A frame follows the outline, with a gap around text, and
/// handles are rings on the corners or ends.
inline constexpr float kSelectionStroke = 1.2f;  ///< Width of the frame.
inline constexpr float kSelectionTextGap = 3.0f;  ///< Gap between text and its frame.
inline constexpr float kHandleRadius = 4.5f;  ///< Radius of a handle ring.
inline constexpr float kHandleStroke = 1.5f;  ///< Width of a handle ring.
/// How far the chrome reaches past an annotation's bounds.
inline constexpr float kSelectionReach = std::max(kHandleRadius + kHandleStroke / 2, kSelectionTextGap + kSelectionStroke / 2);

/// The shared stroke style with round caps and joins; null before Direct2D exists.
ID2D1StrokeStyle* RoundStrokeStyle();

/// Draws one annotation in image space. Mosaic strokes are baked into the base
/// bitmap beforehand, so they only render a translucent trail while being drawn.
void DrawAnnotation(ID2D1RenderTarget* target, const Annotation& annotation,
                    ID2D1SolidColorBrush* brush, bool preview);
/// Draws the selection frame and resize handles of `annotation` in image
/// space, `unit` image pixels per device-independent pixel. None for a watermark.
void DrawSelectionChrome(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, const Annotation& annotation, float unit);

}  // namespace easnap::editor
