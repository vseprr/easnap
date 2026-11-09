// Easnap integration test - Scrollshot against a generated application.
//
// A page with real text (ClearType, five fonts, bold and italic runs),
// photo-like pictures, a code block, a comment box with a blinking caret,
// pictures that load late and a list that grows at its end is scrolled the
// way a browser scrolls it: every wheel notch starts an eased animation that
// is presented once per display refresh. Pictures of it are taken at jittered
// intervals of 16 to 50 ms, so most of them fall in the middle of an
// animation, and handed to the production stitching engine as the capture
// session hands them over: identical pictures are dropped first. That filter
// is inline in the session's worker loop, so it is restated here with the same
// comparison (equality of the whole picture).
//
// The page model knows what every picture shows, so each placement is checked
// against the true offset and the stitched image against the page. Scenarios
// marked "measured" or "expected failure" only report what happened; they
// never turn wrong output into a pass. No window, desktop or capture is used.
// See docs/decisions/ADR-041-scrollshot-realistic-test-application.md.
//
// Options: --only <scenario> runs one scenario; --dump <directory> writes each
// scenario's pictures as NNN.png with a sequence.txt in the corpus manifest
// format, plus stitched.png and expected.png (use build/Testing/...; never the
// source tree: pictures are not committed).

#include "scrollshot/stitch_engine.h"

#include "scrollshot_realistic_app.h"

#include <bcrypt.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

namespace {

using easnap::scrollshot::FrameOutcome;
using easnap::scrollshot::FrameResult;
using easnap::scrollshot::FrameView;
using easnap::scrollshot::StitchEngine;
using namespace easnap_test::realistic;

int g_pass = 0;
int g_fail = 0;
const char* g_case = "";

void Check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL %d [%s]: %s\n", line, g_case, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

constexpr int kSkipped = 77;
constexpr double kVsyncMs = 1000.0 / 60.0;
constexpr double kSimulationLimitMs = 120000.0;

enum class Expectation : uint8_t {
    Exact,            ///< Every placement at the true offset; the image equals the page outside the masks.
    Placement,        ///< Every placement at the true offset; the image is measured.
    Measured,         ///< Reported only; the outcome is to be decided.
    ExpectedFailure,  ///< A documented limit of ADR-027; reported only.
};

enum class Pattern : uint8_t {
    Wheel,     ///< Notches 120-420 ms apart, sometimes two or three in quick succession.
    PageDown,  ///< Mostly Page Down, some notches.
    Settled,   ///< Like Wheel, but the next input waits until the page has come to rest and its pictures loaded.
    Flick,     ///< Page Down every 100-140 ms: pictures leave the view before they load.
};

struct Scenario {
    const char* name;
    const char* rule;  ///< What it exercises in ADR-027.
    Expectation expectation;
    Pattern pattern;
    AppOptions options;
    bool immediate{false};    ///< Render every picture again instead of copying the content layer.
    bool lazy_either{false};  ///< A late picture's rows may show its placeholder or the picture.
    /// Share of pictures, in percent, torn between two presents: the rows from
    /// a seeded split row down show the present before the latest one.
    int torn_percent{0};
};

[[nodiscard]] const char* ToString(Expectation expectation) {
    switch (expectation) {
        case Expectation::Exact: return "assert-exact";
        case Expectation::Placement: return "assert-placement";
        case Expectation::Measured: return "measured";
        case Expectation::ExpectedFailure: return "expected-failure";
    }
    return "?";
}

std::vector<Scenario> Scenarios() {
    std::vector<Scenario> list;
    const auto add = [&](const char* name, const char* rule, Expectation expectation, Pattern pattern, uint32_t seed,
                         auto&& configure) {
        Scenario scenario{name, rule, expectation, pattern, AppOptions{}};
        scenario.options.seed = seed;
        configure(scenario);
        list.push_back(scenario);
    };
    add("wheel-eased", "offset search, exact row hashes, Unchanged", Expectation::Exact, Pattern::Wheel, 4101,
        [](Scenario&) {});
    add("page-down", "search within one frame height", Expectation::Exact, Pattern::PageDown, 4102, [](Scenario&) {});
    add("sticky-header", "fixed bands, header from the first frame", Expectation::Exact, Pattern::Wheel, 4103,
        [](Scenario& s) { s.options.header = Header::Fixed; });
    add("caret-spinner", "55 % agreement, visible rows overwritten", Expectation::Exact, Pattern::Wheel, 4104,
        [](Scenario& s) {
            s.options.caret = true;
            s.options.spinner = true;
        });
    add("overlay-scrollbar", "a scrollbar is content", Expectation::Exact, Pattern::Wheel, 4105,
        [](Scenario& s) { s.options.overlay_scrollbar = true; });
    add("lazy-at-rest", "unmoved judged by rows; a late picture ends loaded", Expectation::Exact, Pattern::Settled, 4106,
        [](Scenario& s) { s.options.lazy_slots = 4; });
    add("lazy-in-motion", "an unplaced frame changes nothing", Expectation::Exact, Pattern::Flick, 4107, [](Scenario& s) {
        s.options.lazy_slots = 4;
        s.lazy_either = true;
    });
    add("growing-tail", "rows appended, Refreshed when the batch arrives", Expectation::Exact, Pattern::Settled, 4108,
        [](Scenario& s) { s.options.growing_tail = true; });
    add("solid-background", "a fixed solid background is no detail", Expectation::Exact, Pattern::Wheel, 4109,
        [](Scenario& s) { s.options.background = Background::FixedSolid; });
    add("cleartype-immediate", "exact row hashes need translation-invariant text", Expectation::Exact, Pattern::Wheel,
        4110, [](Scenario& s) {
            s.immediate = true;
            s.options.document_rows = 2000;
        });
    add("realistic-app", "everything at once, as the desktop test shows it", Expectation::Exact, Pattern::Settled, 4111,
        [](Scenario& s) {
            s.options.header = Header::Fixed;
            s.options.caret = true;
            s.options.spinner = true;
            s.options.lazy_slots = 3;
            s.options.growing_tail = true;
            s.options.overlay_scrollbar = true;
        });
    add("gradient-background", "informative segments; exact path lost on the background", Expectation::Placement,
        Pattern::Wheel, 4112, [](Scenario& s) { s.options.background = Background::FixedGradient; });
    add("sticky-section", "fixed bands that appear mid-scroll (median of five)", Expectation::Measured, Pattern::Wheel,
        4113, [](Scenario& s) { s.options.sticky_section = true; });
    // Finding (ADR-041): the research expected every placement within one row
    // or a refusal. Measured on 2026-11-09: 68 of 90 placements further off
    // (worst 3.07 rows), so this stays measured.
    add("fractional-offsets", "no exact path; tolerant descriptors only", Expectation::Measured, Pattern::Wheel, 4114,
        [](Scenario& s) { s.options.snap_to_pixels = false; });
    add("textured-background", "unmoved judged by rows may win over motion", Expectation::ExpectedFailure,
        Pattern::Wheel, 4115, [](Scenario& s) { s.options.background = Background::FixedTextured; });
    add("animating-header", "a header that changes every frame is not fixed", Expectation::ExpectedFailure,
        Pattern::Wheel, 4116, [](Scenario& s) { s.options.header = Header::Animating; });
    // Finding (ADR-041, lab Soak run 20261109-054120-soak): the desktop test's
    // image came out too tall while the shadow engine agreed with the session.
    // Offline, only torn pictures reproduce that - rows above a split from one
    // present, rows below it from the present before - and whether the lab's
    // pictures were torn is not yet shown. The engine accepts such a picture
    // on 55 % agreement of the whole picture, appends the part that disagrees,
    // and every later placement shifts: a product weakness, measured here and
    // left to a later engine change. The amendment of ADR-027 proposed for it
    // (not made): a placed picture must agree with the canvas in each part of
    // its overlap; appended rows stay provisional until a second picture
    // agrees; a contiguous contradicting band refuses the picture.
    add("torn-pictures", "a picture torn between two presents is placed whole (finding)", Expectation::ExpectedFailure,
        Pattern::Settled, 4117, [](Scenario& s) {
            s.options.header = Header::Fixed;
            s.options.caret = true;
            s.options.spinner = true;
            s.options.lazy_slots = 3;
            s.options.growing_tail = true;
            s.options.overlay_scrollbar = true;
            s.torn_percent = 20;
        });
    return list;
}

// --- Dumping in the corpus format ------------------------------------------------------

class Dump {
public:
    bool Open(const std::wstring& root, const char* scenario) {
        if (root.empty()) { return false; }
        directory_ = root + L"\\" + std::wstring(scenario, scenario + std::strlen(scenario));
        ::CreateDirectoryW(root.c_str(), nullptr);
        ::CreateDirectoryW(directory_.c_str(), nullptr);
        active_ = true;
        return true;
    }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] const std::wstring& directory() const { return directory_; }

    bool WritePng(const std::wstring& name, int width, int height, const uint32_t* pixels) {
        return easnap_test::realistic::WritePng(directory_ + L"\\" + name, width, height, pixels, width);
    }

    [[nodiscard]] std::string Sha256(const std::wstring& name) const {
        std::ifstream file(directory_ + L"\\" + name, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        UCHAR digest[32]{};
        if (!BCRYPT_SUCCESS(::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                         reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                         static_cast<ULONG>(bytes.size()), digest, sizeof(digest)))) {
            return "unavailable";
        }
        std::string hex;
        char pair[3]{};
        for (const UCHAR byte : digest) {
            std::snprintf(pair, sizeof(pair), "%02x", byte);
            hex += pair;
        }
        return hex;
    }

    void Line(const std::string& text) { manifest_ += text + "\n"; }

    void Close() {
        if (!active_) { return; }
        std::ofstream file(directory_ + L"\\sequence.txt", std::ios::binary);
        file << manifest_;
        active_ = false;
    }

private:
    bool active_{false};
    std::wstring directory_;
    std::string manifest_;
};

// --- One scenario ---------------------------------------------------------------------------

struct Figures {
    int captured{0};
    int identical{0};
    int submitted{0};
    int mid_animation{0};
    int written{0};
    int refreshed{0};      ///< Placed where the previous frame was, with changed rows.
    int loading_frames{0}; ///< Frames taken while a batch of the list was loading.
    int unchanged{0};
    int unchanged_while_moving{0};
    int ambiguous{0};
    int low_information{0};
    int no_overlap{0};
    int invalid{0};
    int misplaced{0};
    double worst_error{0};  ///< Rows between a placement and the true position.
    int inputs{0};
    int translation_frames{0};
    int64_t translation_pixels{0};
    int differing_rows{0};
    int first_differing{-1};
    int scrollbar_rows{0};
    int placeholder_rows{0};  ///< Rows of a late picture that kept the placeholder.
    int header_rows_differing{0};
    int height{0};
    int expected_height{0};
    int torn{0};          ///< Submitted pictures whose rows below the split differ from the latest present.
    int torn_written{0};  ///< Of those, the ones the engine wrote.
    bool reached_end{false};
    double simulated_ms{0};
};

[[nodiscard]] bool Written(const FrameResult& result) {
    return result.placed && result.outcome != FrameOutcome::Unchanged && result.outcome != FrameOutcome::Invalid;
}

/// Pixels of a column range equal, alpha ignored.
[[nodiscard]] bool SameSpan(const uint32_t* a, const uint32_t* b, int left, int right) {
    for (int x = left; x < right; ++x) {
        if (((a[x] ^ b[x]) & 0x00FFFFFFu) != 0) { return false; }
    }
    return true;
}

void RunScenario(const Scenario& scenario, const std::wstring& dump_root) {
    g_case = scenario.name;
    const auto wall_start = std::chrono::steady_clock::now();
    std::wstring error;
    auto app = Application::Create(scenario.options, error);
    CHECK(app != nullptr);
    if (app == nullptr) {
        std::fprintf(stderr, "  %s: %ls\n", scenario.name, error.c_str());
        return;
    }
    const int width = app->width(), view = app->view_height(), header = app->header_rows();
    Simulation simulation(*app, scenario.options.seed ^ 0xA5A5u);
    Random inputs(scenario.options.seed * 31u + 7u);
    Random capture_clock(scenario.options.seed * 17u + 3u);
    Random tearing(scenario.options.seed * 13u + 5u);
    StitchEngine engine;
    Dump dump;
    if (!dump_root.empty()) {
        CHECK(dump.Open(dump_root, scenario.name));
        dump.Line("easnap-scrollshot-sequence 1");
        dump.Line(std::string("name ") + scenario.name);
        dump.Line("source generated");
        dump.Line("content generated page, seed " + std::to_string(scenario.options.seed) +
                  "; no third-party text or pictures");
        dump.Line("application easnap-realistic-test-app 1");
        dump.Line("scale " + std::to_string(static_cast<int>(std::lround(scenario.options.scale * 100.0))));
        dump.Line("frame " + std::to_string(width) + "x" + std::to_string(view));
        dump.Line("limits 32000 48000000");
        dump.Line("truth ruler");
        if (!scenario.options.snap_to_pixels) { dump.Line("# positions are rounded: the presented offsets were fractional"); }
        dump.Line("# index file sha256 expectation [position]");
    }

    Figures figures;
    Pixels frame, previous, composed;
    State earlier;  // The present before the latest one, for a torn picture.
    bool has_earlier = false;
    std::vector<int> last_view_row;  // Canvas row -> viewport row it was last written from.
    int first_offset = -1;
    double first_position = 0;
    int last_written_offset = -1;
    double last_written_fraction = 0;
    int deepest_offset = 0;

    const auto capture = [&] {
        const State& state = simulation.state();
        bool torn = false;
        if (scenario.immediate) {
            CHECK(app->RenderImmediate(state, frame));
            app->Compose(state, composed);
            int64_t differing = 0;
            for (size_t i = 0; i < frame.data.size() && i < composed.data.size(); ++i) {
                differing += ((frame.data[i] ^ composed.data[i]) & 0x00FFFFFFu) != 0 ? 1 : 0;
            }
            if (differing != 0) { ++figures.translation_frames; }
            figures.translation_pixels += differing;
        } else {
            app->Compose(state, frame);
            if (scenario.torn_percent > 0 && has_earlier && tearing.Chance(scenario.torn_percent)) {
                // From the split down, the picture still shows the present before.
                const int split = tearing.Range(header + 1, view - 1);
                app->Compose(earlier, composed);
                for (int y = split; y < view; ++y) {
                    torn = torn || !SameSpan(frame.Row(y), composed.Row(y), 0, width);
                    std::copy(composed.Row(y), composed.Row(y) + width, frame.Row(y));
                }
            }
        }
        ++figures.captured;
        // The session's identical-picture filter, restated (it is inline in the
        // session's worker loop, which compares whole pictures with memcmp).
        if (frame.data == previous.data) {
            ++figures.identical;
            return;
        }
        previous = frame;
        const int index = figures.submitted++;
        if (state.animating) { ++figures.mid_animation; }
        if (state.loading) { ++figures.loading_frames; }
        deepest_offset = std::max(deepest_offset, state.offset);
        const FrameResult result = engine.Submit(FrameView{frame.data.data(), width, view, width});
        if (torn) {
            ++figures.torn;
            if (Written(result)) { ++figures.torn_written; }
        }
        if (first_offset < 0) {
            first_offset = state.offset;
            first_position = state.position;
            CHECK(result.outcome == FrameOutcome::First);
        }
        // What the frame shows: the presented offset, or the exact position
        // where the page was not snapped to whole pixels.
        const double truth = scenario.options.snap_to_pixels ? static_cast<double>(state.offset - first_offset)
                                                             : state.position - first_position;
        std::string expectation;
        const int truth_row = static_cast<int>(std::lround(truth));
        if (result.outcome == FrameOutcome::Invalid) {
            ++figures.invalid;
        } else if (result.outcome == FrameOutcome::Unchanged) {
            ++figures.unchanged;
            if (state.offset != last_written_offset || state.fraction != last_written_fraction) {
                ++figures.unchanged_while_moving;
            }
            expectation = "placed-or-refused " + std::to_string(truth_row);
        } else if (Written(result)) {
            ++figures.written;
            if (result.outcome == FrameOutcome::Refreshed) { ++figures.refreshed; }
            const double error_rows = std::abs(result.position - truth);
            figures.worst_error = std::max(figures.worst_error, error_rows);
            const bool wrong = scenario.options.snap_to_pixels ? error_rows != 0.0 : error_rows > 1.0;
            if (wrong) {
                if (figures.misplaced < 4) {
                    std::fprintf(stderr, "  %s frame %d: %ls at %d, true position %.2f%s\n", scenario.name, index,
                                 easnap::scrollshot::ToString(result.outcome), result.position, truth,
                                 torn ? " (torn)" : "");
                }
                ++figures.misplaced;
                expectation = "placed-or-refused " + std::to_string(truth_row);
            } else if (torn) {
                expectation = "placed-or-refused " + std::to_string(truth_row);
            } else {
                expectation = (result.outcome == FrameOutcome::First ? "first " : "placed ") + std::to_string(truth_row);
            }
            last_written_offset = state.offset;
            last_written_fraction = state.fraction;
            for (int y = 0; y < view; ++y) {
                const int row = state.offset - first_offset + y;
                if (row < 0) { continue; }
                if (static_cast<size_t>(row) >= last_view_row.size()) { last_view_row.resize(static_cast<size_t>(row) + 1, -1); }
                last_view_row[static_cast<size_t>(row)] = y;
            }
        } else {
            if (result.outcome == FrameOutcome::Ambiguous) { ++figures.ambiguous; }
            if (result.outcome == FrameOutcome::LowInformation) { ++figures.low_information; }
            if (result.outcome == FrameOutcome::NoOverlap) { ++figures.no_overlap; }
            expectation = "placed-or-refused " + std::to_string(truth_row);
        }
        if (dump.active()) {
            char name[32]{};
            std::snprintf(name, sizeof(name), "%03d.png", index);
            const std::wstring file(name, name + std::strlen(name));
            CHECK(dump.WritePng(file, width, view, frame.data.data()));
            dump.Line(std::to_string(index) + " " + name + " " + dump.Sha256(file) + " " + expectation);
        }
    };

    // --- The scripted user and the jittered capture clock -----------------------------
    double next_input = 300.0;
    int burst_left = 0;
    double settled_since = -1.0;
    double settle_pause = 0.0;
    double next_capture = 0.0;
    double end_at = -1.0;
    for (int tick = 0;; ++tick) {
        const double now = tick * kVsyncMs;
        figures.simulated_ms = now;
        if (now > kSimulationLimitMs) { break; }
        if (simulation.settled()) {
            if (settled_since < 0.0) { settled_since = now; }
        } else {
            settled_since = -1.0;
        }
        bool ready = end_at < 0.0 && now >= next_input;
        if (ready && scenario.pattern == Pattern::Settled && burst_left == 0) {
            ready = settled_since >= 0.0 && now - settled_since >= settle_pause;
        }
        if (ready && figures.inputs < 400) {
            const double at = now - inputs.Range(0, 15);  // Between the previous refresh and this one.
            ++figures.inputs;
            const bool page = scenario.pattern == Pattern::Flick ||
                              (scenario.pattern == Pattern::PageDown && inputs.Chance(70)) ||
                              (scenario.pattern == Pattern::Settled && burst_left == 0 && inputs.Chance(15));
            if (page) {
                simulation.PageDown(at);
            } else {
                simulation.Wheel(at, 1);
            }
            switch (scenario.pattern) {
                case Pattern::Flick: next_input = now + inputs.Range(100, 140); break;
                case Pattern::PageDown: next_input = now + inputs.Range(220, 520); break;
                case Pattern::Wheel:
                case Pattern::Settled:
                    if (burst_left > 0) {
                        --burst_left;
                    } else if (!page && inputs.Chance(30)) {
                        burst_left = inputs.Range(1, 2);
                    }
                    next_input = now + (burst_left > 0 ? inputs.Range(40, 90) : inputs.Range(120, 420));
                    settle_pause = inputs.Range(60, 200);
                    break;
            }
        }
        if (tick > 0) {
            earlier = simulation.state();
            has_earlier = true;
        }
        simulation.Present(now);
        if (end_at < 0.0 && simulation.at_end()) {
            figures.reached_end = true;
            end_at = now + 1100.0;  // Lets the overlay scrollbar fade out.
        }
        while (next_capture < now + kVsyncMs) {
            capture();
            next_capture += capture_clock.Range(16, 50);
        }
        if (end_at >= 0.0 && now >= end_at) { break; }
    }
    CHECK(figures.reached_end);

    // --- The stitched image against the page --------------------------------------------
    figures.height = engine.height();
    figures.expected_height = deepest_offset - std::max(first_offset, 0) + view;
    const int compared = std::min(figures.height, figures.expected_height);
    const int scrollbar_left = width - app->scrollbar_columns();
    const std::vector<Box> masks = app->masks();
    const bool fixed_background =
        scenario.options.background == Background::FixedGradient || scenario.options.background == Background::FixedTextured;
    std::vector<uint32_t> expected(static_cast<size_t>(width));
    Pixels expected_image;
    if (dump.active()) { expected_image.Reset(width, std::max(compared, 1), 0xFF000000u); }
    for (int row = 0; row < compared; ++row) {
        const uint32_t* actual = engine.Row(row);
        if (row < header) {
            const uint32_t* bar = app->header().Row(row);
            if (dump.active()) { std::copy(bar, bar + width, expected_image.Row(row)); }
            if (scenario.options.header == Header::Fixed && !SameSpan(actual, bar, 0, width)) { ++figures.header_rows_differing; }
            continue;
        }
        const int document_row = first_offset + row;
        int view_row = 0;
        if (fixed_background) {
            view_row = static_cast<size_t>(row) < last_view_row.size() ? last_view_row[static_cast<size_t>(row)] : -1;
            if (view_row < 0) { continue; }
        }
        app->ComposeContentRow(document_row, view_row, expected.data());
        if (dump.active()) { std::copy(expected.begin(), expected.end(), expected_image.Row(row)); }
        // Columns to compare: everything except the scrollbar and the masks.
        std::vector<std::pair<int, int>> skip;
        for (const Box& mask : masks) {
            if (mask.HasRow(document_row)) { skip.emplace_back(mask.left, mask.right); }
        }
        bool same = true;
        int x = 0;
        std::sort(skip.begin(), skip.end());
        for (const auto& [left, right] : skip) {
            same = same && SameSpan(actual, expected.data(), x, std::min(left, scrollbar_left));
            x = std::max(x, right);
        }
        same = same && SameSpan(actual, expected.data(), x, scrollbar_left);
        if (!same && scenario.lazy_either) {
            // A picture that left the view before it loaded keeps its placeholder.
            same = true;
            for (size_t slot = 0; slot < app->lazy_slots().size(); ++slot) {
                const Box& box = app->lazy_slots()[slot];
                if (!box.HasRow(document_row)) { continue; }
                const uint32_t* placeholder = app->placeholder(slot).Row(document_row - box.top);
                const bool loaded = SameSpan(actual, expected.data(), box.left, box.right);
                const bool pending = SameSpan(actual + box.left, placeholder, 0, box.right - box.left);
                if (pending && !loaded) { ++figures.placeholder_rows; }
                same = same && (loaded || pending) && SameSpan(actual, expected.data(), 0, box.left) &&
                       SameSpan(actual, expected.data(), box.right, scrollbar_left);
            }
            same = same && std::any_of(app->lazy_slots().begin(), app->lazy_slots().end(),
                                       [&](const Box& box) { return box.HasRow(document_row); });
        }
        if (!same) {
            if (figures.differing_rows == 0) { figures.first_differing = row; }
            ++figures.differing_rows;
        }
        if (scrollbar_left < width && !SameSpan(actual, expected.data(), scrollbar_left, width)) { ++figures.scrollbar_rows; }
    }

    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_start).count();
    std::printf("MEASURE scrollshot_realistic_stitch scenario=%s expectation=%s frames=%d identical=%d submitted=%d "
                "mid_animation=%d written=%d refreshed=%d loading_frames=%d unchanged=%d unchanged_while_moving=%d "
                "refused=%d (ambiguous=%d low_information=%d no_overlap=%d) misplaced=%d worst_error_rows=%.2f "
                "height=%d expected_height=%d differing_rows=%d first_differing=%d header_rows_differing=%d "
                "scrollbar_rows=%d placeholder_rows=%d inputs=%d simulated_ms=%.0f wall_ms=%.0f",
                scenario.name, ToString(scenario.expectation), figures.captured, figures.identical, figures.submitted,
                figures.mid_animation, figures.written, figures.refreshed, figures.loading_frames, figures.unchanged,
                figures.unchanged_while_moving,
                figures.ambiguous + figures.low_information + figures.no_overlap, figures.ambiguous,
                figures.low_information, figures.no_overlap, figures.misplaced, figures.worst_error, figures.height,
                figures.expected_height, figures.differing_rows, figures.first_differing, figures.header_rows_differing,
                figures.scrollbar_rows, figures.placeholder_rows, figures.inputs, figures.simulated_ms, wall_ms);
    const easnap::scrollshot::StitchLayout layout = engine.layout();
    std::printf(" layout=%d,%d,%d,%d", layout.header_rows, layout.footer_rows, layout.left_rail, layout.right_rail);
    if (scenario.immediate) {
        std::printf(" translation_frames=%d translation_pixels=%lld", figures.translation_frames,
                    static_cast<long long>(figures.translation_pixels));
    }
    if (scenario.torn_percent > 0) {
        std::printf(" torn_percent=%d torn=%d torn_written=%d height_delta=%d", scenario.torn_percent, figures.torn,
                    figures.torn_written, figures.height - figures.expected_height);
    }
    std::printf("\n  rule: %s\n", scenario.rule);

    CHECK(figures.invalid == 0);
    switch (scenario.expectation) {
        case Expectation::Exact:
            CHECK(figures.misplaced == 0);
            CHECK(figures.height == figures.expected_height);
            CHECK(figures.differing_rows == 0);
            CHECK(figures.header_rows_differing == 0);
            if (scenario.immediate) { CHECK(figures.translation_pixels == 0); }
            break;
        case Expectation::Placement:
            CHECK(figures.misplaced == 0);
            CHECK(figures.height == figures.expected_height);
            break;
        case Expectation::Measured:
        case Expectation::ExpectedFailure:
            break;  // Reported above; never counted as a pass of the image.
    }

    if (dump.active()) {
        dump.Line("final-height " + std::to_string(figures.expected_height));
        if (figures.height > 0) {
            std::vector<uint32_t> canvas(static_cast<size_t>(width) * static_cast<size_t>(figures.height));
            CHECK(engine.CopyRows(0, figures.height, canvas.data(), width));
            CHECK(dump.WritePng(L"stitched.png", width, figures.height, canvas.data()));
        }
        if (compared > 0) { CHECK(dump.WritePng(L"expected.png", width, compared, expected_image.data.data())); }
        dump.Close();
        std::printf("  dumped to %ls\n", dump.directory().c_str());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring dump_root;
    std::wstring only;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--dump") == 0 && i + 1 < argc) {
            dump_root = argv[++i];
        } else if (std::wcscmp(argv[i], L"--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else {
            std::fprintf(stderr, "usage: test_scrollshot_realistic [--only <scenario>] [--dump <directory>]\n");
            return 2;
        }
    }
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) { return 1; }
    const std::wstring unavailable = UnavailableReason();
    if (!unavailable.empty()) {
        std::printf("SKIPPED: the generated application cannot be rendered here: %ls.\n", unavailable.c_str());
        ::CoUninitialize();
        return kSkipped;
    }
    int ran = 0;
    for (const Scenario& scenario : Scenarios()) {
        const std::wstring name(scenario.name, scenario.name + std::strlen(scenario.name));
        if (!only.empty() && name != only) { continue; }
        ++ran;
        RunScenario(scenario, dump_root);
    }
    if (ran == 0) {
        std::fprintf(stderr, "No scenario is called %ls\n", only.c_str());
        ++g_fail;
    }
    std::printf("scrollshot realistic stitch: %d passed, %d failed\n", g_pass, g_fail);
    ::CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}
