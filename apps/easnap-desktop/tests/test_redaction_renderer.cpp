// Easnap unit tests - incremental redaction rendering.
//
// The renderer recomputes only the rectangle an edit can influence. These
// tests hold it to the definition it must preserve: after every edit the
// image equals a rebuild from the capture, and nothing outside the reported
// rectangle moved. No window, device or desktop is used.

#include "editor/redaction_renderer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using easnap::editor::RedactionEffect;
using easnap::editor::RedactionKind;
using easnap::editor::RedactionOutline;
using easnap::editor::RedactionRenderer;
using easnap::platform::BgraImage;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

/// Deterministic generator: a failing sequence reproduces from its seed.
class Random {
public:
    explicit Random(uint32_t seed) : state_(seed) {}
    uint32_t Next() {
        state_ = state_ * 1664525u + 1013904223u;
        return state_ >> 8;
    }
    int Range(int low, int high) { return low + static_cast<int>(Next() % static_cast<uint32_t>(high - low + 1)); }
    float Real(float low, float high) { return low + (high - low) * (static_cast<float>(Next() % 4096u) / 4095.0f); }

private:
    uint32_t state_;
};

BgraImage MakeSource(int width, int height, uint32_t seed) {
    BgraImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    Random random(seed);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Gradient plus noise: every blur radius and block size changes it.
            const uint32_t noise = random.Next() & 63u;
            const uint32_t red = (static_cast<uint32_t>(x) * 255u / static_cast<uint32_t>(width) + noise) & 255u;
            const uint32_t green = (static_cast<uint32_t>(y) * 255u / static_cast<uint32_t>(height) + noise) & 255u;
            const uint32_t blue = (static_cast<uint32_t>(x + y) * 3u + noise) & 255u;
            image.row(y)[x] = 0xff000000u | (red << 16) | (green << 8) | blue;
        }
    }
    return image;
}

bool Inside(const RECT& rect, int x, int y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

bool SamePixels(const BgraImage& a, const BgraImage& b) {
    return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}

/// True when `before` and `after` agree everywhere outside `changed`.
bool UnchangedOutside(const BgraImage& before, const BgraImage& after, const RECT& changed) {
    for (int y = 0; y < after.height; ++y) {
        for (int x = 0; x < after.width; ++x) {
            if (!Inside(changed, x, y) && before.row(y)[x] != after.row(y)[x]) {
                return false;
            }
        }
    }
    return true;
}

RedactionEffect Region(RedactionKind kind, float left, float top, float right, float bottom, float strength) {
    RedactionEffect effect;
    effect.kind = kind;
    effect.box = D2D1::RectF(left, top, right, bottom);
    effect.strength = strength;
    return effect;
}

RedactionEffect Spotlight(RedactionOutline outline, float left, float top, float right, float bottom, float dim) {
    RedactionEffect effect;
    effect.kind = RedactionKind::Spotlight;
    effect.outline = outline;
    effect.box = D2D1::RectF(left, top, right, bottom);
    effect.corner_radius = 16.0f;
    effect.dim = dim;
    return effect;
}

RedactionEffect Brush(RedactionKind kind, std::vector<D2D1_POINT_2F> path, float radius, float strength) {
    RedactionEffect effect;
    effect.kind = kind;
    effect.brush = true;
    effect.brush_radius = radius;
    effect.strength = strength;
    float left = path.front().x, top = path.front().y, right = left, bottom = top;
    for (const auto& point : path) {
        left = (std::min)(left, point.x);
        top = (std::min)(top, point.y);
        right = (std::max)(right, point.x);
        bottom = (std::max)(bottom, point.y);
    }
    effect.box = D2D1::RectF(left - radius, top - radius, right + radius, bottom + radius);
    effect.brush_path = std::move(path);
    return effect;
}

RedactionEffect RandomEffect(Random& random, int width, int height) {
    const float left = random.Real(-20.0f, static_cast<float>(width) - 10.0f);
    const float top = random.Real(-20.0f, static_cast<float>(height) - 10.0f);
    const float right = left + random.Real(3.0f, 90.0f);
    const float bottom = top + random.Real(3.0f, 70.0f);
    const float strength = random.Real(0.0f, 100.0f);
    switch (random.Range(0, 4)) {
        case 0: return Region(RedactionKind::Blur, left, top, right, bottom, strength);
        case 1: return Region(RedactionKind::Pixelate, left, top, right, bottom, strength);
        case 2: {
            std::vector<D2D1_POINT_2F> path;
            float x = left, y = top;
            for (int i = random.Range(1, 6); i > 0; --i) {
                path.push_back(D2D1::Point2F(x, y));
                x += random.Real(-25.0f, 40.0f);
                y += random.Real(-25.0f, 40.0f);
            }
            return Brush(random.Range(0, 1) ? RedactionKind::Blur : RedactionKind::Pixelate, std::move(path),
                         random.Real(4.0f, 24.0f), strength);
        }
        default: {
            // Few distinct dim values, so spotlight edits usually keep the shared dim.
            const float dim = 0.25f * static_cast<float>(random.Range(1, 3));
            return Spotlight(static_cast<RedactionOutline>(random.Range(0, 2)), left, top, right, bottom, dim);
        }
    }
}

/// Applies one edit and checks the incremental image against a rebuild.
void Step(RedactionRenderer& renderer, const BgraImage& source, const std::vector<RedactionEffect>& effects,
          const char* what, uint32_t seed, int step) {
    const BgraImage before = renderer.image();
    const RECT changed = renderer.Update(source, effects);
    RedactionRenderer reference;
    reference.Update(source, effects);
    const bool equal = SamePixels(renderer.image(), reference.image());
    const bool local = before.empty() || UnchangedOutside(before, renderer.image(), changed);
    if (!equal || !local) {
        std::fprintf(stderr, "seed %u step %d (%s): %s\n", seed, step, what,
                     !equal ? "differs from a rebuild" : "changed pixels outside the reported rectangle");
    }
    CHECK(equal);
    CHECK(local);
}

void TestRandomEditSequences() {
    const int width = 257, height = 193;  // Odd sizes exercise partial blocks.
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        const BgraImage source = MakeSource(width, height, seed);
        Random random(seed * 7919u);
        RedactionRenderer renderer;
        std::vector<RedactionEffect> effects;
        Step(renderer, source, effects, "empty", seed, 0);
        for (int step = 1; step <= 60; ++step) {
            const int action = effects.empty() ? 0 : random.Range(0, 5);
            const size_t index = effects.empty() ? 0 : static_cast<size_t>(random.Range(0, static_cast<int>(effects.size()) - 1));
            const char* what = "add";
            if (action <= 1 && effects.size() < 9) {
                effects.push_back(RandomEffect(random, width, height));
            } else if (action == 2) {
                what = "remove";
                effects.erase(effects.begin() + static_cast<ptrdiff_t>(index));
            } else if (action == 3) {
                what = "move";
                const float dx = random.Real(-30.0f, 30.0f), dy = random.Real(-30.0f, 30.0f);
                auto& effect = effects[index];
                effect.box.left += dx; effect.box.right += dx; effect.box.top += dy; effect.box.bottom += dy;
                for (auto& point : effect.brush_path) { point.x += dx; point.y += dy; }
            } else if (action == 4) {
                what = "restyle";
                effects[index].strength = random.Real(0.0f, 100.0f);
                effects[index].dim = 0.25f * static_cast<float>(random.Range(1, 3));
            } else {
                what = "replace";
                effects[index] = RandomEffect(random, width, height);
            }
            Step(renderer, source, effects, what, seed, step);
        }
    }
}

void TestDraggingOneEffectStaysLocal() {
    const int width = 1200, height = 800;
    const BgraImage source = MakeSource(width, height, 99);
    RedactionRenderer renderer;
    // Two committed effects far from the drag must not be recomputed or reported.
    std::vector<RedactionEffect> effects{Region(RedactionKind::Pixelate, 40, 40, 200, 160, 60),
                                         Region(RedactionKind::Blur, 900, 600, 1100, 760, 80)};
    const RECT first = renderer.Update(source, effects);
    CHECK(first.left == 0 && first.top == 0 && first.right == width && first.bottom == height);

    effects.push_back(Region(RedactionKind::Blur, 500, 300, 520, 320, 50));
    for (int step = 0; step < 40; ++step) {
        effects.back().box.right = 520.0f + static_cast<float>(step) * 2.0f;
        effects.back().box.bottom = 320.0f + static_cast<float>(step);
        const BgraImage before = renderer.image();
        const RECT changed = renderer.Update(source, effects);
        CHECK(!::IsRectEmpty(&changed));
        // The preview and its blur reach stay well inside the middle of the image.
        CHECK(changed.left >= 400 && changed.top >= 200 && changed.right <= 720 && changed.bottom <= 480);
        CHECK(UnchangedOutside(before, renderer.image(), changed));
    }
    RedactionRenderer reference;
    reference.Update(source, effects);
    CHECK(SamePixels(renderer.image(), reference.image()));

    const RECT repeated = renderer.Update(source, effects);
    CHECK(::IsRectEmpty(&repeated));
}

void TestOverlappingEffectsAreReappliedInOrder() {
    const int width = 400, height = 300;
    const BgraImage source = MakeSource(width, height, 5);
    RedactionRenderer renderer;
    // The blur reads pixels the pixelation wrote, and the brush reads both.
    std::vector<RedactionEffect> effects{
        Region(RedactionKind::Pixelate, 100, 100, 220, 200, 70),
        Region(RedactionKind::Blur, 180, 150, 300, 240, 90),
        Brush(RedactionKind::Blur, {D2D1::Point2F(150, 120), D2D1::Point2F(260, 210)}, 14.0f, 40)};
    renderer.Update(source, effects);
    effects.front().box.left = 90.0f;  // Moving the first one changes what the others read.
    Step(renderer, source, effects, "move first of an overlapping chain", 5, 1);
    effects.erase(effects.begin());
    Step(renderer, source, effects, "remove first of an overlapping chain", 5, 2);
}

void TestSpotlightDimIsGlobal() {
    const int width = 300, height = 200;
    const BgraImage source = MakeSource(width, height, 11);
    RedactionRenderer renderer;
    std::vector<RedactionEffect> effects;
    renderer.Update(source, effects);
    CHECK(SamePixels(renderer.image(), source));

    // The first spotlight darkens everything outside it.
    effects.push_back(Spotlight(RedactionOutline::Ellipse, 100, 60, 200, 140, 0.5f));
    RECT changed = renderer.Update(source, effects);
    CHECK(changed.left == 0 && changed.top == 0 && changed.right == width && changed.bottom == height);
    CHECK(renderer.image().row(5)[5] != source.row(5)[5]);
    CHECK(renderer.image().row(100)[150] == source.row(100)[150]);

    // Resizing it with the same dim only touches the old and new extent.
    effects.back().box.right = 230.0f;
    Step(renderer, source, effects, "resize spotlight", 11, 1);
    changed = renderer.Update(source, effects);
    CHECK(::IsRectEmpty(&changed));

    // Removing the last spotlight restores the capture everywhere.
    effects.clear();
    changed = renderer.Update(source, effects);
    CHECK(changed.left == 0 && changed.top == 0 && changed.right == width && changed.bottom == height);
    CHECK(SamePixels(renderer.image(), source));
}

void TestResetAndNewSourceRebuild() {
    const BgraImage first_capture = MakeSource(64, 48, 3);
    const BgraImage second_capture = MakeSource(96, 80, 4);
    const std::vector<RedactionEffect> effects{Region(RedactionKind::Pixelate, 8, 8, 40, 30, 50)};
    RedactionRenderer renderer;
    renderer.Update(first_capture, effects);
    // A different capture size cannot reuse the previous pixels.
    const RECT changed = renderer.Update(second_capture, effects);
    CHECK(changed.right == 96 && changed.bottom == 80);
    RedactionRenderer reference;
    reference.Update(second_capture, effects);
    CHECK(SamePixels(renderer.image(), reference.image()));

    renderer.Reset();
    CHECK(renderer.image().empty());
    const RECT rebuilt = renderer.Update(second_capture, effects);
    CHECK(rebuilt.right == 96 && rebuilt.bottom == 80);
    CHECK(SamePixels(renderer.image(), reference.image()));
}

}  // namespace

int main() {
    TestRandomEditSequences();
    TestDraggingOneEffectStaysLocal();
    TestOverlappingEffectsAreReappliedInOrder();
    TestSpotlightDimIsGlobal();
    TestResetAndNewSourceRebuild();
    std::printf("redaction renderer: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
