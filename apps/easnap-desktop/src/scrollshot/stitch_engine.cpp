// Easnap - scrolling-screenshot stitching engine implementation.
//
// Each row of a frame is reduced to per-segment descriptors (mean luminance and
// horizontal gradient energy). A frame is located by comparing its descriptors
// with those of the canvas rows it would cover, counting only segments that
// carry vertical detail. Pixels are written only after that comparison has
// accepted one offset over every rival. See docs/decisions/ADR-027.
#include "scrollshot/stitch_engine.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace easnap::scrollshot {
namespace {

// --- Tuned constants (ADR-027 lists the reasons and the test table) --------

/// Canvas rows are stored in strips, so growing never copies written pixels.
constexpr int kStripRows = 256;
constexpr int kMaxSegments = 32;
constexpr int kMinSegments = 4;
/// Narrowest segment worth describing, in pixels.
constexpr int kSegmentPixels = 24;
constexpr int kCoarseGroups = 4;
/// Descriptors keep four fractional bits of an 8-bit level.
constexpr int kFixedPoint = 16;
/// Two descriptors within 2.5 levels describe the same pixels. Capture noise
/// of a few levels per pixel averages far below this over a segment.
constexpr int kMatchTolerance = 40;
/// A segment carries vertical detail when it differs from the row above or
/// below by at least 3 levels. Only such segments can tell offsets apart.
constexpr int kInformativeStep = 48;
constexpr int kCoarseCap = 8 * kFixedPoint;
constexpr int kMinOverlapRows = 12;
/// Share of informative segments that must agree at the chosen offset. The
/// remainder absorbs hover highlights, carets and images that finish loading.
constexpr int kAcceptPercent = 55;
/// "Nothing moved" is accepted when this share of the rows with detail is
/// exactly as it was. A picture that finishes loading changes many rows at once
/// and leaves the others untouched; a jump to other content leaves none.
constexpr int kUnmovedCleanRowsPercent = 25;
/// Before any motion was seen the fixed bands are unknown, and a tall header
/// alone must not pass for an unmoved viewport.
constexpr int kUnmovedCleanRowsUnknownBandsPercent = 50;
/// Rows' worth of segments assumed to disagree when two shares are compared.
constexpr int kSharePriorRows = 4;
constexpr int kHistoryLength = 5;
constexpr int kMaxCandidates = 12;
/// Offsets this close together are one candidate; the best of them is used.
constexpr int kBasinRadius = 2;
/// Rows beside a fixed band that are written once and never refreshed.
constexpr int kEdgeMarginRows = 12;
constexpr int kRailSampleRows = 96;
constexpr int kRailChannelTolerance = 12;
/// Side rails are decided within this many moving frames, or not at all.
constexpr int kRailDecisionFrames = 8;
constexpr int kNoPosition = std::numeric_limits<int>::min();

enum RowState : uint8_t { kEmpty = 0, kContent = 1, kTail = 2 };

[[nodiscard]] inline int Luma(uint32_t pixel) noexcept {
    return static_cast<int>(((pixel & 0xFFu) * 29u + ((pixel >> 8) & 0xFFu) * 150u +
                             ((pixel >> 16) & 0xFFu) * 77u) >> 8);
}

[[nodiscard]] inline bool NearPixel(uint32_t a, uint32_t b) noexcept {
    for (int shift = 0; shift < 24; shift += 8) {
        const int delta = static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu);
        if (delta > kRailChannelTolerance || delta < -kRailChannelTolerance) { return false; }
    }
    return true;
}

[[nodiscard]] inline int Popcount(uint32_t value) noexcept {
    int count = 0;
    while (value != 0) { value &= value - 1; ++count; }
    return count;
}

/// Row descriptors of one frame, or of the canvas (which only ever grows).
struct Descriptors {
    int segments{0};
    std::vector<uint16_t> mean;    ///< rows * segments
    std::vector<uint16_t> energy;  ///< rows * segments
    std::vector<uint16_t> coarse;  ///< rows * kCoarseGroups
    std::vector<uint32_t> info;    ///< one bit per segment with vertical detail
    std::vector<uint64_t> hash;    ///< exact pixels of the row, rails excluded
    std::vector<uint8_t> distinct; ///< the row's pixels differ from a neighbour's

    void Resize(int rows, int segment_count) {
        segments = segment_count;
        const size_t count = static_cast<size_t>(rows);
        hash.resize(count);
        distinct.resize(count);
        mean.resize(count * static_cast<size_t>(segments));
        energy.resize(count * static_cast<size_t>(segments));
        coarse.resize(count * kCoarseGroups);
        info.resize(count);
    }
    [[nodiscard]] const uint16_t* Mean(int row) const noexcept { return mean.data() + static_cast<size_t>(row) * segments; }
    [[nodiscard]] const uint16_t* Energy(int row) const noexcept { return energy.data() + static_cast<size_t>(row) * segments; }
    [[nodiscard]] const uint16_t* Coarse(int row) const noexcept { return coarse.data() + static_cast<size_t>(row) * kCoarseGroups; }
    [[nodiscard]] uint16_t* Mean(int row) noexcept { return mean.data() + static_cast<size_t>(row) * segments; }
    [[nodiscard]] uint16_t* Energy(int row) noexcept { return energy.data() + static_cast<size_t>(row) * segments; }
    [[nodiscard]] uint16_t* Coarse(int row) noexcept { return coarse.data() + static_cast<size_t>(row) * kCoarseGroups; }
};

/// Agreement of a frame with the canvas at one position.
struct Score {
    int position{kNoPosition};
    int rows{0};        ///< Compared rows.
    int info{0};        ///< Informative segments compared.
    int good{0};        ///< Informative segments that agree.
    int info_rows{0};   ///< Rows with at least one informative segment.
    int clean_rows{0};  ///< Such rows in which every informative segment agrees.
};

/// True when `a` agrees on a larger share of its informative segments. `prior`
/// segments are added to both totals, so a perfect fit on a sliver of overlap
/// does not outrank a near-perfect fit on a large one.
[[nodiscard]] bool BetterShare(const Score& a, const Score& b, int prior) noexcept {
    const int64_t left = static_cast<int64_t>(a.good) * (b.info + prior);
    const int64_t right = static_cast<int64_t>(b.good) * (a.info + prior);
    if (left != right) { return left > right; }
    return a.info > b.info;
}

[[nodiscard]] int UpperMedian(std::array<int, kHistoryLength> values, int count) noexcept {
    if (count <= 0) { return 0; }
    std::sort(values.begin(), values.begin() + count);
    return values[static_cast<size_t>(count / 2)];
}

}  // namespace

struct StitchEngine::Impl {
    explicit Impl(StitchLimits limits_in) : limits(limits_in) {}

    StitchLimits limits;
    int width{0};
    int frame_height{0};
    int segments{0};
    int max_rows{0};
    std::array<int, kMaxSegments + 1> segment_begin{};
    uint32_t active_mask{0};  ///< Segments that take part in comparisons.
    int active_segments{0};

    // Canvas.
    int rows{0};
    std::vector<std::unique_ptr<uint32_t[]>> strips;
    std::vector<uint8_t> state;
    Descriptors canvas;

    // Tracking.
    Descriptors previous;      ///< The frame submitted before this one.
    Descriptors current;
    bool previous_valid{false};
    bool tracking{false};      ///< The previous submitted frame was placed.
    int position{0};           ///< Canvas row of the last placed frame.
    int last_displacement{0};
    FrameResult last_result;
    std::array<int, kHistoryLength> top_history{};
    std::array<int, kHistoryLength> bottom_history{};
    int history_count{0};
    int history_next{0};
    bool at_limit{false};

    // Side rails.
    std::vector<uint32_t> first_frame;
    std::vector<uint32_t> rail_fill;
    int rail_left{0};
    int rail_right{0};
    int pending_left{-1};
    int pending_right{-1};
    int moving_frames{0};
    bool rails_decided{false};

    [[nodiscard]] uint32_t* RowPixels(int row) noexcept {
        return strips[static_cast<size_t>(row / kStripRows)].get() + static_cast<size_t>(row % kStripRows) * width;
    }
    [[nodiscard]] const uint32_t* RowPixels(int row) const noexcept {
        return strips[static_cast<size_t>(row / kStripRows)].get() + static_cast<size_t>(row % kStripRows) * width;
    }

    void Start(const FrameView& frame) {
        width = frame.width;
        frame_height = frame.height;
        segments = std::clamp(width / kSegmentPixels, kMinSegments, kMaxSegments);
        segments = std::min(segments, width);
        for (int k = 0; k <= segments; ++k) {
            segment_begin[static_cast<size_t>(k)] = static_cast<int>(static_cast<int64_t>(width) * k / segments);
        }
        active_mask = segments >= 32 ? 0xFFFFFFFFu : ((1u << segments) - 1u);
        active_segments = segments;
        const int64_t by_pixels = limits.max_pixels / std::max(1, width);
        max_rows = static_cast<int>(std::clamp<int64_t>(std::min<int64_t>(limits.max_height, by_pixels),
                                                        frame_height, std::numeric_limits<int>::max() / 2));
        canvas.Resize(0, segments);
    }

    /// Identical rows hash identically; alpha and the rail columns do not count.
    [[nodiscard]] uint64_t HashRow(const uint32_t* row) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        for (int x = rail_left; x < width - rail_right; ++x) {
            hash = (hash ^ (row[x] & 0x00FFFFFFu)) * 1099511628211ull;
        }
        return hash;
    }

    void Describe(const FrameView& frame, Descriptors& out) const {
        out.Resize(frame_height, segments);
        for (int y = 0; y < frame_height; ++y) {
            const uint32_t* row = frame.pixels + static_cast<std::ptrdiff_t>(y) * frame.stride;
            out.hash[static_cast<size_t>(y)] = HashRow(row);
            uint16_t* mean = out.Mean(y);
            uint16_t* energy = out.Energy(y);
            for (int k = 0; k < segments; ++k) {
                const int begin = segment_begin[static_cast<size_t>(k)];
                const int end = segment_begin[static_cast<size_t>(k) + 1];
                int previous_luma = Luma(row[begin]);
                uint32_t sum = static_cast<uint32_t>(previous_luma);
                uint32_t gradient = 0;
                for (int x = begin + 1; x < end; ++x) {
                    const int luma = Luma(row[x]);
                    sum += static_cast<uint32_t>(luma);
                    gradient += static_cast<uint32_t>(std::abs(luma - previous_luma));
                    previous_luma = luma;
                }
                const uint32_t count = static_cast<uint32_t>(end - begin);
                mean[k] = static_cast<uint16_t>(sum * kFixedPoint / count);
                energy[k] = static_cast<uint16_t>(count > 1 ? gradient * kFixedPoint / (count - 1) : 0);
            }
            uint16_t* coarse = out.Coarse(y);
            for (int group = 0; group < kCoarseGroups; ++group) {
                const int first = group * segments / kCoarseGroups;
                const int last = std::max(first + 1, (group + 1) * segments / kCoarseGroups);
                uint32_t total = 0;
                for (int k = first; k < last; ++k) { total += mean[k]; }
                coarse[group] = static_cast<uint16_t>(total / static_cast<uint32_t>(last - first));
            }
        }
        for (int y = 0; y < frame_height; ++y) {
            uint32_t mask = 0;
            for (int neighbour : {y - 1, y + 1}) {
                if (neighbour < 0 || neighbour >= frame_height) { continue; }
                const uint16_t* mean_a = out.Mean(y);
                const uint16_t* mean_b = out.Mean(neighbour);
                const uint16_t* energy_a = out.Energy(y);
                const uint16_t* energy_b = out.Energy(neighbour);
                for (int k = 0; k < segments; ++k) {
                    if (std::abs(mean_a[k] - mean_b[k]) >= kInformativeStep ||
                        std::abs(energy_a[k] - energy_b[k]) >= kInformativeStep) {
                        mask |= 1u << k;
                    }
                }
            }
            out.info[static_cast<size_t>(y)] = mask;
            const uint64_t hash = out.hash[static_cast<size_t>(y)];
            out.distinct[static_cast<size_t>(y)] =
                (y > 0 && out.hash[static_cast<size_t>(y) - 1] != hash) ||
                (y + 1 < frame_height && out.hash[static_cast<size_t>(y) + 1] != hash) ? 1 : 0;
        }
    }

    /// Segments of row `y` that do not agree with the previous frame's row `y`.
    [[nodiscard]] int ChangedSegments(int y) const noexcept {
        const uint16_t* mean_a = current.Mean(y);
        const uint16_t* mean_b = previous.Mean(y);
        const uint16_t* energy_a = current.Energy(y);
        const uint16_t* energy_b = previous.Energy(y);
        int changed = 0;
        for (int k = 0; k < segments; ++k) {
            if ((active_mask & (1u << k)) == 0) { continue; }
            if (std::abs(mean_a[k] - mean_b[k]) > kMatchTolerance ||
                std::abs(energy_a[k] - energy_b[k]) > kMatchTolerance) {
                ++changed;
            }
        }
        return changed;
    }

    [[nodiscard]] bool SameAsPrevious() const noexcept {
        for (int y = 0; y < frame_height; ++y) {
            if (ChangedSegments(y) != 0) { return false; }
        }
        return true;
    }

    /// Rows at one edge that kept their place while the picture changed. Any
    /// differing segment ends the run: sparse content (short lines in a wide
    /// frame) changes only a few segments per row and still scrolls.
    [[nodiscard]] int StaticRun(bool from_top) const noexcept {
        int run = 0;
        for (int i = 0; i < frame_height; ++i) {
            const int y = from_top ? i : frame_height - 1 - i;
            if (ChangedSegments(y) != 0) { break; }
            ++run;
        }
        return run;
    }

    /// Canvas rows [skip_begin, skip_end) are not used as reference.
    [[nodiscard]] Score ScoreAt(int candidate, int first_row, int end_row, int skip_begin = 0,
                                int skip_end = 0) const noexcept {
        Score score;
        score.position = candidate;
        for (int y = std::max(first_row, -candidate); y < end_row; ++y) {
            const int row = candidate + y;
            if (row >= rows) { break; }
            if (state[static_cast<size_t>(row)] != kContent || (row >= skip_begin && row < skip_end)) { continue; }
            ++score.rows;
            if (current.hash[static_cast<size_t>(y)] == canvas.hash[static_cast<size_t>(row)]) {
                // The same pixels. That is evidence only where the row differs
                // from its neighbours; a flat area fits at any offset.
                if (current.distinct[static_cast<size_t>(y)] != 0 || canvas.distinct[static_cast<size_t>(row)] != 0) {
                    score.info += active_segments;
                    score.good += active_segments;
                    ++score.info_rows;
                    ++score.clean_rows;
                }
                continue;
            }
            uint32_t mask = (current.info[static_cast<size_t>(y)] | canvas.info[static_cast<size_t>(row)]) & active_mask;
            if (mask == 0) { continue; }
            const uint16_t* mean_a = current.Mean(y);
            const uint16_t* mean_b = canvas.Mean(row);
            const uint16_t* energy_a = current.Energy(y);
            const uint16_t* energy_b = canvas.Energy(row);
            bool clean = true;
            for (int k = 0; mask != 0; ++k, mask >>= 1) {
                if ((mask & 1u) == 0) { continue; }
                ++score.info;
                if (std::abs(mean_a[k] - mean_b[k]) <= kMatchTolerance &&
                    std::abs(energy_a[k] - energy_b[k]) <= kMatchTolerance) {
                    ++score.good;
                } else {
                    clean = false;
                }
            }
            ++score.info_rows;
            if (clean) { ++score.clean_rows; }
        }
        return score;
    }

    /// Cheap pass over every position in [low, high]: the positions where a
    /// four-value-per-row comparison is locally best, cheapest first.
    void CoarseCandidates(int low, int high, int first_row, int end_row, int skip_begin, int skip_end,
                          std::vector<int>& out) const {
        out.clear();
        if (high < low || end_row <= first_row) { return; }
        const int step = std::max(1, (end_row - first_row) / 256);
        const int needed = std::max(3, (kMinOverlapRows + step - 1) / step);
        const size_t count = static_cast<size_t>(high - low + 1);
        std::vector<uint32_t> cost(count, std::numeric_limits<uint32_t>::max());
        for (int candidate = low; candidate <= high; ++candidate) {
            uint64_t sum = 0;
            int compared = 0;
            for (int y = first_row; y < end_row; y += step) {
                const int row = candidate + y;
                if (row < 0) { continue; }
                if (row >= rows) { break; }
                if (state[static_cast<size_t>(row)] != kContent || (row >= skip_begin && row < skip_end)) { continue; }
                const uint16_t* a = current.Coarse(y);
                const uint16_t* b = canvas.Coarse(row);
                for (int group = 0; group < kCoarseGroups; ++group) {
                    sum += static_cast<uint64_t>(std::min(std::abs(a[group] - b[group]), kCoarseCap));
                }
                ++compared;
            }
            if (compared >= needed) {
                cost[static_cast<size_t>(candidate - low)] =
                    static_cast<uint32_t>(sum * 4096 / (static_cast<uint64_t>(compared) * kCoarseGroups * kCoarseCap));
            }
        }
        std::vector<int> minima;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t value = cost[i];
            if (value == std::numeric_limits<uint32_t>::max()) { continue; }
            if ((i > 0 && cost[i - 1] < value) || (i + 1 < count && cost[i + 1] < value)) { continue; }
            minima.push_back(low + static_cast<int>(i));
        }
        std::sort(minima.begin(), minima.end(), [&](int a, int b) {
            const uint32_t cost_a = cost[static_cast<size_t>(a - low)];
            const uint32_t cost_b = cost[static_cast<size_t>(b - low)];
            if (cost_a != cost_b) { return cost_a < cost_b; }
            const int near_a = std::abs(a - position);
            const int near_b = std::abs(b - position);
            return near_a != near_b ? near_a < near_b : a < b;
        });
        for (int candidate : minima) {
            const bool close = std::any_of(out.begin(), out.end(), [candidate](int chosen) {
                return std::abs(chosen - candidate) <= kBasinRadius + 1;
            });
            if (close) { continue; }
            out.push_back(candidate);
            if (static_cast<int>(out.size()) >= kMaxCandidates) { break; }
        }
    }

    bool EnsureRows(int count) {
        if (count <= rows) { return true; }
        const size_t needed_strips = static_cast<size_t>((count + kStripRows - 1) / kStripRows);
        while (strips.size() < needed_strips) {
            strips.emplace_back(new uint32_t[static_cast<size_t>(kStripRows) * width]());
        }
        state.resize(static_cast<size_t>(count), kEmpty);
        canvas.Resize(count, segments);
        rows = count;
        return true;
    }

    /// Copies frame rows [first_row, end_row) to the canvas at `candidate`.
    /// With `only_new`, rows that already hold content are left as they are.
    void WriteRows(const FrameView& frame, int candidate, int first_row, int end_row, RowState row_state,
                   bool only_new = false) {
        const bool rails = rail_left > 0 || rail_right > 0;
        for (int y = first_row; y < end_row; ++y) {
            const int row = candidate + y;
            if (row < 0) { continue; }
            if (row >= max_rows) { at_limit = true; break; }
            const bool is_new = row >= rows || state[static_cast<size_t>(row)] == kEmpty;
            if (only_new && !is_new && state[static_cast<size_t>(row)] == kContent) { continue; }
            EnsureRows(row + 1);
            const uint32_t* source = frame.pixels + static_cast<std::ptrdiff_t>(y) * frame.stride;
            uint32_t* target = RowPixels(row);
            if (!rails) {
                std::memcpy(target, source, static_cast<size_t>(width) * sizeof(uint32_t));
            } else {
                // A rail does not scroll: its pixels belong to the first
                // viewport, and rows below continue it with a flat fill.
                std::memcpy(target + rail_left, source + rail_left,
                            static_cast<size_t>(width - rail_left - rail_right) * sizeof(uint32_t));
                if (is_new) { PaintRails(row); }
            }
            std::memcpy(canvas.Mean(row), current.Mean(y), static_cast<size_t>(segments) * sizeof(uint16_t));
            std::memcpy(canvas.Energy(row), current.Energy(y), static_cast<size_t>(segments) * sizeof(uint16_t));
            std::memcpy(canvas.Coarse(row), current.Coarse(y), kCoarseGroups * sizeof(uint16_t));
            canvas.info[static_cast<size_t>(row)] = current.info[static_cast<size_t>(y)];
            canvas.hash[static_cast<size_t>(row)] = current.hash[static_cast<size_t>(y)];
            canvas.distinct[static_cast<size_t>(row)] = current.distinct[static_cast<size_t>(y)];
            state[static_cast<size_t>(row)] = row_state;
        }
    }

    /// Gives the rail columns of one canvas row their final pixels.
    void PaintRails(int row) noexcept {
        uint32_t* target = RowPixels(row);
        const uint32_t* source = row < frame_height ? first_frame.data() + static_cast<size_t>(row) * width
                                                    : rail_fill.data();
        std::memcpy(target, source, static_cast<size_t>(rail_left) * sizeof(uint32_t));
        std::memcpy(target + width - rail_right, source + width - rail_right,
                    static_cast<size_t>(rail_right) * sizeof(uint32_t));
    }

    /// Looks for columns at the left and right edges that stayed in place while
    /// the rest scrolled from `position` to `candidate`. Uses the canvas as it
    /// was before this frame is written.
    void ObserveRails(const FrameView& frame, int candidate, int first_row, int end_row) {
        std::vector<int> sample;
        for (int y = first_row; y < end_row; ++y) {
            const int still = position + y;
            const int moved = candidate + y;
            if (still < 0 || moved < 0 || still >= rows || moved >= rows) { continue; }
            if (state[static_cast<size_t>(still)] != kContent || state[static_cast<size_t>(moved)] != kContent) { continue; }
            sample.push_back(y);
        }
        if (static_cast<int>(sample.size()) < kMinOverlapRows) { return; }
        const size_t stride = std::max<size_t>(1, sample.size() / kRailSampleRows);
        std::vector<int> used;
        for (size_t i = 0; i < sample.size(); i += stride) { used.push_back(sample[i]); }
        // Per sampled pixel: 1 stayed in place, 2 followed the scroll, 0 says nothing.
        std::vector<uint8_t> vote(used.size() * static_cast<size_t>(width), 0);
        std::vector<uint16_t> fixed(static_cast<size_t>(width), 0);
        std::vector<uint16_t> scrolled(static_cast<size_t>(width), 0);
        for (size_t i = 0; i < used.size(); ++i) {
            const int y = used[i];
            const uint32_t* now = frame.pixels + static_cast<std::ptrdiff_t>(y) * frame.stride;
            const uint32_t* still = RowPixels(position + y);
            const uint32_t* moved = RowPixels(candidate + y);
            uint8_t* votes = vote.data() + i * static_cast<size_t>(width);
            for (int x = 0; x < width; ++x) {
                const bool same_place = NearPixel(now[x], still[x]);
                const bool followed = NearPixel(now[x], moved[x]);
                if (same_place && !followed) { votes[x] = 1; ++fixed[static_cast<size_t>(x)]; }
                else if (followed && !same_place) { votes[x] = 2; ++scrolled[static_cast<size_t>(x)]; }
            }
        }
        const auto measure = [&](bool from_left) {
            int last_evidence = -1;
            int evidence = 0;
            bool bounded = false;
            for (int i = 0; i < width; ++i) {
                const size_t x = static_cast<size_t>(from_left ? i : width - 1 - i);
                if (scrolled[x] >= 3 && scrolled[x] > fixed[x]) { bounded = true; break; }
                if (fixed[x] >= 3) { last_evidence = i; ++evidence; }
            }
            // A rail reaches as far as its own detail does; blank columns
            // between it and the content say nothing and stay content. Without
            // a scrolling column nothing separates a rail from content at all.
            const int extent = last_evidence + 1;
            if (!bounded || extent <= 0 || extent > width * 45 / 100 || evidence < std::max(4, extent / 8)) { return 0; }
            // It must stand still over much of the height. A highlight under a
            // resting pointer also keeps its place, but only on a few rows.
            size_t covered = 0;
            for (size_t i = 0; i < used.size(); ++i) {
                const uint8_t* votes = vote.data() + i * static_cast<size_t>(width);
                bool seen = false;
                for (int j = 0; j < extent && !seen; ++j) { seen = votes[from_left ? j : width - 1 - j] == 1; }
                if (seen) { ++covered; }
            }
            return covered * 4 >= used.size() ? extent : 0;
        };
        const int left = measure(true);
        const int right = measure(false);
        const bool agrees = pending_left >= 0 && std::abs(left - pending_left) <= 2 &&
                            std::abs(right - pending_right) <= 2;
        if (agrees && (left > 0 || right > 0)) {
            EstablishRails(std::min(left, pending_left), std::min(right, pending_right));
            // This frame was hashed with the rails still included.
            for (int y = 0; y < frame_height; ++y) {
                current.hash[static_cast<size_t>(y)] = HashRow(frame.pixels + static_cast<std::ptrdiff_t>(y) * frame.stride);
            }
            for (int y = 0; y < frame_height; ++y) {
                const uint64_t hash = current.hash[static_cast<size_t>(y)];
                current.distinct[static_cast<size_t>(y)] =
                    (y > 0 && current.hash[static_cast<size_t>(y) - 1] != hash) ||
                    (y + 1 < frame_height && current.hash[static_cast<size_t>(y) + 1] != hash) ? 1 : 0;
            }
            return;
        }
        pending_left = left;
        pending_right = right;
    }

    void EstablishRails(int left, int right) {
        rail_left = left;
        rail_right = right;
        rails_decided = true;
        // The fill is each column's median over the first viewport: the rail's
        // background, not whichever item happened to be on one row.
        rail_fill.assign(static_cast<size_t>(width), 0);
        std::vector<uint8_t> channel(static_cast<size_t>(frame_height));
        const auto fill_column = [&](int x) {
            uint32_t pixel = 0xFF000000u;
            for (int shift = 0; shift < 24; shift += 8) {
                for (int y = 0; y < frame_height; ++y) {
                    channel[static_cast<size_t>(y)] = static_cast<uint8_t>(
                        (first_frame[static_cast<size_t>(y) * width + static_cast<size_t>(x)] >> shift) & 0xFFu);
                }
                std::nth_element(channel.begin(), channel.begin() + frame_height / 2, channel.end());
                pixel |= static_cast<uint32_t>(channel[static_cast<size_t>(frame_height / 2)]) << shift;
            }
            rail_fill[static_cast<size_t>(x)] = pixel;
        };
        for (int x = 0; x < rail_left; ++x) { fill_column(x); }
        for (int x = width - rail_right; x < width; ++x) { fill_column(x); }
        for (int row = 0; row < rows; ++row) {
            if (state[static_cast<size_t>(row)] != kEmpty) { PaintRails(row); }
        }
        for (int k = 0; k < segments; ++k) {
            const int begin = segment_begin[static_cast<size_t>(k)];
            const int end = segment_begin[static_cast<size_t>(k) + 1];
            if (end <= rail_left || begin >= width - rail_right) { active_mask &= ~(1u << k); }
        }
        active_segments = Popcount(active_mask);
        // Row hashes now leave the rails out, on the canvas as in later frames.
        for (int row = 0; row < rows; ++row) { canvas.hash[static_cast<size_t>(row)] = HashRow(RowPixels(row)); }
        for (int row = 0; row < rows; ++row) {
            const uint64_t hash = canvas.hash[static_cast<size_t>(row)];
            canvas.distinct[static_cast<size_t>(row)] =
                (row > 0 && canvas.hash[static_cast<size_t>(row) - 1] != hash) ||
                (row + 1 < rows && canvas.hash[static_cast<size_t>(row) + 1] != hash) ? 1 : 0;
        }
    }

    [[nodiscard]] FrameResult Report(FrameOutcome outcome, bool placed, int displacement, int appended,
                                     bool by_continuity = false) {
        FrameResult result;
        result.outcome = outcome;
        result.placed = placed;
        result.displacement = displacement;
        result.position = position;
        result.appended_rows = appended;
        result.height = rows;
        result.at_limit = at_limit;
        result.by_continuity = by_continuity;
        last_result = result;
        return result;
    }

    FrameResult SubmitFirst(const FrameView& frame) {
        Start(frame);
        Describe(frame, current);
        first_frame.resize(static_cast<size_t>(width) * frame_height);
        for (int y = 0; y < frame_height; ++y) {
            std::memcpy(first_frame.data() + static_cast<size_t>(y) * width,
                        frame.pixels + static_cast<std::ptrdiff_t>(y) * frame.stride,
                        static_cast<size_t>(width) * sizeof(uint32_t));
        }
        position = 0;
        WriteRows(frame, 0, 0, frame_height, kContent);
        previous = current;
        previous_valid = true;
        tracking = true;
        last_displacement = 0;
        return Report(FrameOutcome::First, true, 0, rows);
    }

    FrameResult SubmitNext(const FrameView& frame) {
        Describe(frame, current);
        if (previous_valid && SameAsPrevious()) {
            FrameResult result = last_result;
            result.outcome = FrameOutcome::Unchanged;
            result.placed = tracking;
            result.displacement = 0;
            result.appended_rows = 0;
            result.by_continuity = false;
            // Motion stopped: continuity must not carry over a pause.
            last_displacement = 0;
            return result;
        }

        const int top_static = previous_valid ? StaticRun(true) : 0;
        const int bottom_static = previous_valid ? StaticRun(false) : 0;
        const int top_known = UpperMedian(top_history, history_count);
        const int bottom_known = UpperMedian(bottom_history, history_count);
        const int top_fixed = std::max(top_static, top_known);
        const int bottom_fixed = std::max(bottom_static, bottom_known);

        // A frame that moved is compared outside the rows that stayed fixed in
        // the viewport. After an unplaced frame that comparison says nothing
        // about where the viewport is, so only the known bands are excluded.
        const int moved_first = tracking ? top_fixed : top_known;
        const int moved_end = frame_height - (tracking ? bottom_fixed : bottom_known);
        const int same_first = top_known;
        const int same_end = frame_height - bottom_known;
        // What the previous frame showed in its bottom band may be a footer
        // rather than content, so those canvas rows are no reference for a
        // frame that moved.
        const int skip_begin = tracking ? position + frame_height - bottom_fixed : 0;
        const int skip_end = tracking ? position + frame_height : 0;

        const int info_needed = 3 * active_segments;
        const int prior = kSharePriorRows * active_segments;
        bool saw_overlap = false;
        const auto qualifies = [&](const Score& score) {
            if (score.rows >= kMinOverlapRows) { saw_overlap = true; }
            return score.rows >= kMinOverlapRows && score.info >= info_needed;
        };

        // --- The frame moved: one candidate per basin -------------------------
        std::vector<Score> scored;
        if (moved_end - moved_first >= kMinOverlapRows) {
            const int low = tracking ? position - frame_height : -frame_height;
            const int high = std::min(tracking ? position + frame_height : rows, rows - moved_first - 1);
            std::vector<int> candidates;
            CoarseCandidates(low, high, moved_first, moved_end, skip_begin, skip_end, candidates);
            if (tracking && last_displacement != 0) { candidates.push_back(position + last_displacement); }
            for (int candidate : candidates) {
                Score best;
                for (int offset = -kBasinRadius; offset <= kBasinRadius; ++offset) {
                    const int at = candidate + offset;
                    if (tracking && at == position) { continue; }  // That is the unmoved hypothesis.
                    const Score score = ScoreAt(at, moved_first, moved_end, skip_begin, skip_end);
                    if (!qualifies(score)) { continue; }
                    if (best.position == kNoPosition || BetterShare(score, best, prior)) { best = score; }
                }
                if (best.position == kNoPosition) { continue; }
                const bool duplicate = std::any_of(scored.begin(), scored.end(), [&](const Score& other) {
                    return std::abs(other.position - best.position) <= kBasinRadius;
                });
                if (!duplicate) { scored.push_back(best); }
            }
        }

        FrameOutcome failure = saw_overlap ? FrameOutcome::LowInformation : FrameOutcome::NoOverlap;
        Score moved;
        bool by_continuity = false;
        if (!scored.empty()) {
            size_t best_index = 0;
            for (size_t i = 1; i < scored.size(); ++i) {
                if (BetterShare(scored[i], scored[best_index], prior)) { best_index = i; }
            }
            // A rival ties unless the best offset explains clearly more detail:
            // at the rival's share it would agree on at least two rows' worth
            // of segments (and 2 % of its own) fewer than it does.
            std::vector<size_t> tied;
            for (size_t i = 0; i < scored.size(); ++i) {
                if (i == best_index) { continue; }
                const Score& best = scored[best_index];
                const Score& rival = scored[i];
                const int64_t best_total = best.info + prior;
                const int64_t rival_total = rival.info + prior;
                const int64_t lead = static_cast<int64_t>(best.good) * rival_total -
                                     static_cast<int64_t>(rival.good) * best_total;
                const int64_t evidence_needed = std::max<int64_t>(2 * active_segments, best_total / 50);
                if (lead < evidence_needed * rival_total) { tied.push_back(i); }
            }
            bool decided = tied.empty();
            if (!decided && tracking && last_displacement != 0) {
                // Equal fits: only the one that continues the previous motion
                // may be taken, and only while a scroll is in progress.
                tied.push_back(best_index);
                const int predicted = position + last_displacement;
                const int tolerance = std::max(6, std::abs(last_displacement) / 2);
                size_t nearest = tied.front();
                for (size_t index : tied) {
                    if (std::abs(scored[index].position - predicted) < std::abs(scored[nearest].position - predicted)) {
                        nearest = index;
                    }
                }
                decided = std::abs(scored[nearest].position - predicted) <= tolerance;
                for (size_t index : tied) {
                    if (index != nearest && std::abs(scored[index].position - predicted) <= 2 * tolerance) {
                        decided = false;
                    }
                }
                if (decided) {
                    best_index = nearest;
                    by_continuity = true;
                }
            }
            if (!decided) {
                failure = FrameOutcome::Ambiguous;
            } else if (static_cast<int64_t>(scored[best_index].good) * 100 >=
                       static_cast<int64_t>(kAcceptPercent) * scored[best_index].info) {
                moved = scored[best_index];
            } else {
                failure = FrameOutcome::NoOverlap;
                by_continuity = false;
            }
        }

        // --- The frame did not move ------------------------------------------
        // Judged by rows, not segments: a picture that finished loading changes
        // a large area at once, but leaves the rows around it exactly as they
        // were. A viewport that jumped elsewhere leaves no such rows.
        Score same;
        bool same_accepted = false;
        if (tracking) {
            same = ScoreAt(position, same_first, same_end);
            const int percent = history_count == 0 ? kUnmovedCleanRowsUnknownBandsPercent : kUnmovedCleanRowsPercent;
            same_accepted = qualifies(same) && same.clean_rows >= kMinOverlapRows &&
                            static_cast<int64_t>(same.clean_rows) * 100 >= static_cast<int64_t>(percent) * same.info_rows;
        }
        const bool use_same = same_accepted && (moved.position == kNoPosition || BetterShare(same, moved, prior));
        if (!use_same && moved.position == kNoPosition) {
            previous = current;
            previous_valid = true;
            tracking = false;
            last_displacement = 0;
            return Report(failure, false, 0, 0);
        }

        const int target = use_same ? position : moved.position;
        const int displacement = target - position;
        const bool has_moved = displacement != 0;
        const int rows_before = rows;
        if (has_moved && tracking) {
            top_history[static_cast<size_t>(history_next)] = top_static;
            bottom_history[static_cast<size_t>(history_next)] = bottom_static;
            history_next = (history_next + 1) % kHistoryLength;
            history_count = std::min(history_count + 1, kHistoryLength);
            if (!rails_decided && position >= 0 && target >= 0 && std::abs(displacement) >= 2) {
                ObserveRails(frame, target, moved_first, moved_end);
                if (!rails_decided && ++moving_frames >= kRailDecisionFrames) {
                    rails_decided = true;
                    first_frame = {};
                }
            }
            // The previous frame's bottom band turned out to be fixed. Where it
            // was the lowest frame, the canvas rows it wrote there may show a
            // footer: they stay in the picture but may be replaced.
            if (bottom_fixed > 0 && position >= 0 && position + frame_height >= rows) {
                for (int row = std::max(0, position + frame_height - bottom_fixed); row < rows; ++row) {
                    if (state[static_cast<size_t>(row)] == kContent) { state[static_cast<size_t>(row)] = kTail; }
                }
            }
        }

        const int write_first = use_same ? same_first : top_fixed;
        const int write_end = use_same ? same_end : frame_height - bottom_fixed;
        position = target;
        if (position >= 0 && write_end > write_first) {
            // Rows next to a fixed band can carry its shadow. After a move they
            // are written only where nothing is known yet, never to refresh
            // existing content, which would leave one faint line per frame.
            const int margin = has_moved ? std::min(kEdgeMarginRows, (write_end - write_first) / 4) : 0;
            WriteRows(frame, position, write_first, write_first + margin, kContent, true);
            WriteRows(frame, position, write_first + margin, write_end - margin, kContent);
            WriteRows(frame, position, write_end - margin, write_end, kContent, true);
        }
        // The rows under the bottom band belong to the picture only while this
        // is the lowest frame; a later, lower frame replaces them.
        if (position >= 0 && position + frame_height >= rows) {
            WriteRows(frame, position, std::max(write_first, write_end), frame_height, kTail);
        }
        if (position + frame_height > max_rows) { at_limit = true; }

        previous = current;
        previous_valid = true;
        tracking = true;
        last_displacement = displacement;
        const int appended = rows - rows_before;
        FrameOutcome outcome = FrameOutcome::Moved;
        if (position < 0) { outcome = FrameOutcome::OutsideCaptured; }
        else if (position + frame_height > max_rows) { outcome = FrameOutcome::LimitReached; }
        else if (appended > 0) { outcome = FrameOutcome::Extended; }
        else if (!has_moved) { outcome = FrameOutcome::Refreshed; }
        return Report(outcome, true, displacement, appended, by_continuity && !use_same);
    }
};

StitchEngine::StitchEngine(StitchLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
StitchEngine::~StitchEngine() = default;

void StitchEngine::Reset() { impl_ = std::make_unique<Impl>(impl_->limits); }

FrameResult StitchEngine::Submit(const FrameView& frame) {
    Impl& impl = *impl_;
    const bool usable = frame.pixels != nullptr && frame.width > 0 && frame.height > 0 &&
                        frame.stride >= frame.width;
    if (!usable || (impl.width != 0 && (frame.width != impl.width || frame.height != impl.frame_height))) {
        FrameResult result;
        result.outcome = FrameOutcome::Invalid;
        result.position = impl.position;
        result.height = impl.rows;
        result.at_limit = impl.at_limit;
        return result;
    }
    return impl.width == 0 ? impl.SubmitFirst(frame) : impl.SubmitNext(frame);
}

int StitchEngine::width() const noexcept { return impl_->width; }
int StitchEngine::height() const noexcept { return impl_->rows; }
int StitchEngine::frame_height() const noexcept { return impl_->frame_height; }
int StitchEngine::max_height() const noexcept { return impl_->max_rows; }
bool StitchEngine::at_limit() const noexcept { return impl_->at_limit; }

StitchLayout StitchEngine::layout() const noexcept {
    StitchLayout layout;
    layout.header_rows = UpperMedian(impl_->top_history, impl_->history_count);
    layout.footer_rows = UpperMedian(impl_->bottom_history, impl_->history_count);
    layout.left_rail = impl_->rail_left;
    layout.right_rail = impl_->rail_right;
    return layout;
}

const uint32_t* StitchEngine::Row(int y) const noexcept {
    return y >= 0 && y < impl_->rows ? impl_->RowPixels(y) : nullptr;
}

bool StitchEngine::CopyRows(int first, int count, uint32_t* out, std::ptrdiff_t stride) const noexcept {
    if (out == nullptr || first < 0 || count < 0 || first + count > impl_->rows || stride < impl_->width) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        std::memcpy(out + static_cast<std::ptrdiff_t>(i) * stride, impl_->RowPixels(first + i),
                    static_cast<size_t>(impl_->width) * sizeof(uint32_t));
    }
    return true;
}

const wchar_t* ToString(FrameOutcome outcome) noexcept {
    switch (outcome) {
        case FrameOutcome::First: return L"first";
        case FrameOutcome::Unchanged: return L"unchanged";
        case FrameOutcome::Refreshed: return L"refreshed";
        case FrameOutcome::Extended: return L"extended";
        case FrameOutcome::Moved: return L"moved";
        case FrameOutcome::OutsideCaptured: return L"outside-captured";
        case FrameOutcome::LimitReached: return L"limit-reached";
        case FrameOutcome::Ambiguous: return L"ambiguous";
        case FrameOutcome::LowInformation: return L"low-information";
        case FrameOutcome::NoOverlap: return L"no-overlap";
        case FrameOutcome::Invalid: return L"invalid";
    }
    return L"unknown";
}

}  // namespace easnap::scrollshot
