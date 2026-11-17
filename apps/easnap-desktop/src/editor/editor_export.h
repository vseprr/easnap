// Easnap - the screenshot editor's export: annotations composited onto the image.
//
// Annotations stay vector commands while the image is edited; they become
// pixels only here, when the image is copied, saved or pinned.
#pragma once

#include <vector>

#include "editor/annotation_document.h"
#include "platform/bgra_image.h"

namespace easnap::editor {

/// A picture up to this height is one Direct2D bitmap and one composite pass,
/// as every screenshot is. A taller document is shown and exported in bands,
/// which keeps each bitmap within what Direct2D and WIC accept.
inline constexpr int kWholeBitmapRows = 8192;

/// `annotations` drawn onto `base_image` at its full resolution, opaque.
/// A picture taller than kWholeBitmapRows is composited in bands. The base
/// image itself when there is nothing to draw; empty on failure.
[[nodiscard]] platform::BgraImage CompositeAnnotations(const platform::BgraImage& base_image, const std::vector<Annotation>& annotations);

}  // namespace easnap::editor
