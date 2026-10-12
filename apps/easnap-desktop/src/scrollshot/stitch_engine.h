// Easnap - scrolling-screenshot stitching engine.
//
// Pure pixel logic. This module has no window, thread, clock or capture
// dependency, so the unit tests compile it on its own and every result is a
// function of the submitted frames only.
//
// The caller submits successive pictures of one fixed screen rectangle while
// its content scrolls vertically. The engine places each picture on a growing
// canvas, or says explicitly that it could not. It never reports a placement
// it did not verify. Method and thresholds: docs/decisions/ADR-027.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace easnap::scrollshot {

/// A top-down 32-bit BGRA picture. Alpha is ignored.
struct FrameView {
    const uint32_t* pixels{nullptr};
    int width{0};
    int height{0};
    /// Distance between the starts of two rows, in pixels (at least `width`).
    std::ptrdiff_t stride{0};
};

/// The canvas stops growing at whichever bound is reached first.
struct StitchLimits {
    int max_height{32000};
    int64_t max_pixels{48'000'000};
};

enum class FrameOutcome : uint8_t {
    First,            ///< First frame: it defines the width and starts the canvas.
    Unchanged,        ///< Same picture as the previous frame; nothing was done.
    Refreshed,        ///< Placed where the previous frame was; visible rows were updated.
    Extended,         ///< Placed; new rows were appended.
    Moved,            ///< Placed inside rows that already exist; nothing was appended.
    OutsideCaptured,  ///< Placed above the first captured row; nothing was written.
    LimitReached,     ///< Placed, but rows beyond the size limit were dropped.
    Ambiguous,        ///< Not placed: several offsets fit equally well.
    LowInformation,   ///< Not placed: the overlap has too little detail to measure.
    NoOverlap,        ///< Not placed: no offset fits (scrolled too far, or other content).
    Invalid,          ///< Rejected: empty, or a different size than the first frame.
};

struct FrameResult {
    FrameOutcome outcome{FrameOutcome::Invalid};
    /// True when the position of this frame on the canvas is known.
    bool placed{false};
    /// Rows moved since the previously placed frame; positive is scrolling down.
    int displacement{0};
    /// Canvas row of the frame's first row. Meaningful only when `placed`.
    int position{0};
    int appended_rows{0};
    /// Canvas height after this frame.
    int height{0};
    bool at_limit{false};
    /// The offset was one of several equal fits and was chosen because it
    /// continued the motion of the previous frame.
    bool by_continuity{false};
};

/// What the engine currently believes about the fixed parts of the picture.
struct StitchLayout {
    int header_rows{0};
    int footer_rows{0};
    int left_rail{0};
    int right_rail{0};
};

class StitchEngine {
public:
    explicit StitchEngine(StitchLimits limits = {});
    ~StitchEngine();

    StitchEngine(const StitchEngine&) = delete;
    StitchEngine& operator=(const StitchEngine&) = delete;

    FrameResult Submit(const FrameView& frame);
    /// Forgets everything, including the frame size.
    void Reset();

    [[nodiscard]] int width() const noexcept;
    /// Rows written so far, including the provisional rows of the lowest frame.
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] int frame_height() const noexcept;
    /// Largest height this canvas may reach for its width.
    [[nodiscard]] int max_height() const noexcept;
    [[nodiscard]] bool at_limit() const noexcept;
    [[nodiscard]] StitchLayout layout() const noexcept;

    /// `width()` pixels of canvas row `y`, or nullptr outside the canvas. The
    /// pointer stays valid until the next Submit or Reset.
    [[nodiscard]] const uint32_t* Row(int y) const noexcept;
    /// Copies rows [first, first + count) to `out`, `stride` pixels apart.
    bool CopyRows(int first, int count, uint32_t* out, std::ptrdiff_t stride) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] const wchar_t* ToString(FrameOutcome outcome) noexcept;

}  // namespace easnap::scrollshot
