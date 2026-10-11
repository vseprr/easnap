#include "editor/redaction_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace easnap::editor {

namespace {

RECT PixelRect(const D2D1_RECT_F& box, int width, int height) {
    return RECT{std::clamp(static_cast<LONG>(std::floor(box.left)), 0L, static_cast<LONG>(width)),
                std::clamp(static_cast<LONG>(std::floor(box.top)), 0L, static_cast<LONG>(height)),
                std::clamp(static_cast<LONG>(std::ceil(box.right)), 0L, static_cast<LONG>(width)),
                std::clamp(static_cast<LONG>(std::ceil(box.bottom)), 0L, static_cast<LONG>(height))};
}

int BlurRadius(const RedactionEffect& effect) {
    return std::clamp(static_cast<int>(1 + effect.strength * 0.35f), 1, 36);
}

int BlockSize(const RedactionEffect& effect) {
    return std::clamp(static_cast<int>(2 + effect.strength * 0.7f), 2, 72);
}

/// Pixels the effect may change.
RECT WriteRect(const RedactionEffect& effect, int width, int height) {
    const RECT rect = PixelRect(effect.box, width, height);
    return ::IsRectEmpty(&rect) ? RECT{} : rect;
}

/// Pixels the effect's result depends on. Three box passes reach three radii.
RECT ReadRect(const RedactionEffect& effect, int width, int height) {
    RECT rect = WriteRect(effect, width, height);
    if (::IsRectEmpty(&rect) || effect.kind != RedactionKind::Blur) {
        return rect;
    }
    const LONG reach = BlurRadius(effect) * 3;
    rect.left = std::max(0L, rect.left - reach);
    rect.top = std::max(0L, rect.top - reach);
    rect.right = std::min(static_cast<LONG>(width), rect.right + reach);
    rect.bottom = std::min(static_cast<LONG>(height), rect.bottom + reach);
    return rect;
}

void Extend(RECT& target, const RECT& rect) {
    if (::IsRectEmpty(&rect)) {
        return;
    }
    if (::IsRectEmpty(&target)) {
        target = rect;
        return;
    }
    target.left = std::min(target.left, rect.left);
    target.top = std::min(target.top, rect.top);
    target.right = std::max(target.right, rect.right);
    target.bottom = std::max(target.bottom, rect.bottom);
}

bool Intersects(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

/// Darkening shared by every spotlight, or a negative value when none is on the image.
float SpotlightDim(const std::vector<RedactionEffect>& effects, int width, int height) {
    float dim = -1.0f;
    for (const auto& effect : effects) {
        if (effect.kind != RedactionKind::Spotlight) {
            continue;
        }
        const RECT rect = WriteRect(effect, width, height);
        if (!::IsRectEmpty(&rect)) {
            dim = std::max({dim, 0.0f, effect.dim});
        }
    }
    return dim;
}

// Three separable box passes approximate a Gaussian. Sliding sums keep cost
// linear in pixels, independent of strength.
void SmoothBlur(platform::BgraImage& image, int radius) {
    std::vector<uint32_t> scratch(image.pixels.size());
    const int w = image.width, h = image.height;
    const auto pass = [&](bool horizontal) {
        const int major = horizontal ? h : w, minor = horizontal ? w : h;
        const int count = radius * 2 + 1;
        for (int row = 0; row < major; ++row) {
            const auto index = [&](int col) {
                return horizontal ? static_cast<size_t>(row) * w + std::clamp(col, 0, w - 1)
                                  : static_cast<size_t>(std::clamp(col, 0, h - 1)) * w + row;
            };
            int red = 0, green = 0, blue = 0;
            const auto accumulate = [&](uint32_t pixel, int sign) {
                red += static_cast<int>((pixel >> 16) & 255) * sign;
                green += static_cast<int>((pixel >> 8) & 255) * sign;
                blue += static_cast<int>(pixel & 255) * sign;
            };
            for (int k = -radius; k <= radius; ++k) {
                accumulate(image.pixels[index(k)], 1);
            }
            for (int col = 0; col < minor; ++col) {
                scratch[index(col)] = 0xff000000u | (static_cast<uint32_t>(red / count) << 16) |
                                      (static_cast<uint32_t>(green / count) << 8) |
                                      static_cast<uint32_t>(blue / count);
                accumulate(image.pixels[index(col - radius)], -1);
                accumulate(image.pixels[index(col + radius + 1)], 1);
            }
        }
        image.pixels.swap(scratch);
    };
    for (int i = 0; i < 3; ++i) {
        pass(true);
        pass(false);
    }
}

/// Marks a filled circle of `radius` pixels centred on (cx, cy) in the `area`-sized mask.
void Dab(float cx, float cy, float radius, const RECT& area, std::vector<uint8_t>& mask) {
    const int x0 = std::max(static_cast<int>(area.left), static_cast<int>(std::floor(cx - radius)));
    const int y0 = std::max(static_cast<int>(area.top), static_cast<int>(std::floor(cy - radius)));
    const int x1 = std::min(static_cast<int>(area.right) - 1, static_cast<int>(std::ceil(cx + radius)));
    const int y1 = std::min(static_cast<int>(area.bottom) - 1, static_cast<int>(std::ceil(cy + radius)));
    const size_t stride = static_cast<size_t>(area.right - area.left);
    const float radius_sq = radius * radius;
    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            if (dx * dx + dy * dy <= radius_sq) {
                mask[static_cast<size_t>(y - area.top) * stride + static_cast<size_t>(x - area.left)] = 1;
            }
        }
    }
}

/// Walks the polyline and stamps overlapping dabs so the trail is solid.
void StampBrush(const RedactionEffect& effect, const RECT& area, std::vector<uint8_t>& mask) {
    const auto& points = effect.brush_path;
    if (points.empty()) {
        return;
    }
    const float radius = effect.brush_radius;
    const float step = std::max(1.0f, radius * 0.4f);
    Dab(points.front().x, points.front().y, radius, area, mask);
    for (size_t i = 1; i < points.size(); ++i) {
        const D2D1_POINT_2F a = points[i - 1];
        const D2D1_POINT_2F b = points[i];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        const int segments = std::max(1, static_cast<int>(std::ceil(length / step)));
        for (int s = 1; s <= segments; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(segments);
            Dab(a.x + dx * t, a.y + dy * t, radius, area, mask);
        }
    }
}

/// How much of the pixel centre (x, y) lies inside the spotlight, 0..1.
float SpotlightCoverage(const RedactionEffect& effect, LONG x, LONG y) {
    const D2D1_RECT_F& box = effect.box;
    const float rx = (box.right - box.left) / 2, ry = (box.bottom - box.top) / 2;
    const float px = x + 0.5f - (box.left + rx), py = y + 0.5f - (box.top + ry);
    if (effect.outline == RedactionOutline::Ellipse && rx > 0 && ry > 0) {
        return std::clamp((1 - std::hypot(px / rx, py / ry)) * std::min(rx, ry) + 0.5f, 0.0f, 1.0f);
    }
    if (effect.outline == RedactionOutline::Rounded) {
        const float radius = std::min({rx, ry, effect.corner_radius});
        const float qx = std::abs(px) - (rx - radius), qy = std::abs(py) - (ry - radius);
        const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) +
                        std::min(std::max(qx, qy), 0.0f) - radius;
        return std::clamp(0.5f - d, 0.0f, 1.0f);
    }
    return 1.0f;
}

}  // namespace

bool RedactionEffect::operator==(const RedactionEffect& other) const noexcept {
    return kind == other.kind && box.left == other.box.left && box.top == other.box.top &&
           box.right == other.box.right && box.bottom == other.box.bottom && brush == other.brush &&
           brush_radius == other.brush_radius && strength == other.strength &&
           outline == other.outline && corner_radius == other.corner_radius && dim == other.dim &&
           brush_path.size() == other.brush_path.size() &&
           std::equal(brush_path.begin(), brush_path.end(), other.brush_path.begin(),
                      [](const D2D1_POINT_2F& a, const D2D1_POINT_2F& b) { return a.x == b.x && a.y == b.y; });
}

void RedactionRenderer::Reset() {
    image_ = platform::BgraImage{};
    effects_.clear();
    valid_ = false;
}

RECT RedactionRenderer::Update(const platform::BgraImage& source, std::vector<RedactionEffect> effects) {
    if (source.empty()) {
        Reset();
        return {};
    }
    const int w = source.width, h = source.height;
    const bool rebuild = !valid_ || image_.width != w || image_.height != h;

    // Everything between the common head and tail of the two lists changed.
    RECT dirty{};
    if (rebuild) {
        dirty = RECT{0, 0, w, h};
    } else {
        const size_t shortest = std::min(effects_.size(), effects.size());
        size_t head = 0;
        while (head < shortest && effects_[head] == effects[head]) {
            ++head;
        }
        size_t tail = 0;
        while (tail < shortest - head &&
               effects_[effects_.size() - 1 - tail] == effects[effects.size() - 1 - tail]) {
            ++tail;
        }
        if (head == effects_.size() && head == effects.size()) {
            return {};
        }
        if (SpotlightDim(effects_, w, h) != SpotlightDim(effects, w, h)) {
            dirty = RECT{0, 0, w, h};  // The dim layer covers the whole image.
        } else {
            for (size_t i = head; i < effects_.size() - tail; ++i) {
                Extend(dirty, ReadRect(effects_[i], w, h));
            }
            for (size_t i = head; i < effects.size() - tail; ++i) {
                Extend(dirty, ReadRect(effects[i], w, h));
            }
        }
    }

    // An effect that reads a dirty pixel must be reapplied, and then everything
    // it reads has to be rebuilt from the source as well. Growing the rectangle
    // until it is closed under that rule leaves every other effect's input and
    // output untouched, so those keep their pixels.
    std::vector<char> selected(effects.size(), 0);
    for (bool grew = !::IsRectEmpty(&dirty); grew;) {
        grew = false;
        for (size_t i = 0; i < effects.size(); ++i) {
            if (selected[i] || effects[i].kind == RedactionKind::Spotlight) {
                continue;
            }
            const RECT read = ReadRect(effects[i], w, h);
            if (::IsRectEmpty(&read) || !Intersects(read, dirty)) {
                continue;
            }
            selected[i] = 1;
            const RECT before = dirty;
            Extend(dirty, read);
            grew = grew || !::EqualRect(&before, &dirty);
        }
    }

    valid_ = false;
    effects_ = std::move(effects);
    if (::IsRectEmpty(&dirty)) {
        valid_ = true;
        return {};
    }

    const size_t dirty_width = static_cast<size_t>(dirty.right - dirty.left);
    if (rebuild) {
        image_ = source;
    } else {
        for (LONG y = dirty.top; y < dirty.bottom; ++y) {
            std::memcpy(image_.row(static_cast<int>(y)) + dirty.left,
                        source.row(static_cast<int>(y)) + dirty.left, dirty_width * sizeof(uint32_t));
        }
    }

    for (size_t i = 0; i < effects_.size(); ++i) {
        if (!selected[i]) {
            continue;
        }
        const RedactionEffect& effect = effects_[i];
        const RECT area = WriteRect(effect, w, h);
        const size_t area_width = static_cast<size_t>(area.right - area.left);
        mask_.assign(area_width * static_cast<size_t>(area.bottom - area.top),
                     effect.brush ? uint8_t{0} : uint8_t{1});
        if (effect.brush) {
            StampBrush(effect, area, mask_);
        }
        if (effect.kind == RedactionKind::Pixelate) {
            platform::ApplyMosaicMasked(image_, area, mask_, BlockSize(effect));
            continue;
        }
        const RECT read = ReadRect(effect, w, h);
        platform::BgraImage blurred;
        if (!platform::CropImage(image_, read, blurred)) {
            continue;
        }
        SmoothBlur(blurred, BlurRadius(effect));
        for (LONG y = area.top; y < area.bottom; ++y) {
            const uint8_t* covered = mask_.data() + static_cast<size_t>(y - area.top) * area_width;
            const uint32_t* from = blurred.row(static_cast<int>(y - read.top)) + (area.left - read.left);
            uint32_t* to = image_.row(static_cast<int>(y)) + area.left;
            for (size_t x = 0; x < area_width; ++x) {
                if (covered[x]) {
                    to[x] = from[x];
                }
            }
        }
    }

    // Spotlights darken everything outside their union, after every pixel effect.
    const float dim = SpotlightDim(effects_, w, h);
    if (dim >= 0.0f) {
        spots_.assign(dirty_width * static_cast<size_t>(dirty.bottom - dirty.top), 0.0f);
        for (const auto& effect : effects_) {
            if (effect.kind != RedactionKind::Spotlight) {
                continue;
            }
            RECT area = WriteRect(effect, w, h);
            if (!::IntersectRect(&area, &area, &dirty)) {
                continue;
            }
            for (LONG y = area.top; y < area.bottom; ++y) {
                float* row = spots_.data() + static_cast<size_t>(y - dirty.top) * dirty_width;
                for (LONG x = area.left; x < area.right; ++x) {
                    float& value = row[x - dirty.left];
                    value = std::max(value, SpotlightCoverage(effect, x, y));
                }
            }
        }
        for (LONG y = dirty.top; y < dirty.bottom; ++y) {
            const float* spot = spots_.data() + static_cast<size_t>(y - dirty.top) * dirty_width;
            uint32_t* row = image_.row(static_cast<int>(y)) + dirty.left;
            for (size_t x = 0; x < dirty_width; ++x) {
                const float factor = 1 - dim * (1 - spot[x]);
                const uint32_t pixel = row[x];
                row[x] = 0xff000000u | (static_cast<uint32_t>(((pixel >> 16) & 255) * factor) << 16) |
                         (static_cast<uint32_t>(((pixel >> 8) & 255) * factor) << 8) |
                         static_cast<uint32_t>((pixel & 255) * factor);
            }
        }
    }

    valid_ = true;
    return dirty;
}

}  // namespace easnap::editor
