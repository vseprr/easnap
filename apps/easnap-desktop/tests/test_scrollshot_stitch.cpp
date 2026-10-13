// Easnap unit tests - scrolling-screenshot stitching engine.
//
// Every case builds a tall synthetic document, cuts viewport frames out of it
// along a scripted scroll trajectory and feeds them to the production engine.
// Clean input must reproduce the document exactly, noisy input within the
// noise, and input that cannot be placed must be reported as such while the
// canvas stays untouched. No window, device or desktop is used.

#include "scrollshot/stitch_engine.h"

#include "scrollshot_synthetic.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using easnap::scrollshot::FrameOutcome;
using easnap::scrollshot::FrameResult;
using easnap::scrollshot::FrameView;
using easnap::scrollshot::StitchEngine;
using easnap::scrollshot::StitchLimits;
using namespace easnap_test::synthetic;

namespace {

int g_pass = 0;
int g_fail = 0;
const char* g_case = "";

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  [%s]  %s\n", file, line, g_case, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

// --- Viewport composition ---------------------------------------------------

struct Scene {
    const Image* document{nullptr};
    int view_height{0};
    int header{0};
    int footer{0};
    int rail{0};
    int scrollbar{0};
    int max_scroll{1};
};

struct Moment {
    int scroll{0};
    int clock{0};            ///< Changes one block inside the header.
    int hover_top{-1};       ///< Viewport row of a hover highlight, or -1.
    int hover_height{0};
    int caret_row{-1};       ///< Document row of a text caret, or -1.
    int noise{0};            ///< Per-channel amplitude.
    uint32_t noise_seed{1};
    /// Document rectangle that shows a placeholder until it has loaded.
    int lazy_top{-1}, lazy_bottom{-1}, lazy_left{0}, lazy_right{0};
};

void DrawHeader(Image& frame, int height, int clock) {
    frame.Fill(0, 0, frame.width, height, Gray(240));
    frame.Fill(0, height - 1, frame.width, height, Gray(150));
    frame.Fill(14, 10, 14 + height - 20, height - 10, 0xFF2A7BDEu);
    Random random(4242);
    DrawTextLine(frame, height + 10, (height - 14) / 2, frame.width / 2, random);
    Random digits(900u + static_cast<uint32_t>(clock));
    DrawTextLine(frame, frame.width - 96, (height - 14) / 2, frame.width - 40, digits);
}

void DrawFooter(Image& frame, int height) {
    const int top = frame.height - height;
    frame.Fill(0, top, frame.width, frame.height, Gray(52));
    frame.Fill(0, top, frame.width, top + 1, Gray(20));
    Random random(5151);
    for (int x = 20; x < frame.width - 60; x += 70) {
        frame.Fill(x, top + 10, x + 44, frame.height - 10, Gray(random.Range(110, 200)));
    }
}

void DrawRail(Image& frame, int width) {
    frame.Fill(0, 0, width, frame.height, Gray(43));
    Random random(6161);
    for (int y = 12; y + 28 < frame.height; y += 40) {
        frame.Fill(10, y, 30, y + 20, Gray(random.Range(120, 230)));
        Image strip(width, 14, Gray(43));
        DrawTextLine(strip, 0, 0, width - 46, random);
        for (int row = 0; row < 14; ++row) {
            for (int x = 0; x < width - 46; ++x) {
                if (strip.Row(row)[x] != Gray(43)) { frame.Row(y + 3 + row)[38 + x] = Gray(225); }
            }
        }
    }
}

constexpr uint32_t kTrack = 0xFFF1F1F1u;
constexpr uint32_t kThumb = 0xFFA8A8A8u;
constexpr uint32_t kArrow = 0xFF606060u;

void DrawScrollbar(Image& frame, int width, int top, int bottom, int scroll, int max_scroll) {
    const int left = frame.width - width;
    frame.Fill(left, top, frame.width, bottom, kTrack);
    frame.Fill(left + 4, top + 5, frame.width - 4, top + 12, kArrow);
    frame.Fill(left + 4, bottom - 12, frame.width - 4, bottom - 5, kArrow);
    const int lane = bottom - top - 2 * width;
    const int thumb = std::max(24, lane / 5);
    const int offset = static_cast<int>(static_cast<int64_t>(lane - thumb) * scroll / std::max(1, max_scroll));
    frame.Fill(left + 3, top + width + offset, frame.width - 3, top + width + offset + thumb, kThumb);
}

Image Compose(const Scene& scene, const Moment& moment) {
    const Image& document = *scene.document;
    Image frame(document.width, scene.view_height, kWhite);
    for (int y = 0; y < scene.view_height; ++y) {
        const int source = moment.scroll + y;
        if (source >= 0 && source < document.height) {
            std::copy(document.Row(source), document.Row(source) + document.width, frame.Row(y));
        }
    }
    if (moment.lazy_top >= 0) {
        frame.Fill(moment.lazy_left, moment.lazy_top - moment.scroll, moment.lazy_right,
                   moment.lazy_bottom - moment.scroll, Gray(224));
    }
    if (moment.caret_row >= 0) {
        frame.Fill(200, moment.caret_row - moment.scroll, 202, moment.caret_row - moment.scroll + 16, Gray(0));
    }
    if (moment.hover_top >= 0) {
        for (int y = moment.hover_top; y < std::min(scene.view_height, moment.hover_top + moment.hover_height); ++y) {
            for (int x = scene.rail; x < frame.width - scene.scrollbar; ++x) {
                uint32_t& pixel = frame.Row(y)[x];
                uint32_t out = 0xFF000000u;
                for (int shift = 0; shift < 24; shift += 8) {
                    const uint32_t channel = (pixel >> shift) & 0xFFu;
                    out |= (channel > 18 ? channel - 18 : 0u) << shift;
                }
                pixel = out;
            }
        }
    }
    if (scene.rail > 0) { DrawRail(frame, scene.rail); }
    if (scene.scrollbar > 0) {
        DrawScrollbar(frame, scene.scrollbar, scene.header, scene.view_height - scene.footer, moment.scroll, scene.max_scroll);
    }
    if (scene.header > 0) { DrawHeader(frame, scene.header, moment.clock); }
    if (scene.footer > 0) { DrawFooter(frame, scene.footer); }
    if (moment.noise > 0) {
        Random random(moment.noise_seed);
        for (uint32_t& pixel : frame.pixels) {
            uint32_t out = 0xFF000000u;
            for (int shift = 0; shift < 24; shift += 8) {
                const int channel = static_cast<int>((pixel >> shift) & 0xFFu) + random.Range(-moment.noise, moment.noise);
                out |= static_cast<uint32_t>(std::clamp(channel, 0, 255)) << shift;
            }
            pixel = out;
        }
    }
    return frame;
}

FrameResult Submit(StitchEngine& engine, const Image& frame) {
    return engine.Submit(FrameView{frame.pixels.data(), frame.width, frame.height, frame.width});
}

[[nodiscard]] int MaxChannelDifference(uint32_t a, uint32_t b) {
    int worst = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        worst = std::max(worst, std::abs(static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu)));
    }
    return worst;
}

/// Rows of the canvas whose pixels in [left, right) differ from the document
/// by more than `tolerance`, where canvas row r shows document row origin + r.
[[nodiscard]] int DifferingRows(const StitchEngine& engine, const Image& document, int origin, int first, int end,
                                int left, int right, int tolerance = 0) {
    int differing = 0;
    for (int row = first; row < end; ++row) {
        const uint32_t* actual = engine.Row(row);
        const uint32_t* expected = document.Row(origin + row);
        bool same = actual != nullptr;
        for (int x = left; same && x < right; ++x) {
            same = MaxChannelDifference(actual[x], expected[x]) <= tolerance;
        }
        if (!same) {
            ++differing;
        }
    }
    return differing;
}

[[nodiscard]] bool IsPlaced(FrameOutcome outcome) {
    return outcome == FrameOutcome::First || outcome == FrameOutcome::Refreshed || outcome == FrameOutcome::Extended ||
           outcome == FrameOutcome::Moved || outcome == FrameOutcome::OutsideCaptured ||
           outcome == FrameOutcome::LimitReached;
}

/// Feeds one frame per scroll value. Returns true when every frame was placed
/// at exactly the scripted position.
bool RunTrajectory(StitchEngine& engine, const Scene& scene, const std::vector<int>& scrolls, int origin,
                   Moment moment = {}) {
    bool exact = true;
    for (size_t i = 0; i < scrolls.size(); ++i) {
        moment.scroll = scrolls[i];
        moment.noise_seed = static_cast<uint32_t>(i) + 1u;
        const FrameResult result = Submit(engine, Compose(scene, moment));
        if (!result.placed || result.position != scrolls[i] - origin) {
            std::fprintf(stderr, "  frame %zu: scroll %d outcome %ls placed %d position %d (expected %d)\n", i,
                         scrolls[i], easnap::scrollshot::ToString(result.outcome), result.placed ? 1 : 0,
                         result.position, scrolls[i] - origin);
            exact = false;
        }
    }
    return exact;
}

[[nodiscard]] std::vector<int> Steps(int start, std::initializer_list<int> deltas) {
    std::vector<int> scrolls{start};
    for (int delta : deltas) { scrolls.push_back(scrolls.back() + delta); }
    return scrolls;
}

[[nodiscard]] std::vector<int> Even(int start, int end, int step) {
    std::vector<int> scrolls;
    for (int value = start; step > 0 ? value <= end : value >= end; value += step) { scrolls.push_back(value); }
    return scrolls;
}

constexpr int kWidth = 640;
constexpr int kView = 360;

// --- Cases ------------------------------------------------------------------

void TestCleanScrollIsExact() {
    g_case = "clean scroll";
    const Image document = MakeArticle(kWidth, 4200, 11);
    Scene scene{&document, kView};
    for (int step : {1, 7, 37, 120, 233}) {
        StitchEngine engine;
        const int start = 40;
        // One pixel per frame is the slowest case per row; a short run covers it.
        const auto scrolls = Even(start, step == 1 ? 340 : 3000, step);
        CHECK(RunTrajectory(engine, scene, scrolls, start));
        const int expected_height = scrolls.back() - start + kView;
        CHECK(engine.height() == expected_height);
        CHECK(DifferingRows(engine, document, start, 0, std::min(engine.height(), expected_height), 0, kWidth) == 0);
        CHECK(!engine.at_limit());
    }
}

void TestSmoothScrollAndRepeatedFrames() {
    g_case = "smooth scroll";
    const Image document = MakeArticle(kWidth, 3000, 12);
    Scene scene{&document, kView};
    StitchEngine engine;
    // An eased wheel animation: small growing steps, repeats while it rests.
    std::vector<int> scrolls{100};
    Random random(77);
    for (int notch = 0; notch < 14; ++notch) {
        for (int delta : {2, 5, 11, 19, 24, 19, 11, 6, 3, 0, 0}) { scrolls.push_back(scrolls.back() + delta); }
        if (random.Chance(40)) { scrolls.push_back(scrolls.back()); }
    }
    int unchanged = 0;
    bool exact = true;
    for (int scroll : scrolls) {
        const FrameResult result = Submit(engine, Compose(scene, Moment{scroll}));
        if (result.outcome == FrameOutcome::Unchanged) { ++unchanged; }
        exact = exact && result.placed && result.position == scroll - 100;
    }
    CHECK(exact);
    CHECK(unchanged >= 14);
    CHECK(engine.height() == scrolls.back() - 100 + kView);
    CHECK(DifferingRows(engine, document, 100, 0, engine.height(), 0, kWidth) == 0);
}

void TestBothDirections() {
    g_case = "both directions";
    const Image document = MakeArticle(kWidth, 3600, 13);
    Scene scene{&document, kView};
    StitchEngine engine;
    std::vector<int> scrolls = Even(300, 900, 60);
    const auto up = Even(840, 420, -45);
    scrolls.insert(scrolls.end(), up.begin(), up.end());
    const auto down = Even(480, 2100, 75);
    scrolls.insert(scrolls.end(), down.begin(), down.end());
    int height_at_top = 0;
    bool exact = true;
    bool appended_while_returning = false;
    for (size_t i = 0; i < scrolls.size(); ++i) {
        const int before = engine.height();
        const FrameResult result = Submit(engine, Compose(scene, Moment{scrolls[i]}));
        exact = exact && result.placed && result.position == scrolls[i] - 300;
        if (i >= 11 && i < 11 + up.size()) {
            appended_while_returning = appended_while_returning || engine.height() != before || result.appended_rows != 0;
            if (result.outcome != FrameOutcome::Moved) { exact = false; }
            height_at_top = engine.height();
        }
    }
    CHECK(exact);
    CHECK(!appended_while_returning);
    CHECK(height_at_top == 900 - 300 + kView);
    CHECK(engine.height() == scrolls.back() - 300 + kView);
    CHECK(DifferingRows(engine, document, 300, 0, engine.height(), 0, kWidth) == 0);
}

void TestAboveTheStartAddsNothing() {
    g_case = "above the start";
    const Image document = MakeArticle(kWidth, 2600, 14);
    Scene scene{&document, kView};
    StitchEngine engine;
    CHECK(RunTrajectory(engine, scene, Even(600, 900, 50), 600));
    const int height = engine.height();
    bool outside = false;
    for (int scroll : Even(850, 380, -47)) {
        const FrameResult result = Submit(engine, Compose(scene, Moment{scroll}));
        CHECK(result.placed && result.position == scroll - 600);
        if (scroll < 600) {
            outside = outside || result.outcome == FrameOutcome::OutsideCaptured;
            CHECK(result.outcome == FrameOutcome::OutsideCaptured);
        }
    }
    CHECK(outside);
    CHECK(engine.height() == height);
    CHECK(RunTrajectory(engine, scene, Even(420, 1300, 55), 600));
    CHECK(DifferingRows(engine, document, 600, 0, engine.height(), 0, kWidth) == 0);
}

void TestTooFastThenRecover() {
    g_case = "too fast";
    const Image document = MakeArticle(kWidth, 5200, 15);
    Scene scene{&document, kView};
    StitchEngine engine;
    CHECK(RunTrajectory(engine, scene, Even(0, 600, 60), 0));
    const int height = engine.height();
    // A jump of more than one viewport leaves nothing in common.
    for (int scroll : {1500, 1560, 1620}) {
        const FrameResult result = Submit(engine, Compose(scene, Moment{scroll}));
        CHECK(!result.placed);
        CHECK(!IsPlaced(result.outcome));
        CHECK(result.appended_rows == 0);
    }
    CHECK(engine.height() == height);
    CHECK(DifferingRows(engine, document, 0, 0, engine.height(), 0, kWidth) == 0);
    // Scrolling back into captured rows restores tracking, wherever that is.
    FrameResult back = Submit(engine, Compose(scene, Moment{820}));
    CHECK(back.placed && back.position == 820);
    const auto onward = Even(880, 2400, 70);
    CHECK(RunTrajectory(engine, scene, onward, 0));
    CHECK(engine.height() == onward.back() + kView);
    CHECK(DifferingRows(engine, document, 0, 0, engine.height(), 0, kWidth) == 0);
}

void TestFixedHeaderAndFooter() {
    g_case = "header and footer";
    const Image document = MakeArticle(kWidth, 4000, 16);
    for (const auto [header, footer] : {std::pair{56, 0}, std::pair{0, 44}, std::pair{56, 44}}) {
        Scene scene{&document, kView, header, footer};
        StitchEngine engine;
        const auto scrolls = Even(120, 1900, 43);
        CHECK(RunTrajectory(engine, scene, scrolls, 120));
        const int height = scrolls.back() - 120 + kView;
        CHECK(engine.height() == height);
        // Content between the bands is the document; each band appears once.
        CHECK(DifferingRows(engine, document, 120, header, height - footer, 0, kWidth) == 0);
        const Image first = Compose(scene, Moment{120});
        const Image last = Compose(scene, Moment{scrolls.back()});
        int band_errors = 0;
        for (int y = 0; y < header; ++y) {
            band_errors += std::equal(first.Row(y), first.Row(y) + kWidth, engine.Row(y)) ? 0 : 1;
        }
        for (int y = 0; y < footer; ++y) {
            band_errors += std::equal(last.Row(kView - footer + y), last.Row(kView - footer + y) + kWidth,
                                      engine.Row(height - footer + y)) ? 0 : 1;
        }
        CHECK(band_errors == 0);
        CHECK(engine.layout().header_rows >= header);
        CHECK(engine.layout().footer_rows >= footer);
    }
}

void TestHeaderThatChanges() {
    g_case = "header clock";
    const Image document = MakeArticle(kWidth, 3200, 17);
    Scene scene{&document, kView, 56, 0};
    StitchEngine engine;
    const auto scrolls = Even(0, 1500, 50);
    bool exact = true;
    for (size_t i = 0; i < scrolls.size(); ++i) {
        Moment moment{scrolls[i]};
        moment.clock = static_cast<int>(i / 6);  // The clock ticks while scrolling.
        const FrameResult result = Submit(engine, Compose(scene, moment));
        exact = exact && result.placed && result.position == scrolls[i];
    }
    CHECK(exact);
    CHECK(DifferingRows(engine, document, 0, 56, engine.height(), 0, kWidth) == 0);
}

void TestScrollbar() {
    g_case = "scrollbar";
    const Image document = MakeArticle(kWidth, 4000, 18);
    Scene scene{&document, kView, 0, 0, 0, 17, 4000 - kView};
    StitchEngine engine;
    const auto scrolls = Even(0, 2600, 65);
    CHECK(RunTrajectory(engine, scene, scrolls, 0));
    CHECK(engine.height() == 2600 + kView);
    CHECK(DifferingRows(engine, document, 0, 0, engine.height(), 0, kWidth - 17) == 0);
    // The scrollbar columns hold scrollbar pixels only, never content.
    int foreign = 0;
    for (int row = 0; row < engine.height(); ++row) {
        for (int x = kWidth - 17; x < kWidth; ++x) {
            const uint32_t pixel = engine.Row(row)[x];
            if (pixel != kTrack && pixel != kThumb && pixel != kArrow) { ++foreign; }
        }
    }
    CHECK(foreign == 0);
}

void TestSideRail() {
    g_case = "side rail";
    const Image document = MakeArticle(kWidth, 4000, 19);
    Scene scene{&document, kView, 0, 0, 150, 0};
    StitchEngine engine;
    const auto scrolls = Even(0, 2200, 55);
    CHECK(RunTrajectory(engine, scene, scrolls, 0));
    CHECK(DifferingRows(engine, document, 0, 0, engine.height(), 150, kWidth) == 0);
    // The rail is taken as far as its own detail reaches; the plain strip
    // between its last item and the content is indistinguishable from content.
    CHECK(engine.layout().left_rail >= 120 && engine.layout().left_rail <= 150);
    CHECK(engine.layout().right_rail == 0);
    // The rail is shown once, in the first viewport, and continued flat below.
    const Image first = Compose(scene, Moment{0});
    const int rail = engine.layout().left_rail;
    int errors = 0;
    for (int row = 0; row < kView; ++row) {
        errors += std::equal(first.Row(row), first.Row(row) + rail, engine.Row(row)) ? 0 : 1;
    }
    for (int row = kView + 1; row < engine.height(); ++row) {
        errors += std::equal(engine.Row(kView), engine.Row(kView) + rail, engine.Row(row)) ? 0 : 1;
    }
    CHECK(errors == 0);
}

void TestChangesInsideTheOverlap() {
    g_case = "overlap changes";
    const Image document = MakeArticle(kWidth, 3600, 20);
    Scene scene{&document, kView};
    StitchEngine engine;
    const auto scrolls = Even(0, 2000, 40);
    bool exact = true;
    for (size_t i = 0; i < scrolls.size(); ++i) {
        Moment moment{scrolls[i]};
        moment.hover_top = 150;      // The pointer rests while content passes under it.
        moment.hover_height = 30;
        moment.caret_row = i % 2 == 0 ? 2100 : -1;  // A caret blinks in the last viewport.
        const FrameResult result = Submit(engine, Compose(scene, moment));
        exact = exact && result.placed && result.position == scrolls[i];
    }
    CHECK(exact);
    CHECK(engine.height() == 2000 + kView);
    // Only what the last viewport showed can differ: the highlight and the caret.
    CHECK(DifferingRows(engine, document, 0, 0, 2000, 0, kWidth) == 0);
    CHECK(DifferingRows(engine, document, 0, 2000, engine.height(), 0, kWidth) <= 30 + 16);
}

void TestLazyPictureIsRefreshed() {
    g_case = "lazy picture";
    Image document = MakeArticle(kWidth, 3000, 21);
    Random random(99);
    DrawPicture(document, 60, 700, 520, 900, random);
    Scene scene{&document, kView};
    StitchEngine engine;
    Moment pending;
    pending.lazy_top = 700; pending.lazy_bottom = 900; pending.lazy_left = 60; pending.lazy_right = 520;
    bool exact = true;
    for (int scroll : Even(300, 620, 40)) {
        pending.scroll = scroll;
        const FrameResult result = Submit(engine, Compose(scene, pending));
        exact = exact && result.placed && result.position == scroll - 300;
    }
    CHECK(exact);
    CHECK(DifferingRows(engine, document, 300, 400, 600, 0, kWidth) > 100);  // Still the placeholder.
    // The picture arrives while nothing scrolls.
    const FrameResult loaded = Submit(engine, Compose(scene, Moment{620}));
    CHECK(loaded.outcome == FrameOutcome::Refreshed);
    CHECK(loaded.displacement == 0 && loaded.appended_rows == 0);
    CHECK(RunTrajectory(engine, scene, Even(660, 1400, 40), 300));
    CHECK(DifferingRows(engine, document, 300, 0, engine.height(), 0, kWidth) == 0);
}

void TestLowInformationIsReported() {
    g_case = "low information";
    // Text, then an empty stretch taller than the viewport, then text again.
    Image document = MakeArticle(kWidth, 3000, 22);
    document.Fill(0, 700, kWidth, 1500, kWhite);
    Scene scene{&document, kView};
    StitchEngine engine;
    CHECK(RunTrajectory(engine, scene, Even(0, 660, 60), 0));
    const int height = engine.height();
    // Across the empty stretch nothing measures the offset. The engine may
    // see no change at all, but it must not invent rows.
    bool invented = false;
    for (int scroll : Even(720, 1300, 60)) {
        const FrameResult result = Submit(engine, Compose(scene, Moment{scroll}));
        invented = invented || result.appended_rows != 0 || (result.placed && result.position != 660 &&
                                                              result.outcome != FrameOutcome::Unchanged);
    }
    CHECK(!invented);
    CHECK(engine.height() == height);
    // New text enters below an empty viewport: there is nothing to align with.
    const FrameResult entering = Submit(engine, Compose(scene, Moment{1400}));
    CHECK(!entering.placed);
    CHECK(entering.outcome == FrameOutcome::LowInformation || entering.outcome == FrameOutcome::NoOverlap ||
          entering.outcome == FrameOutcome::Ambiguous);
    CHECK(engine.height() == height);
    CHECK(DifferingRows(engine, document, 0, 0, engine.height(), 0, kWidth) == 0);
}

void TestRepetitiveContent() {
    g_case = "repetitive";
    // Distinct text, then 1,400 rows that repeat exactly every 36, then text.
    Image document = MakeArticle(kWidth, 3600, 23);
    DrawList(document, 900, 2300, 36, false, 5);
    Scene scene{&document, kView};

    {
        // A list whose rows differ in text is ordinary content.
        Image list(kWidth, 3000, kWhite);
        DrawList(list, 0, 3000, 36, true, 9);
        Scene list_scene{&list, kView};
        StitchEngine engine;
        CHECK(RunTrajectory(engine, list_scene, Even(0, 2000, 108), 0));
        CHECK(DifferingRows(engine, list, 0, 0, engine.height(), 0, kWidth) == 0);
    }
    {
        // A steady scroll that starts on distinct content carries through the
        // repeating stretch by continuity.
        StitchEngine engine;
        const auto scrolls = Even(300, 2700, 13);
        bool by_continuity = false;
        bool exact = true;
        for (int scroll : scrolls) {
            const FrameResult result = Submit(engine, Compose(scene, Moment{scroll}));
            exact = exact && result.placed && result.position == scroll - 300;
            by_continuity = by_continuity || result.by_continuity;
        }
        CHECK(exact);
        CHECK(by_continuity);
        CHECK(DifferingRows(engine, document, 300, 0, engine.height(), 0, kWidth) == 0);
    }
    {
        // From rest, a jump inside the repeating stretch fits at every period.
        StitchEngine engine;
        CHECK(RunTrajectory(engine, scene, Even(300, 1100, 20), 300));
        Submit(engine, Compose(scene, Moment{1100}));  // The scroll pauses.
        const int height = engine.height();
        const FrameResult jump = Submit(engine, Compose(scene, Moment{1250}));
        CHECK(!jump.placed);
        CHECK(jump.outcome == FrameOutcome::Ambiguous);
        CHECK(engine.height() == height);
    }
}

void TestSizeLimit() {
    g_case = "size limit";
    const Image document = MakeArticle(kWidth, 4000, 24);
    Scene scene{&document, kView};
    StitchEngine engine(StitchLimits{1200, 48'000'000});
    CHECK(engine.max_height() == 0);
    bool reported = false;
    for (int scroll : Even(0, 2000, 50)) {
        const FrameResult result = Submit(engine, Compose(scene, Moment{scroll}));
        CHECK(result.height <= 1200);
        if (scroll + kView > 1200 && result.placed) {
            reported = reported || result.outcome == FrameOutcome::LimitReached;
            CHECK(result.at_limit);
        }
    }
    CHECK(engine.max_height() == 1200);
    CHECK(reported);
    CHECK(engine.at_limit());
    CHECK(engine.height() == 1200);
    CHECK(DifferingRows(engine, document, 0, 0, 1200, 0, kWidth) == 0);

    StitchEngine by_pixels(StitchLimits{32000, static_cast<int64_t>(kWidth) * 900});
    Submit(by_pixels, Compose(scene, Moment{0}));
    CHECK(by_pixels.max_height() == 900);
}

void TestNoisyInputStaysWithinTheNoise() {
    g_case = "noise";
    const Image document = MakeArticle(kWidth, 3200, 25);
    Scene scene{&document, kView};
    for (int noise : {1, 2, 4}) {
        StitchEngine engine;
        Moment moment;
        moment.noise = noise;
        const auto scrolls = Steps(50, {30, 30, 0, 61, 5, 90, 90, 0, 0, 140, 17, 200, 33, 75, 75, 75, 2, 110});
        CHECK(RunTrajectory(engine, scene, scrolls, 50, moment));
        CHECK(engine.height() == scrolls.back() - 50 + kView);
        CHECK(DifferingRows(engine, document, 50, 0, engine.height(), 0, kWidth, noise) == 0);
    }
}

void TestSparseContentInAWideFrame() {
    g_case = "sparse";
    // Short lines far apart: each row changes only a few segments.
    Image document(kWidth, 3000, kWhite);
    Random random(31);
    for (int y = 30; y < 2950; y += random.Range(38, 90)) {
        DrawTextLine(document, 20, y, 20 + random.Range(30, 110), random);
    }
    Scene scene{&document, kView};
    StitchEngine engine;
    CHECK(RunTrajectory(engine, scene, Even(0, 2200, 44), 0));
    CHECK(DifferingRows(engine, document, 0, 0, engine.height(), 0, kWidth) == 0);
}

void TestRandomTrajectoriesAreExactAndDeterministic() {
    g_case = "random";
    const Image document = MakeArticle(kWidth, 6000, 26);
    Scene scene{&document, kView};
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        Random random(seed);
        std::vector<int> scrolls{1000};
        int lowest = 1000, highest = 1000;
        for (int i = 0; i < 120; ++i) {
            const int delta = random.Chance(15) ? 0 : random.Range(-110, 190);
            scrolls.push_back(std::clamp(scrolls.back() + delta, 1000, 5600));
            lowest = std::min(lowest, scrolls.back());
            highest = std::max(highest, scrolls.back());
        }
        StitchEngine first, second;
        CHECK(RunTrajectory(first, scene, scrolls, 1000));
        CHECK(RunTrajectory(second, scene, scrolls, 1000));
        CHECK(first.height() == highest - 1000 + kView);
        CHECK(DifferingRows(first, document, 1000, 0, first.height(), 0, kWidth) == 0);
        bool identical = first.height() == second.height();
        for (int row = 0; identical && row < first.height(); ++row) {
            identical = std::equal(first.Row(row), first.Row(row) + kWidth, second.Row(row));
        }
        CHECK(identical);
    }
}

void TestInvalidFrames() {
    g_case = "invalid";
    StitchEngine engine;
    CHECK(engine.Submit(FrameView{}).outcome == FrameOutcome::Invalid);
    CHECK(engine.height() == 0 && engine.width() == 0);
    const Image document = MakeArticle(kWidth, 1200, 27);
    Scene scene{&document, kView};
    CHECK(Submit(engine, Compose(scene, Moment{0})).outcome == FrameOutcome::First);
    const Image other(kWidth - 2, kView, kWhite);
    const FrameResult rejected = Submit(engine, other);
    CHECK(rejected.outcome == FrameOutcome::Invalid && !rejected.placed);
    CHECK(engine.height() == kView);
    std::vector<uint32_t> copy(static_cast<size_t>(kWidth) * 10);
    CHECK(engine.CopyRows(5, 10, copy.data(), kWidth));
    CHECK(std::equal(copy.begin(), copy.begin() + kWidth, document.Row(5)));
    CHECK(!engine.CopyRows(kView - 5, 10, copy.data(), kWidth));
    CHECK(engine.Row(-1) == nullptr && engine.Row(kView) == nullptr);
    engine.Reset();
    CHECK(engine.height() == 0);
    CHECK(Submit(engine, other).outcome == FrameOutcome::First);
}

void TestNarrowAndWideFrames() {
    g_case = "sizes";
    for (const auto [width, view] : {std::pair{200, 240}, std::pair{1920, 300}}) {
        const Image document = MakeArticle(width, 2400, 28u + static_cast<uint32_t>(width));
        Scene scene{&document, view};
        StitchEngine engine;
        CHECK(RunTrajectory(engine, scene, Even(0, 1500, 31), 0));
        CHECK(DifferingRows(engine, document, 0, 0, engine.height(), 0, width) == 0);
    }
}

}  // namespace

int main() {
    TestCleanScrollIsExact();
    TestSmoothScrollAndRepeatedFrames();
    TestBothDirections();
    TestAboveTheStartAddsNothing();
    TestTooFastThenRecover();
    TestFixedHeaderAndFooter();
    TestHeaderThatChanges();
    TestScrollbar();
    TestSideRail();
    TestChangesInsideTheOverlap();
    TestLazyPictureIsRefreshed();
    TestLowInformationIsReported();
    TestRepetitiveContent();
    TestSizeLimit();
    TestNoisyInputStaysWithinTheNoise();
    TestSparseContentInAWideFrame();
    TestRandomTrajectoriesAreExactAndDeterministic();
    TestInvalidFrames();
    TestNarrowAndWideFrames();
    std::printf("scrollshot stitch: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
