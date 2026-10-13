// Easnap tests - synthetic scrolling documents.
//
// Tall pictures with the kinds of content a scrolling screenshot meets: text
// lines, pictures, rules and lists. Everything is generated from a seed, so a
// failing case reproduces exactly.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace easnap_test::synthetic {

/// Deterministic generator: a failing case reproduces from its seed.
class Random {
public:
    explicit Random(uint32_t seed) : state_(seed * 2654435761u + 12345u) {}
    uint32_t Next() {
        state_ = state_ * 1664525u + 1013904223u;
        return state_ >> 8;
    }
    int Range(int low, int high) { return low + static_cast<int>(Next() % static_cast<uint32_t>(high - low + 1)); }
    bool Chance(int percent) { return static_cast<int>(Next() % 100u) < percent; }

private:
    uint32_t state_;
};

inline constexpr uint32_t kWhite = 0xFFFFFFFFu;

[[nodiscard]] inline uint32_t Gray(int level) {
    const uint32_t v = static_cast<uint32_t>(std::clamp(level, 0, 255));
    return 0xFF000000u | (v << 16) | (v << 8) | v;
}

struct Image {
    int width{0};
    int height{0};
    std::vector<uint32_t> pixels;

    Image() = default;
    Image(int w, int h, uint32_t fill) : width(w), height(h), pixels(static_cast<size_t>(w) * h, fill) {}
    [[nodiscard]] uint32_t* Row(int y) { return pixels.data() + static_cast<size_t>(y) * width; }
    [[nodiscard]] const uint32_t* Row(int y) const { return pixels.data() + static_cast<size_t>(y) * width; }
    void Fill(int left, int top, int right, int bottom, uint32_t color) {
        for (int y = std::max(0, top); y < std::min(height, bottom); ++y) {
            for (int x = std::max(0, left); x < std::min(width, right); ++x) { Row(y)[x] = color; }
        }
    }
};

/// One line of pseudo text: words made of vertical strokes of varying height.
inline void DrawTextLine(Image& image, int left, int top, int right, Random& random, int line_height = 14) {
    int x = left;
    while (x < right - 12) {
        const int word_end = std::min(right, x + random.Range(18, 84));
        const uint32_t ink = Gray(random.Range(20, 90));
        for (; x < word_end; ++x) {
            if (!random.Chance(62)) { continue; }
            const int from = top + random.Range(0, line_height / 2);
            const int to = top + random.Range(line_height / 2, line_height);
            for (int y = from; y < to && y < image.height; ++y) {
                if (y >= 0) { image.Row(y)[x] = ink; }
            }
        }
        x += random.Range(6, 12);
    }
}

inline void DrawPicture(Image& image, int left, int top, int right, int bottom, Random& random) {
    const int red = random.Range(40, 220), green = random.Range(40, 220), blue = random.Range(40, 220);
    for (int y = std::max(0, top); y < std::min(image.height, bottom); ++y) {
        for (int x = left; x < std::min(image.width, right); ++x) {
            const int r = std::clamp(red + (x - left) / 3 + static_cast<int>(random.Next() % 24u), 0, 255);
            const int g = std::clamp(green + (y - top) / 2 + static_cast<int>(random.Next() % 24u), 0, 255);
            const int b = std::clamp(blue + ((x + y) & 31), 0, 255);
            image.Row(y)[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                              static_cast<uint32_t>(b);
        }
    }
}

/// An article: paragraphs, headings, pictures and rules on a white page.
inline Image MakeArticle(int width, int height, uint32_t seed) {
    Image page(width, height, kWhite);
    Random random(seed);
    int y = 18;
    while (y < height - 40) {
        const int kind = random.Range(0, 9);
        if (kind == 0) {
            const int picture_height = random.Range(70, 180);
            DrawPicture(page, 32, y, width - random.Range(40, 200), y + picture_height, random);
            y += picture_height + 20;
        } else if (kind == 1) {
            page.Fill(24, y, width - 24, y + 1, Gray(210));
            y += 18;
        } else if (kind == 2) {
            DrawTextLine(page, 24, y, width / 2, random, 20);
            y += 34;
        } else {
            const int lines = random.Range(2, 7);
            for (int line = 0; line < lines && y < height - 20; ++line) {
                const int end = line + 1 == lines ? random.Range(width / 3, width - 30) : width - random.Range(24, 60);
                DrawTextLine(page, 24, y, end, random);
                y += 22;
            }
            y += 14;
        }
    }
    return page;
}

/// A list of equally tall rows. With `distinct_text` false every row is the
/// same picture, so the content repeats exactly every `row_height` pixels.
inline void DrawList(Image& page, int top, int bottom, int row_height, bool distinct_text, uint32_t seed) {
    for (int y = top; y + row_height <= bottom; y += row_height) {
        page.Fill(0, y, page.width, y + row_height, kWhite);
        page.Fill(0, y + row_height - 1, page.width, y + row_height, Gray(216));
        page.Fill(16, y + 8, 16 + row_height - 16, y + row_height - 8, Gray(170));
        Random random(distinct_text ? seed + static_cast<uint32_t>(y) * 7919u : seed);
        DrawTextLine(page, row_height + 12, y + (row_height - 14) / 2, page.width - 40, random);
    }
}

}  // namespace easnap_test::synthetic
