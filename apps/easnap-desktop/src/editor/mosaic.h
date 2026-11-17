// Easnap - pixelation of a masked area: the mosaic redaction effect.
#pragma once

#include <cstdint>
#include <vector>

#include "platform/bgra_image.h"

namespace easnap::editor {

/// Mosaic restricted to the pixels of `area` whose `mask` entry is non-zero.
///
/// `mask` has one entry per pixel of `area`, row by row. `area` must lie inside
/// the image. Blocks are anchored at the top-left corner of `area` and average
/// only the masked pixels they contain.
void ApplyMosaicMasked(platform::BgraImage& image, const RECT& area, const std::vector<uint8_t>& mask,
                       int block_size);

}  // namespace easnap::editor
