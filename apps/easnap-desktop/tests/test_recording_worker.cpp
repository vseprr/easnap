#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <vector>

#include "capture/recording_capture_worker.h"

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

struct TimelineObservations {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t capture_count{0};
};

class TimelineCaptureProvider final : public easnap::capture::IRegionCaptureProvider {
public:
    explicit TimelineCaptureProvider(std::shared_ptr<TimelineObservations> observations)
        : observations_(std::move(observations)) {}

    bool CaptureDesktop(bool, easnap::capture::DesktopSnapshot&) override { return false; }
    bool CaptureRegion(const RECT&, bool, easnap::platform::BgraImage&) override { return false; }

    easnap::capture::RegionCaptureResult CaptureRegionFrame(
        const RECT&, bool, easnap::media::MediaTime timestamp,
        easnap::media::MediaTime duration, easnap::media::VideoFrame& out) override {
        std::size_t attempt = 0;
        {
            const std::scoped_lock lock(observations_->mutex);
            attempt = observations_->capture_count++;
        }
        observations_->changed.notify_all();
        if ((attempt % 2) != 0) {
            return {easnap::capture::RegionCaptureStatus::NoNewFrame, {3, 0}};
        }

        easnap::platform::BgraImage image;
        image.width = 2;
        image.height = 2;
        image.pixels.assign(4, 0xff102030u);
        out = easnap::media::VideoFrame{
            std::make_shared<const easnap::platform::BgraImage>(std::move(image)),
            timestamp, duration};
        return {easnap::capture::RegionCaptureStatus::FrameAcquired, {2, 4}};
    }

    easnap::capture::RegionCaptureResult CaptureRegionImage(
        const RECT&, const easnap::capture::RegionImageOptions&, easnap::platform::BgraImage&) override {
        return {};  // Unused by the recording worker; a default result is a failure.
    }

    void Release() noexcept override {}
    [[nodiscard]] const wchar_t* name() const override { return L"timeline-test"; }

private:
    std::shared_ptr<TimelineObservations> observations_;
};

class TransientFailureCaptureProvider final : public easnap::capture::IRegionCaptureProvider {
public:
    explicit TransientFailureCaptureProvider(std::shared_ptr<TimelineObservations> observations)
        : observations_(std::move(observations)) {}

    bool CaptureDesktop(bool, easnap::capture::DesktopSnapshot&) override { return false; }
    bool CaptureRegion(const RECT&, bool, easnap::platform::BgraImage&) override { return false; }

    easnap::capture::RegionCaptureResult CaptureRegionFrame(
        const RECT&, bool, easnap::media::MediaTime timestamp,
        easnap::media::MediaTime duration, easnap::media::VideoFrame& out) override {
        std::size_t attempt = 0;
        {
            const std::scoped_lock lock(observations_->mutex);
            attempt = observations_->capture_count++;
        }
        observations_->changed.notify_all();

        if (attempt == 1 || attempt == 2) {
            return {
                easnap::capture::RegionCaptureStatus::Failed,
                {7, 0},
                {easnap::capture::RegionCaptureFailureStage::DuplicationAcquire,
                 easnap::capture::RegionCaptureFailureCodeKind::HResult,
                 0x887A0026u},
                {easnap::capture::RegionCaptureFailureStage::GdiBitBlt,
                 easnap::capture::RegionCaptureFailureCodeKind::Win32, 5u}};
        }
        if (attempt > 3) {
            return {easnap::capture::RegionCaptureStatus::NoNewFrame, {1, 0}};
        }

        easnap::platform::BgraImage image;
        image.width = 2;
        image.height = 2;
        image.pixels.assign(4, attempt == 0 ? 0xff102030u : 0xff405060u);
        out = easnap::media::VideoFrame{
            std::make_shared<const easnap::platform::BgraImage>(std::move(image)),
            timestamp, duration};
        return {easnap::capture::RegionCaptureStatus::FrameAcquired, {2, 4}};
    }

    easnap::capture::RegionCaptureResult CaptureRegionImage(
        const RECT&, const easnap::capture::RegionImageOptions&, easnap::platform::BgraImage&) override {
        return {};  // Unused by the recording worker; a default result is a failure.
    }

    void Release() noexcept override {}
    [[nodiscard]] const wchar_t* name() const override { return L"transient-failure-test"; }

private:
    std::shared_ptr<TimelineObservations> observations_;
};

class AlwaysFailingCaptureProvider final
    : public easnap::capture::IRegionCaptureProvider {
public:
    explicit AlwaysFailingCaptureProvider(
        std::shared_ptr<TimelineObservations> observations, bool throw_on_capture = false)
        : observations_(std::move(observations)), throw_on_capture_(throw_on_capture) {}

    bool CaptureDesktop(bool, easnap::capture::DesktopSnapshot&) override {
        return false;
    }
    bool CaptureRegion(const RECT&, bool,
                       easnap::platform::BgraImage&) override {
        return false;
    }

    easnap::capture::RegionCaptureResult CaptureRegionFrame(
        const RECT&, bool, easnap::media::MediaTime,
        easnap::media::MediaTime, easnap::media::VideoFrame&) override {
        if (throw_on_capture_) throw std::bad_alloc();
        {
            const std::scoped_lock lock(observations_->mutex);
            ++observations_->capture_count;
        }
        observations_->changed.notify_all();
        return {
            easnap::capture::RegionCaptureStatus::Failed,
            {},
            {easnap::capture::RegionCaptureFailureStage::DuplicationMapTexture,
             easnap::capture::RegionCaptureFailureCodeKind::HResult,
             0x8007000Eu},
            {easnap::capture::RegionCaptureFailureStage::GdiCreateDibSection,
             easnap::capture::RegionCaptureFailureCodeKind::Win32, 8u}};
    }

    easnap::capture::RegionCaptureResult CaptureRegionImage(
        const RECT&, const easnap::capture::RegionImageOptions&, easnap::platform::BgraImage&) override {
        return {};  // Unused by the recording worker; a default result is a failure.
    }

    void Release() noexcept override {}
    [[nodiscard]] const wchar_t* name() const override {
        return L"always-failing-test";
    }

private:
    std::shared_ptr<TimelineObservations> observations_;
    bool throw_on_capture_{false};
};

bool WaitForCaptureCount(const std::shared_ptr<TimelineObservations>& observations,
                         std::size_t minimum) {
    using namespace std::chrono_literals;
    std::unique_lock lock(observations->mutex);
    return observations->changed.wait_for(lock, 2s, [&] {
        return observations->capture_count >= minimum;
    });
}

bool WaitForAcceptedFrames(const easnap::capture::RecordingCaptureWorker& worker,
                           uint64_t minimum) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (worker.accepted_frames() < minimum &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return worker.accepted_frames() >= minimum;
}

void TestLifecycleAndCleanStop() {
    easnap::capture::RecordingWorkerLifecycle lifecycle;
    CHECK(lifecycle.state() == easnap::capture::RecordingWorkerState::Stopped);
    CHECK(!lifecycle.Pause());
    CHECK(lifecycle.Start());
    CHECK(!lifecycle.Start());
    CHECK(lifecycle.Pause());
    CHECK(lifecycle.state() == easnap::capture::RecordingWorkerState::Paused);
    CHECK(!lifecycle.Pause());
    CHECK(lifecycle.Resume());
    CHECK(lifecycle.state() == easnap::capture::RecordingWorkerState::Running);
    CHECK(lifecycle.RequestStop());
    CHECK(lifecycle.state() == easnap::capture::RecordingWorkerState::Stopping);
    CHECK(!lifecycle.Resume());
    CHECK(lifecycle.CompleteStop());
    CHECK(lifecycle.state() == easnap::capture::RecordingWorkerState::Stopped);
    CHECK(!lifecycle.CompleteStop());
}

void TestPauseCanStopCleanly() {
    easnap::capture::RecordingWorkerLifecycle lifecycle;
    CHECK(lifecycle.Start());
    CHECK(lifecycle.Pause());
    CHECK(lifecycle.RequestStop());
    CHECK(lifecycle.CompleteStop());
}

void TestMonotonicFrameTimestamps() {
    using easnap::capture::MediaTimestampForFrame;
    CHECK(MediaTimestampForFrame(0, 30).count() == 0);
    CHECK(MediaTimestampForFrame(1, 30).count() == 333'333);
    CHECK(MediaTimestampForFrame(2, 30).count() == 666'666);
    CHECK(MediaTimestampForFrame(3, 30).count() == 1'000'000);
    CHECK(MediaTimestampForFrame(30, 30).count() == 10'000'000);
    CHECK(MediaTimestampForFrame(31, 30) > MediaTimestampForFrame(30, 30));
}

void TestDropNewestOverloadPolicy() {
    easnap::media::FrameQueue<easnap::media::VideoFrame> queue(1);
    easnap::capture::SaturatingCounter drops(2);

    easnap::media::VideoFrame first;
    first.timestamp = easnap::media::MediaTime{1};
    CHECK(easnap::capture::TrySubmitVideoFrameOrDropNewest(queue, std::move(first), drops));

    easnap::media::VideoFrame newest;
    newest.timestamp = easnap::media::MediaTime{2};
    CHECK(!easnap::capture::TrySubmitVideoFrameOrDropNewest(queue, std::move(newest), drops));
    CHECK(drops.value() == 1);
    CHECK(newest.timestamp.count() == 2);

    drops.Add(10);
    CHECK(drops.value() == 2);
    drops.Reset();
    CHECK(drops.value() == 0);
    auto retained = queue.try_pop();
    CHECK(retained.has_value());
    CHECK(retained->timestamp.count() == 1);
}

void TestPauseExcludesWallClockFromTimeline() {
    using namespace std::chrono_literals;

    auto observations = std::make_shared<TimelineObservations>();
    auto provider = std::make_unique<TimelineCaptureProvider>(observations);
    easnap::media::FrameQueue<easnap::media::VideoFrame> queue(64);
    easnap::media::RecordingOptions options;
    options.region = RECT{0, 0, 2, 2};
    options.include_cursor = false;
    options.target_frame_rate = 30;

    easnap::capture::RecordingCaptureWorker worker(std::move(provider), options, queue);
    CHECK(worker.Start());
    CHECK(WaitForCaptureCount(observations, 4));
    // A provider reports capture entry before the worker submits that frame.
    // Pause may correctly discard the in-flight fourth frame; wait for the
    // production acceptance boundary before asserting four frames per segment.
    CHECK(WaitForAcceptedFrames(worker, 4));
    CHECK(worker.Pause());
    const uint64_t accepted_before_pause = worker.accepted_frames();
    std::size_t captures_before_resume = 0;
    {
        const std::scoped_lock lock(observations->mutex);
        captures_before_resume = observations->capture_count;
    }
    std::this_thread::sleep_for(250ms);
    CHECK(worker.accepted_frames() == accepted_before_pause);
    CHECK(worker.Resume());
    CHECK(WaitForCaptureCount(observations,
                              captures_before_resume + 4));
    CHECK(WaitForAcceptedFrames(worker, accepted_before_pause + 4));
    worker.Stop();

    std::vector<easnap::media::VideoFrame> frames;
    while (auto frame = queue.try_pop()) {
        frames.push_back(std::move(*frame));
    }
    CHECK(frames.size() == worker.accepted_frames());
    CHECK(frames.size() >= 8);
    easnap::media::MediaTime maximum_gap{0};
    bool observed_shared_reuse = false;
    for (std::size_t index = 1; index < frames.size(); ++index) {
        CHECK(frames[index].timestamp > frames[index - 1].timestamp);
        observed_shared_reuse = observed_shared_reuse ||
                                frames[index].image == frames[index - 1].image;
        maximum_gap = (std::max)(maximum_gap,
                                 frames[index].timestamp - frames[index - 1].timestamp);
    }
    CHECK(observed_shared_reuse);
    CHECK(maximum_gap <= easnap::media::MediaTime{666'667});
    CHECK(worker.capture_attempts() == worker.acquired_frames() + worker.no_new_frames() +
                                           worker.capture_failures());
    CHECK(worker.no_new_frames() > 0);
    CHECK(worker.reused_frames() > 0);
    CHECK(worker.queue_full_rejections() == 0);
    CHECK(worker.pacing_slots_missed() == 0);
    CHECK(worker.capture_attempt_timing().count == worker.capture_attempts());
    CHECK(worker.acquisition_timing().count == worker.capture_attempts());
    CHECK(worker.region_copy_timing().count == worker.capture_attempts());
    CHECK(worker.queue_submission_timing().count == worker.accepted_frames());
    CHECK(worker.active_duration() < easnap::media::MediaTime{5'000'000});
    CHECK(worker.state() == easnap::capture::RecordingWorkerState::Stopped);
}

void TestStartupFailureReleasesBarrier() {
    auto observations = std::make_shared<TimelineObservations>();
    auto provider = std::make_unique<AlwaysFailingCaptureProvider>(observations, true);
    easnap::media::FrameQueue<easnap::media::VideoFrame> queue(4);
    easnap::media::RecordingOptions options;
    options.region = {0, 0, 2, 2};
    easnap::capture::RecordingCaptureWorker worker(std::move(provider), options, queue);
    CHECK(!worker.Start());
    CHECK(worker.failed());
    worker.Stop();
    CHECK(worker.state() == easnap::capture::RecordingWorkerState::Stopped);
    CHECK(queue.size() == 0);
}

void TestRapidPauseKeepsFramesInsideActiveTimeline() {
    using namespace std::chrono_literals;
    auto observations = std::make_shared<TimelineObservations>();
    auto provider = std::make_unique<TimelineCaptureProvider>(observations);
    easnap::media::FrameQueue<easnap::media::VideoFrame> queue(64);
    easnap::media::RecordingOptions options;
    options.region = {0, 0, 2, 2};
    options.target_frame_rate = 15;
    easnap::capture::RecordingCaptureWorker worker(std::move(provider), options, queue);
    CHECK(worker.Start());
    CHECK(worker.accepted_frames() > 0);  // first capture is published before Start returns
    CHECK(worker.Pause());               // immediate Pause/Stop can finalize a real first frame
    for (int segment = 0; segment < 24; ++segment) {
        std::this_thread::sleep_for(3ms);
        CHECK(worker.Resume());
        std::this_thread::sleep_for(5ms);
        CHECK(worker.Pause());
    }
    const auto endpoint = worker.active_duration();
    worker.Stop();
    uint64_t count = 0;
    easnap::media::MediaTime previous{-1};
    while (auto frame = queue.try_pop()) {
        CHECK(frame->timestamp > previous);
        CHECK(frame->timestamp < endpoint);
        previous = frame->timestamp;
        ++count;
    }
    CHECK(count == worker.accepted_frames());
    CHECK(count > 0);
    CHECK(worker.queue_full_rejections() == 0);
}

void TestTransientCaptureFailuresReuseLatestFrame() {
    auto observations = std::make_shared<TimelineObservations>();
    auto provider = std::make_unique<TransientFailureCaptureProvider>(observations);
    constexpr std::size_t kQueueCapacity = 4;
    easnap::media::FrameQueue<easnap::media::VideoFrame> queue(kQueueCapacity);
    easnap::media::RecordingOptions options;
    options.region = RECT{0, 0, 2, 2};
    options.include_cursor = false;
    options.target_frame_rate = 10;

    easnap::capture::RecordingCaptureWorker worker(std::move(provider), options, queue);
    CHECK(worker.Start());
    CHECK(WaitForCaptureCount(observations, 4));
    CHECK(WaitForAcceptedFrames(worker, kQueueCapacity));
    worker.Stop();

    CHECK(worker.acquired_frames() == 2);
    CHECK(worker.capture_failures() == 2);
    CHECK(worker.failure_reused_frames() == 2);
    CHECK(worker.reused_frames() >= worker.failure_reused_frames());
    const auto failure_diagnostics = worker.capture_failure_diagnostics();
    CHECK(failure_diagnostics.count == 2);
    CHECK(failure_diagnostics.omitted == 0);
    CHECK(failure_diagnostics.count <=
          easnap::capture::kCaptureFailureDiagnosticCapacity);
    for (std::size_t index = 0; index < failure_diagnostics.count; ++index) {
        const auto& diagnostic = failure_diagnostics.entries[index];
        CHECK(diagnostic.capture_attempt == index + 2);
        CHECK(diagnostic.timestamp == easnap::capture::MediaTimestampForFrame(
                                          static_cast<uint64_t>(index + 1),
                                          options.target_frame_rate));
        CHECK(diagnostic.primary_failure.stage ==
              easnap::capture::RegionCaptureFailureStage::DuplicationAcquire);
        CHECK(diagnostic.primary_failure.code_kind ==
              easnap::capture::RegionCaptureFailureCodeKind::HResult);
        CHECK(diagnostic.primary_failure.native_code == 0x887A0026u);
        CHECK(diagnostic.fallback_failure.stage ==
              easnap::capture::RegionCaptureFailureStage::GdiBitBlt);
        CHECK(diagnostic.fallback_failure.code_kind ==
              easnap::capture::RegionCaptureFailureCodeKind::Win32);
        CHECK(diagnostic.fallback_failure.native_code == 5u);
    }
    CHECK(queue.capacity() == kQueueCapacity);
    CHECK(queue.peak_size() <= queue.capacity());

    std::vector<easnap::media::VideoFrame> frames;
    while (auto frame = queue.try_pop()) {
        frames.push_back(std::move(*frame));
    }
    CHECK(frames.size() == kQueueCapacity);
    if (frames.size() == kQueueCapacity) {
        for (std::size_t index = 0; index < frames.size(); ++index) {
            const auto expected_timestamp = easnap::capture::MediaTimestampForFrame(
                static_cast<uint64_t>(index), options.target_frame_rate);
            const auto expected_next_timestamp = easnap::capture::MediaTimestampForFrame(
                static_cast<uint64_t>(index + 1), options.target_frame_rate);
            CHECK(frames[index].timestamp == expected_timestamp);
            CHECK(frames[index].duration == expected_next_timestamp - expected_timestamp);
        }
        CHECK(frames[0].image != nullptr);
        CHECK(frames[1].image == frames[0].image);
        CHECK(frames[2].image == frames[0].image);
        CHECK(frames[3].image != frames[0].image);
    }
}

void TestCaptureFailureDiagnosticsRemainBounded() {
    auto observations = std::make_shared<TimelineObservations>();
    auto provider =
        std::make_unique<AlwaysFailingCaptureProvider>(observations);
    easnap::media::FrameQueue<easnap::media::VideoFrame> queue(1);
    easnap::media::RecordingOptions options;
    options.region = RECT{0, 0, 2, 2};
    options.include_cursor = false;
    options.target_frame_rate = easnap::capture::kMaximumTargetFrameRate;

    easnap::capture::RecordingCaptureWorker worker(std::move(provider), options,
                                                    queue);
    constexpr std::size_t kRequestedFailures =
        easnap::capture::kCaptureFailureDiagnosticCapacity + 3;
    CHECK(worker.Start());
    CHECK(WaitForCaptureCount(observations, kRequestedFailures));
    worker.Stop();

    const uint64_t failure_count = worker.capture_failures();
    const auto diagnostics = worker.capture_failure_diagnostics();
    CHECK(failure_count >= kRequestedFailures);
    CHECK(diagnostics.count ==
          easnap::capture::kCaptureFailureDiagnosticCapacity);
    CHECK(diagnostics.omitted == failure_count - diagnostics.count);
    CHECK(queue.size() == 0);
    for (std::size_t index = 0; index < diagnostics.count; ++index) {
        const auto& entry = diagnostics.entries[index];
        CHECK(entry.capture_attempt == index + 1);
        if (index == 0) {
            CHECK(entry.timestamp == easnap::media::MediaTime::zero());
        } else {
            CHECK(entry.timestamp > diagnostics.entries[index - 1].timestamp);
        }
        CHECK(entry.primary_failure.stage ==
              easnap::capture::RegionCaptureFailureStage::DuplicationMapTexture);
        CHECK(entry.fallback_failure.stage ==
              easnap::capture::RegionCaptureFailureStage::GdiCreateDibSection);
    }
}

}  // namespace

int main() {
    TestLifecycleAndCleanStop();
    TestPauseCanStopCleanly();
    TestMonotonicFrameTimestamps();
    TestDropNewestOverloadPolicy();
    TestPauseExcludesWallClockFromTimeline();
    TestRapidPauseKeepsFramesInsideActiveTimeline();
    TestStartupFailureReleasesBarrier();
    TestTransientCaptureFailuresReuseLatestFrame();
    TestCaptureFailureDiagnosticsRemainBounded();
    std::printf("QUEUE_LAYOUT object=%zu slot=%zu video_frame=%zu\n",
                sizeof(easnap::media::FrameQueue<easnap::media::VideoFrame>),
                sizeof(std::optional<easnap::media::VideoFrame>),
                sizeof(easnap::media::VideoFrame));
    if (g_fail == 0) {
        std::printf("PASSED  %d/%d tests\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
