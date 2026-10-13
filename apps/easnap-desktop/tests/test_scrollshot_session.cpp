// Easnap unit tests - scrolling-screenshot capture session.
//
// The production session (worker thread, stitching, preview, notifications,
// stop barrier) runs against a scripted capture provider that serves viewports
// of a synthetic document. No desktop is captured and no window is shown; the
// only window is a message-only target for the progress notifications.

#include "scrollshot/scrollshot_session.h"

#include "scrollshot_synthetic.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using easnap::capture::RegionCaptureResult;
using easnap::capture::RegionCaptureStatus;
using easnap::capture::RegionImageOptions;
using easnap::platform::BgraImage;
using easnap::scrollshot::ScrollshotCondition;
using easnap::scrollshot::ScrollshotPreview;
using easnap::scrollshot::ScrollshotProgress;
using easnap::scrollshot::ScrollshotSession;
using namespace easnap_test::synthetic;
using namespace std::chrono_literals;

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

constexpr int kWidth = 480;
constexpr int kView = 300;
constexpr UINT kProgressMessage = WM_APP + 77;

/// What the scripted screen currently shows, shared with the test thread.
struct Screen {
    std::mutex mutex;
    std::condition_variable served;
    const Image* document{nullptr};
    int scroll{0};
    int served_scroll{-1};       ///< Scroll of the picture last handed out.
    bool fail{false};
    bool report_no_change{true}; ///< Answer NoNewFrame while nothing moved, as duplication does.
    std::chrono::milliseconds capture_time{0};
    bool capturing{false};       ///< A slow capture is in progress right now.
    int calls{0};
    RegionImageOptions last_options{};
};

class ScriptedProvider final : public easnap::capture::IRegionCaptureProvider {
public:
    explicit ScriptedProvider(std::shared_ptr<Screen> screen) : screen_(std::move(screen)) {}

    bool CaptureDesktop(bool, easnap::capture::DesktopSnapshot&) override { return false; }
    bool CaptureRegion(const RECT&, bool, BgraImage&) override { return false; }
    RegionCaptureResult CaptureRegionFrame(const RECT&, bool, easnap::media::MediaTime, easnap::media::MediaTime,
                                            easnap::media::VideoFrame&) override {
        return {};
    }

    RegionCaptureResult CaptureRegionImage(const RECT& region, const RegionImageOptions& options,
                                            BgraImage& out) override {
        std::chrono::milliseconds capture_time{0};
        {
            const std::scoped_lock lock(screen_->mutex);
            capture_time = screen_->capture_time;
            screen_->capturing = capture_time.count() > 0;
        }
        if (capture_time.count() > 0) {
            screen_->served.notify_all();
            std::this_thread::sleep_for(capture_time);  // Stands for a slow driver, not for synchronisation.
        }
        RegionCaptureResult result;
        {
            const std::scoped_lock lock(screen_->mutex);
            screen_->capturing = false;
            ++screen_->calls;
            screen_->last_options = options;
            if (screen_->fail) {
                result.status = RegionCaptureStatus::Failed;
            } else if (screen_->report_no_change && screen_->served_scroll == screen_->scroll) {
                result.status = RegionCaptureStatus::NoNewFrame;
            } else {
                const int width = region.right - region.left;
                const int height = region.bottom - region.top;
                out.Reset(width, height);
                for (int y = 0; y < height; ++y) {
                    const uint32_t* source = screen_->document->Row(screen_->scroll + y);
                    std::copy(source, source + width, out.row(y));
                }
                screen_->served_scroll = screen_->scroll;
                result.status = RegionCaptureStatus::FrameAcquired;
            }
        }
        screen_->served.notify_all();
        return result;
    }

    void Release() noexcept override { released_.store(true); }
    [[nodiscard]] const wchar_t* name() const override { return L"scripted"; }
    [[nodiscard]] bool released() const noexcept { return released_.load(); }

private:
    std::shared_ptr<Screen> screen_;
    std::atomic<bool> released_{false};
};

ScrollshotSession::Options MakeOptions(uint64_t generation = 7, HWND window = nullptr) {
    ScrollshotSession::Options options;
    options.region = RECT{100, 50, 100 + kWidth, 50 + kView};
    options.generation = generation;
    options.notify_window = window;
    options.notify_message = kProgressMessage;
    options.preview_width = 120;
    return options;
}

/// Waits for a condition on the session's progress; never a fixed sleep.
template <typename Predicate>
bool WaitForProgress(const ScrollshotSession& session, Predicate predicate,
                     std::chrono::milliseconds deadline = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        if (predicate(session.progress())) { return true; }
        std::this_thread::sleep_for(2ms);
    }
    return predicate(session.progress());
}

void ScrollTo(Screen& screen, int scroll) {
    const std::scoped_lock lock(screen.mutex);
    screen.scroll = scroll;
}

bool SameAsDocument(const BgraImage& image, const Image& document, int origin) {
    if (image.width != document.width) { return false; }
    for (int y = 0; y < image.height; ++y) {
        if (!std::equal(image.row(y), image.row(y) + image.width, document.Row(origin + y))) { return false; }
    }
    return true;
}

void TestCaptureStitchesWhatScrollsPast() {
    const Image document = MakeArticle(kWidth, 3000, 41);
    auto screen = std::make_shared<Screen>();
    screen->document = &document;
    screen->scroll = 200;
    auto provider = std::make_unique<ScriptedProvider>(screen);
    ScriptedProvider* observed = provider.get();
    ScrollshotSession session(std::move(provider), MakeOptions());

    CHECK(session.Start());
    ScrollshotProgress progress = session.progress();
    CHECK(progress.generation == 7);
    CHECK(progress.width == kWidth && progress.height == kView);
    CHECK(progress.condition == ScrollshotCondition::Following);
    CHECK(!progress.finished);
    {
        const std::scoped_lock lock(screen->mutex);
        // The pointer never belongs to a scrollshot, a black monitor part is a
        // failure, and an idle screen is waited for instead of polled.
        CHECK(!screen->last_options.include_cursor);
        CHECK(screen->last_options.require_every_output);
        CHECK(screen->last_options.wait_milliseconds > 0);
    }

    int scroll = 200;
    bool followed = true;
    for (int step : {40, 75, 12, 130, 60, 90, 5, 110, 150, 33}) {
        scroll += step;
        ScrollTo(*screen, scroll);
        const int expected = scroll - 200 + kView;
        followed = followed && WaitForProgress(session, [expected](const ScrollshotProgress& p) {
            return p.height == expected;
        });
    }
    CHECK(followed);

    // The reduced image is rebuilt on the publishing cadence, so it follows
    // the stitched height with a short delay.
    ScrollshotPreview preview;
    const int reduced_height = ((scroll - 200 + kView) * 120 + kWidth - 1) / kWidth;
    CHECK(WaitForProgress(session, [&](const ScrollshotProgress&) {
        session.CopyPreview(10000, preview);
        return preview.total_height == reduced_height;
    }));
    progress = session.progress();
    CHECK(preview.width == 120);
    CHECK(preview.height == preview.total_height);
    CHECK(preview.pixels.size() == static_cast<size_t>(preview.width) * preview.height);
    CHECK(preview.version > 0 && preview.version == progress.preview_version);
    ScrollshotPreview tail;
    session.CopyPreview(20, tail);
    CHECK(tail.height == 20 && tail.total_height == preview.total_height);
    CHECK(std::equal(tail.pixels.begin(), tail.pixels.end(),
                     preview.pixels.end() - static_cast<std::ptrdiff_t>(tail.pixels.size())));

    CHECK(session.Stop(5000ms));
    CHECK(observed->released());
    CHECK(session.progress().finished);
    const BgraImage result = session.TakeResult();
    CHECK(result.width == kWidth && result.height == scroll - 200 + kView);
    CHECK(SameAsDocument(result, document, 200));
    CHECK(session.TakeResult().empty());  // Handed out once.
    CHECK(session.Stop(10ms));            // Stopping again is harmless.
}

void TestStartFailsWhenTheScreenCannotBeRead() {
    const Image document = MakeArticle(kWidth, 1000, 42);
    auto screen = std::make_shared<Screen>();
    screen->document = &document;
    screen->fail = true;
    ScrollshotSession session(std::make_unique<ScriptedProvider>(screen), MakeOptions());
    CHECK(!session.Start(400ms));
    CHECK(session.TakeResult().empty());
    CHECK(!session.Start(100ms));  // A session is not restarted.

    ScrollshotSession::Options empty = MakeOptions();
    empty.region = RECT{};
    ScrollshotSession invalid(std::make_unique<ScriptedProvider>(screen), empty);
    CHECK(!invalid.Start(100ms));
}

void TestLostTrackingAndCaptureFailureAreReportedAndKeepTheImage() {
    const Image document = MakeArticle(kWidth, 4000, 43);
    auto screen = std::make_shared<Screen>();
    screen->document = &document;
    ScrollshotSession session(std::make_unique<ScriptedProvider>(screen), MakeOptions());
    CHECK(session.Start());
    ScrollTo(*screen, 120);
    CHECK(WaitForProgress(session, [](const ScrollshotProgress& p) { return p.height == 120 + kView; }));

    // A jump beyond the overlap cannot be placed. It becomes "lost" only after
    // it lasted; the stitched image does not change.
    ScrollTo(*screen, 2000);
    CHECK(WaitForProgress(session, [](const ScrollshotProgress& p) {
        return p.condition == ScrollshotCondition::Lost;
    }));
    CHECK(session.progress().height == 120 + kView);
    // Scrolling back into captured rows recovers.
    ScrollTo(*screen, 300);
    CHECK(WaitForProgress(session, [](const ScrollshotProgress& p) {
        return p.condition == ScrollshotCondition::Following && p.height == 300 + kView;
    }));

    {
        const std::scoped_lock lock(screen->mutex);
        screen->fail = true;
    }
    CHECK(WaitForProgress(session, [](const ScrollshotProgress& p) {
        return p.condition == ScrollshotCondition::CaptureFailed;
    }, 8000ms));
    CHECK(session.progress().height == 300 + kView);
    {
        const std::scoped_lock lock(screen->mutex);
        screen->fail = false;
        screen->scroll = 360;
    }
    CHECK(WaitForProgress(session, [](const ScrollshotProgress& p) {
        return p.condition == ScrollshotCondition::Following && p.height == 360 + kView;
    }));

    CHECK(session.Stop(5000ms));
    const BgraImage result = session.TakeResult();
    CHECK(result.height == 360 + kView);
    CHECK(SameAsDocument(result, document, 0));
}

void TestSizeLimitIsReported() {
    const Image document = MakeArticle(kWidth, 3000, 44);
    auto screen = std::make_shared<Screen>();
    screen->document = &document;
    ScrollshotSession::Options options = MakeOptions();
    options.limits.max_height = 700;
    ScrollshotSession session(std::make_unique<ScriptedProvider>(screen), options);
    CHECK(session.Start());
    bool reached = false;
    for (int scroll = 100; scroll <= 600 && !reached; scroll += 100) {
        ScrollTo(*screen, scroll);
        const int expected = std::min(700, scroll + kView);
        reached = WaitForProgress(session, [expected](const ScrollshotProgress& p) { return p.height == expected; }) &&
                  scroll + kView > 700;
    }
    CHECK(reached);
    CHECK(WaitForProgress(session, [](const ScrollshotProgress& p) {
        return p.condition == ScrollshotCondition::AtLimit;
    }));
    CHECK(session.Stop(5000ms));
    const BgraImage result = session.TakeResult();
    CHECK(result.height == 700);
    CHECK(SameAsDocument(result, document, 0));
}

void TestPollingPathBacksOffAndStopHasADeadline() {
    const Image document = MakeArticle(kWidth, 1500, 45);
    auto screen = std::make_shared<Screen>();
    screen->document = &document;
    screen->report_no_change = false;  // Like a GDI blit: always a picture, changed or not.
    ScrollshotSession session(std::make_unique<ScriptedProvider>(screen), MakeOptions());
    CHECK(session.Start());

    // Identical pictures slow the polling down instead of spinning.
    int before = 0;
    {
        std::unique_lock lock(screen->mutex);
        screen->served.wait_for(lock, 3000ms, [&] { return screen->calls >= 8; });
        before = screen->calls;
    }
    const auto measured_from = std::chrono::steady_clock::now();
    {
        std::unique_lock lock(screen->mutex);
        screen->served.wait_for(lock, 5000ms, [&] { return screen->calls >= before + 5; });
    }
    const auto five_polls = std::chrono::steady_clock::now() - measured_from;
    CHECK(five_polls >= 700ms);  // Four full idle periods of 200 ms, with margin for the first.
    CHECK(session.progress().height == kView);

    // A capture that takes long keeps Stop waiting no longer than its deadline.
    {
        const std::scoped_lock lock(screen->mutex);
        screen->capture_time = 600ms;
    }
    {
        std::unique_lock lock(screen->mutex);
        CHECK(screen->served.wait_for(lock, 3000ms, [&] { return screen->capturing; }));
    }
    const auto stop_started = std::chrono::steady_clock::now();
    const bool stopped_in_time = session.Stop(50ms);
    const auto waited = std::chrono::steady_clock::now() - stop_started;
    CHECK(!stopped_in_time);
    CHECK(waited < 400ms);
    CHECK(!session.progress().finished);
    CHECK(session.Stop(5000ms));
    CHECK(session.progress().finished);
    CHECK(session.TakeResult().height == kView);
}

void TestNotificationsCarryTheGenerationAndAreThrottled() {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = ::DefWindowProcW;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"EasnapScrollshotSessionTest";
    CHECK(::RegisterClassExW(&window_class) != 0);
    HWND window = ::CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    window_class.hInstance, nullptr);
    CHECK(window != nullptr);

    const Image document = MakeArticle(kWidth, 3000, 46);
    auto screen = std::make_shared<Screen>();
    screen->document = &document;
    auto session = std::make_unique<ScrollshotSession>(std::make_unique<ScriptedProvider>(screen),
                                                       MakeOptions(0x1234'5678'9ABCull, window));
    CHECK(session->Start());

    int notifications = 0;
    int wrong_generation = 0;
    int unread = 0;  // Notifications that arrived while an earlier one was unanswered.
    bool answered = true;
    const auto pump = [&](bool answer) {
        MSG message{};
        while (::PeekMessageW(&message, window, kProgressMessage, kProgressMessage, PM_REMOVE) != FALSE) {
            ++notifications;
            if (static_cast<uint64_t>(message.wParam) != 0x1234'5678'9ABCull) { ++wrong_generation; }
            if (!answered) { ++unread; }
            answered = answer;
            if (answer) { static_cast<void>(session->progress()); }
        }
    };

    // While the UI does not read progress, at most one notification is queued.
    for (int scroll = 30; scroll <= 600; scroll += 30) {
        ScrollTo(*screen, scroll);
        std::unique_lock lock(screen->mutex);
        screen->served.wait_for(lock, 2000ms, [&] { return screen->served_scroll == scroll; });
    }
    pump(false);
    CHECK(notifications == 1);
    CHECK(unread == 0);
    static_cast<void>(session->progress());
    answered = true;

    // Reading progress allows the next one; they stay well below one per frame.
    const int frames = 40;
    for (int i = 1; i <= frames; ++i) {
        const int scroll = 600 + i * 20;
        ScrollTo(*screen, scroll);
        std::unique_lock lock(screen->mutex);
        screen->served.wait_for(lock, 2000ms, [&] { return screen->served_scroll == scroll; });
        lock.unlock();
        pump(true);
    }
    CHECK(WaitForProgress(*session, [](const ScrollshotProgress& p) { return p.height == 1400 + kView; }));
    pump(true);
    CHECK(notifications >= 3);
    CHECK(notifications <= frames);
    CHECK(wrong_generation == 0);

    // The final notification announces the finished result.
    const int before_stop = notifications;
    CHECK(session->Stop(5000ms));
    pump(true);
    CHECK(notifications > before_stop);
    CHECK(session->progress().finished);
    session.reset();
    pump(true);
    ::DestroyWindow(window);
}

}  // namespace

int main() {
    TestCaptureStitchesWhatScrollsPast();
    TestStartFailsWhenTheScreenCannotBeRead();
    TestLostTrackingAndCaptureFailureAreReportedAndKeepTheImage();
    TestSizeLimitIsReported();
    TestPollingPathBacksOffAndStopHasADeadline();
    TestNotificationsCarryTheGenerationAndAreThrottled();
    std::printf("scrollshot session: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
