// Easnap - derived redaction pixels for the screenshot editor.
//
// Blur, pixelation and the spotlight dim layer are not vector annotations:
// they replace captured pixels. This renderer owns that derived image and
// keeps it equal to "every effect applied in order to the immutable capture".
// An edit recomputes only the rectangle the edit can influence, so previewing
// or moving one effect does not copy and reprocess the whole capture.
#pragma once

#include <cstdint>
#include <vector>

#include "platform/platform_utils.h"

namespace easnap::editor {

enum class RedactionKind { Blur, Pixelate, Spotlight };
enum class RedactionOutline { Rectangle, Ellipse, Rounded };

/// One pixel effect in source-image coordinates.
struct RedactionEffect {
    RedactionKind kind{RedactionKind::Blur};
    /// Extent of the effect. A brush effect's box already includes the brush radius.
    D2D1_RECT_F box{};
    /// Blur/Pixelate only: cover the dabs along `brush_path` instead of the whole box.
    bool brush{false};
    std::vector<D2D1_POINT_2F> brush_path;
    float brush_radius{0.0f};
    /// Blur/Pixelate strength, 0..100.
    float strength{50.0f};
    /// Spotlight only.
    RedactionOutline outline{RedactionOutline::Rectangle};
    float corner_radius{0.0f};
    /// Spotlight only: how far everything outside the spotlights is darkened, 0..1.
    float dim{0.0f};

    [[nodiscard]] bool operator==(const RedactionEffect& other) const noexcept;
};

class RedactionRenderer {
public:
    /// Forgets the derived image; the next Update rebuilds it completely.
    void Reset();

    /// Brings image() up to date with `effects` applied in order to `source`.
    /// `source` must be the same pixels on every call between two Resets.
    /// Returns the source rectangle that may differ from the previous image();
    /// it is empty when nothing changed.
    RECT Update(const platform::BgraImage& source, std::vector<RedactionEffect> effects);

    [[nodiscard]] const platform::BgraImage& image() const noexcept { return image_; }

private:
    platform::BgraImage image_;
    std::vector<RedactionEffect> effects_;
    std::vector<uint8_t> mask_;
    std::vector<float> spots_;
    bool valid_{false};
};

}  // namespace easnap::editor
