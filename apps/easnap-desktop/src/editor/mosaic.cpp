// Easnap - pixelation of a masked area: the mosaic redaction effect.
#include "editor/mosaic.h"

#include <algorithm>

namespace easnap::editor {

using platform::IsEmptyRect;

void ApplyMosaicMasked(platform::BgraImage& image, const RECT& area, const std::vector<uint8_t>& mask,
                       int block_size) {
    if (image.empty() || block_size <= 1 || IsEmptyRect(area) || area.left < 0 || area.top < 0 ||
        area.right > image.width || area.bottom > image.height) {
        return;
    }
    const size_t area_width = static_cast<size_t>(area.right - area.left);
    if (mask.size() != area_width * static_cast<size_t>(area.bottom - area.top)) {
        return;
    }
    const RECT& clipped = area;

    const auto covered = [&](LONG x, LONG y) {
        return mask[static_cast<size_t>(y - area.top) * area_width + static_cast<size_t>(x - area.left)] != 0;
    };

    for (LONG y = clipped.top; y < clipped.bottom; y += block_size) {
        const LONG y_end = std::min<LONG>(y + block_size, clipped.bottom);
        for (LONG x = clipped.left; x < clipped.right; x += block_size) {
            const LONG x_end = std::min<LONG>(x + block_size, clipped.right);
            uint64_t b = 0, g = 0, r = 0;
            uint64_t count = 0;
            for (LONG yy = y; yy < y_end; ++yy) {
                const uint32_t* row = image.row(static_cast<int>(yy));
                for (LONG xx = x; xx < x_end; ++xx) {
                    if (!covered(xx, yy)) {
                        continue;
                    }
                    const uint32_t pixel = row[xx];
                    b += pixel & 0xFFu;
                    g += (pixel >> 8) & 0xFFu;
                    r += (pixel >> 16) & 0xFFu;
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const uint32_t average = 0xFF000000u | (static_cast<uint32_t>(r / count) << 16) |
                                     (static_cast<uint32_t>(g / count) << 8) | static_cast<uint32_t>(b / count);
            for (LONG yy = y; yy < y_end; ++yy) {
                uint32_t* row = image.row(static_cast<int>(yy));
                for (LONG xx = x; xx < x_end; ++xx) {
                    if (covered(xx, yy)) {
                        row[xx] = average;
                    }
                }
            }
        }
    }
}

}  // namespace easnap::editor
