// Easnap tests - a generated application for Scrollshot tests.
//
// A deterministic page of the kinds of content a real application shows -
// headings and paragraphs in several fonts, code, a quote, lists, photo-like
// pictures, a comment box with a caret, pictures that load late and a list
// that grows at its end - rendered with Direct2D and DirectWrite (ClearType)
// and scrolled the way a browser scrolls it: a wheel notch starts an eased
// animation that is presented once per display refresh.
//
// The page is rasterised once into a content layer. A viewport is composed
// from that layer at an integer offset by copying rows, so what any frame
// shows is known exactly; the parts that change on their own (caret,
// spinner, overlay scrollbar, late pictures, the loading row) are drawn over
// it and reported as masks. Nothing is read from disk and nothing from a
// third party is used: text is made of generated syllables, pictures are
// computed. See docs/decisions/ADR-041-scrollshot-realistic-test-application.md.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace easnap_test::realistic {

/// Top-down 32-bit BGRA pixels, always opaque.
struct Pixels {
    int width{0};
    int height{0};
    std::vector<uint32_t> data;

    void Reset(int w, int h, uint32_t fill);
    [[nodiscard]] uint32_t* Row(int y) { return data.data() + static_cast<size_t>(y) * static_cast<size_t>(width); }
    [[nodiscard]] const uint32_t* Row(int y) const {
        return data.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
    }
};

/// A rectangle in pixels; `right` and `bottom` are exclusive.
struct Box {
    int left{0};
    int top{0};
    int right{0};
    int bottom{0};

    [[nodiscard]] bool Empty() const { return right <= left || bottom <= top; }
    [[nodiscard]] bool HasRow(int y) const { return y >= top && y < bottom; }
};

/// Deterministic generator: a failing case reproduces from its seed.
class Random {
public:
    explicit Random(uint32_t seed) : state_(seed * 2654435761u + 0x9E3779B9u) {}
    uint32_t Next() {
        state_ = state_ * 1664525u + 1013904223u;
        return state_ >> 8;
    }
    int Range(int low, int high) { return low + static_cast<int>(Next() % static_cast<uint32_t>(high - low + 1)); }
    bool Chance(int percent) { return static_cast<int>(Next() % 100u) < percent; }

private:
    uint32_t state_;
};

/// What lies behind the page's content.
enum class Background : uint8_t {
    Page,           ///< An opaque page that scrolls with its content.
    FixedSolid,     ///< One colour fixed to the viewport (CSS background-attachment: fixed).
    FixedGradient,  ///< A vertical gradient fixed to the viewport.
    FixedTextured,  ///< A detailed texture fixed to the viewport.
};

enum class Header : uint8_t {
    None,
    Fixed,      ///< An application bar fixed to the top of the viewport from the start.
    Animating,  ///< The same bar with an activity indicator that changes every frame.
};

struct AppOptions {
    uint32_t seed{1};
    int width{720};        ///< Viewport width in pixels.
    int view_height{560};  ///< Viewport height in pixels, header included.
    double scale{1.25};    ///< Device pixels per DIP (1.25 is 125 %).
    int document_rows{3200};  ///< Approximate height of the page before any tail is loaded.
    Header header{Header::None};
    /// A section heading that scrolls with the page until it reaches the top
    /// of the viewport and stays there from then on (CSS position: sticky).
    bool sticky_section{false};
    Background background{Background::Page};
    bool caret{false};    ///< A blinking caret in the comment box.
    bool spinner{false};  ///< An activity indicator in the page that changes every frame.
    int lazy_slots{0};    ///< Pictures that show a placeholder until they have loaded.
    int lazy_delay_ms{250};  ///< Time from first being visible to loaded.
    bool growing_tail{false};  ///< The list at the end loads more rows when it is reached.
    int tail_batches{3};
    int tail_delay_ms{400};
    bool overlay_scrollbar{false};  ///< A thin scrollbar over the content that fades when idle.
    bool snap_to_pixels{true};      ///< Scroll offsets are rounded to whole device pixels.
    int caret_blink_ms{530};        ///< 0 or less keeps the caret on.
};

/// Why the generated application cannot be rendered here, or empty when it can.
/// Covers Direct2D, DirectWrite, the WARP device and the five fonts it uses.
[[nodiscard]] std::wstring UnavailableReason();

/// What one presented frame shows.
struct State {
    double time_ms{0};
    double position{0};  ///< Exact scroll position in document rows.
    int offset{0};       ///< Document row at the top of the viewport (rounded down when not snapped).
    double fraction{0};  ///< Sub-pixel part of the position; 0 when snapped.
    uint64_t frame{0};   ///< Presented frames so far; the spinners advance with it.
    bool caret_on{true};
    bool animating{false};
    std::vector<uint8_t> lazy_loaded;  ///< One flag per lazy slot.
    int loaded_height{0};              ///< Document rows that exist now.
    bool loading{false};               ///< The loading row is shown below `loaded_height`.
    bool complete{true};               ///< No more rows will be loaded.
    int max_scroll{0};                 ///< Largest offset the page allows now.
    double scrollbar_alpha{0};
};

class Application {
public:
    /// Lays out and rasterises the page. Null with `error` set on failure.
    static std::unique_ptr<Application> Create(const AppOptions& options, std::wstring& error);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] const AppOptions& options() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int view_height() const;
    [[nodiscard]] int header_rows() const;
    /// Height of the page with every batch of the tail loaded.
    [[nodiscard]] int content_height() const;
    /// Rows that exist before any batch of the tail is loaded.
    [[nodiscard]] int base_height() const;
    /// Rows that exist after `batches` batches of the tail have loaded.
    [[nodiscard]] int loaded_height_after(int batches) const;
    [[nodiscard]] int loading_row_height() const;
    [[nodiscard]] int notch_rows() const;  ///< Rows one wheel notch scrolls.
    [[nodiscard]] int page_rows() const;   ///< Rows one Page Down scrolls.

    /// The page with everything loaded. For a fixed background, the page drawn
    /// over black; `transmission()` then holds how much of the background
    /// shows through each channel (0-255).
    [[nodiscard]] const Pixels& content() const;
    [[nodiscard]] const Pixels& transmission() const;
    /// The application bar as the first frame shows it (empty without one).
    [[nodiscard]] const Pixels& header() const;
    /// A lazy slot's placeholder, slot-sized.
    [[nodiscard]] const Pixels& placeholder(size_t slot) const;

    /// Document rectangles whose pixels change on their own: the caret and
    /// the spinner. They are left out of every exact comparison.
    [[nodiscard]] std::vector<Box> masks() const;
    /// Document rectangles of the pictures that load late.
    [[nodiscard]] const std::vector<Box>& lazy_slots() const;
    /// Columns at the right edge that the overlay scrollbar can cover (0 without one).
    [[nodiscard]] int scrollbar_columns() const;
    /// Document row of the sticky section heading, or -1.
    [[nodiscard]] int sticky_section_top() const;
    /// Background pixel of viewport row `y`, column `x` (for fixed backgrounds).
    [[nodiscard]] uint32_t BackgroundPixel(int x, int y) const;
    /// Document row `row` composed over viewport background row `view_row`.
    void ComposeContentRow(int row, int view_row, uint32_t* out) const;

    /// Composes the viewport for `state` from the content layer.
    void Compose(const State& state, Pixels& frame) const;
    /// Renders the viewport for `state` again with Direct2D, translated to the
    /// state's offset, instead of copying the content layer. Only for a page
    /// background and snapped offsets. False when rendering failed.
    [[nodiscard]] bool RenderImmediate(const State& state, Pixels& frame);

    struct Impl;

private:
    explicit Application(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// The scroll model and the behaviours that change with time. Times are
/// milliseconds on any clock that only moves forward.
class Simulation {
public:
    Simulation(const Application& app, uint32_t seed);

    /// Positive notches scroll down. The animation restarts from the current
    /// position towards the accumulated target, as browsers do.
    void Wheel(double time_ms, int notches);
    void PageDown(double time_ms);
    /// Advances the behaviours to `time_ms` and fixes the frame presented then.
    void Present(double time_ms);

    [[nodiscard]] const State& state() const { return state_; }
    [[nodiscard]] int max_scroll() const;
    [[nodiscard]] int target() const;
    [[nodiscard]] int pending_lazy() const;
    /// No animation and no lazy picture waiting to load.
    [[nodiscard]] bool settled() const;
    /// Settled at the very end of a page that loads nothing more.
    [[nodiscard]] bool at_end() const;

private:
    [[nodiscard]] double PositionAt(double time_ms) const;
    void Retarget(double time_ms, double rows);

    const Application& app_;
    Random random_;
    double start_{0};
    double target_{0};
    double started_ms_{0};
    double duration_ms_{0};
    double last_motion_ms_{-1e9};
    bool moved_{false};
    std::vector<double> first_seen_ms_;
    int batches_{0};
    double loading_since_ms_{0};
    State state_;
};

// --- Side channel for a test that shows the application on the desktop ---------

/// Published by the process that shows the application, read by the test
/// that captures it: the exact pixels of the content layer and the state that
/// decides what is on screen. Dynamic values are written with interlocked
/// operations; the pixels are written once before `ready` is set.
struct SharedHeader {
    static constexpr uint32_t kMagic = 0x52535345u;  // "ESSR"
    static constexpr int kMaxBoxes = 8;
    uint32_t magic{0};
    uint32_t version{0};
    int32_t width{0};
    int32_t view_height{0};
    int32_t header_rows{0};
    int32_t content_height{0};
    int32_t scrollbar_columns{0};
    int32_t mask_count{0};
    int32_t slot_count{0};
    Box masks[kMaxBoxes]{};
    Box slots[kMaxBoxes]{};
    uint64_t header_offset{0};   ///< Byte offset of the header pixels in the mapping.
    uint64_t content_offset{0};  ///< Byte offset of the content pixels.
    volatile LONG ready{0};
    volatile LONG offset{0};  ///< Offset of the last presented frame.
    volatile LONG target{0};
    volatile LONG animating{0};
    volatile LONG pending_lazy{0};
    volatile LONG loading{0};
    volatile LONG loaded_height{0};  ///< Never decreases.
    volatile LONG max_scroll{0};
    volatile LONG complete{0};
    volatile LONG inputs{0};  ///< Wheel and key messages handled.
    volatile LONG presented{0};
};

[[nodiscard]] std::wstring SharedMappingName(DWORD process_id);
[[nodiscard]] LONG ReadShared(const volatile LONG& value);
void WriteShared(volatile LONG& value, LONG number);

/// The publishing side: creates the mapping and copies the pixels into it.
class SharedPublisher {
public:
    SharedPublisher() = default;
    ~SharedPublisher();
    SharedPublisher(const SharedPublisher&) = delete;
    SharedPublisher& operator=(const SharedPublisher&) = delete;

    [[nodiscard]] bool Create(const std::wstring& name, const Application& app);
    void Publish(const Simulation& simulation, LONG inputs, LONG presented);
    void SetReady();

private:
    HANDLE mapping_{nullptr};
    SharedHeader* header_{nullptr};
};

/// The reading side.
class SharedReader {
public:
    SharedReader() = default;
    ~SharedReader();
    SharedReader(const SharedReader&) = delete;
    SharedReader& operator=(const SharedReader&) = delete;

    /// True once the mapping exists and its publisher has set `ready`.
    [[nodiscard]] bool Open(const std::wstring& name);
    [[nodiscard]] const SharedHeader& header() const { return *header_; }
    [[nodiscard]] const uint32_t* ContentRow(int y) const;
    [[nodiscard]] const uint32_t* HeaderRow(int y) const;

private:
    HANDLE mapping_{nullptr};
    const SharedHeader* header_{nullptr};
    const uint8_t* base_{nullptr};
};

}  // namespace easnap_test::realistic
