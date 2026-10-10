// Easnap unit tests - trim range and asynchronous export orchestration.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "app/trim_workflow.h"
#include "media/preview_transport.h"
#include "ui/trim_layout.h"

using easnap::app::SessionState;
using easnap::app::TrimExportController;
using easnap::app::TrimExportRequest;
using easnap::app::TrimExportStatus;
using easnap::app::TrimRangeModel;
using easnap::media::MediaTime;
using easnap::media::PreviewTransportCommandType;
using easnap::media::PreviewNativeState;

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

struct ExportProbe final {
    std::mutex mutex;
    std::condition_variable changed;
    bool started{false};
    bool release{false};
    bool block{false};
    bool fail{false};
    int calls{0};
    std::wstring source;
    std::wstring destination;
    MediaTime start{0};
    MediaTime end{0};
    easnap::media::RecordingFormat format{
        easnap::media::RecordingFormat::Mp4};
    bool replace_existing{false};
    easnap::media::TrimExportFailure failure;
};

class ControlledExporter final : public easnap::media::ITrimExporter {
public:
    explicit ControlledExporter(std::shared_ptr<ExportProbe> probe)
        : probe_(std::move(probe)) {}

    bool Export(const std::wstring& source_path,
                const std::wstring& destination_path, MediaTime start,
                MediaTime end, const easnap::media::RecordingOptions& options,
                ProgressCallback progress, bool replace_existing = false) override {
        {
            const std::scoped_lock lock(probe_->mutex);
            ++probe_->calls;
            probe_->source = source_path;
            probe_->destination = destination_path;
            probe_->start = start;
            probe_->end = end;
            probe_->format = options.format;
            probe_->replace_existing = replace_existing;
            probe_->started = true;
        }
        probe_->changed.notify_all();
        if (!progress(0.25f)) {
            return false;
        }

        if (probe_->block) {
            std::unique_lock lock(probe_->mutex);
            while (!probe_->release) {
                probe_->changed.wait_for(lock, std::chrono::milliseconds(2));
                lock.unlock();
                const bool keep_running = progress(0.5f);
                lock.lock();
                if (!keep_running) {
                    return false;
                }
            }
        }
        if (probe_->fail) {
            return false;
        }
        return progress(1.0f);
    }

    std::optional<uint64_t> EstimateOutputBytes(
        const std::wstring&, MediaTime, MediaTime,
        const easnap::media::RecordingOptions&) const override {
        return std::nullopt;
    }

    [[nodiscard]] easnap::media::TrimExportFailure LastFailure()
        const override {
        const std::scoped_lock lock(probe_->mutex);
        return probe_->failure;
    }

private:
    std::shared_ptr<ExportProbe> probe_;
};

TrimExportRequest ValidRequest() {
    TrimExportRequest request;
    request.source_path = L"C:\\temp\\source.mp4";
    request.destination_path = L"C:\\temp\\trimmed.mp4";
    request.start = MediaTime{10};
    request.end = MediaTime{90};
    return request;
}

void WaitUntilStarted(const std::shared_ptr<ExportProbe>& probe) {
    std::unique_lock lock(probe->mutex);
    probe->changed.wait_for(lock, std::chrono::seconds(2),
                            [&] { return probe->started; });
}

void Release(const std::shared_ptr<ExportProbe>& probe) {
    {
        const std::scoped_lock lock(probe->mutex);
        probe->release = true;
    }
    probe->changed.notify_all();
}

void TestRangeInitializationAndValidation() {
    TrimRangeModel range;
    CHECK(!range.Initialize(MediaTime::zero()));
    CHECK(!range.valid());
    CHECK(range.Initialize(MediaTime{1'000}));
    CHECK(range.valid());
    CHECK(range.in() == MediaTime::zero());
    CHECK(range.out() == MediaTime{1'000});

    CHECK(range.SetIn(MediaTime{250}));
    CHECK(range.SetOut(MediaTime{800}));
    CHECK(range.in() == MediaTime{250});
    CHECK(range.out() == MediaTime{800});
    CHECK(!range.SetIn(MediaTime{800}));
    CHECK(!range.SetIn(MediaTime{-1}));
    CHECK(!range.SetOut(MediaTime{250}));
    CHECK(!range.SetOut(MediaTime{1'001}));
    CHECK(range.valid());
}

void TestRangeClampingAndTimelineMapping() {
    TrimRangeModel range;
    CHECK(range.Initialize(MediaTime{1'000}));
    range.SetCurrent(MediaTime{-50});
    CHECK(range.current() == MediaTime::zero());
    range.SetCurrent(MediaTime{2'000});
    CHECK(range.current() == MediaTime{1'000});

    CHECK(range.TimeFromTimelineOffset(0, 100) == MediaTime::zero());
    CHECK(range.TimeFromTimelineOffset(25, 100) == MediaTime{250});
    CHECK(range.TimeFromTimelineOffset(100, 100) == MediaTime{1'000});
    CHECK(range.TimeFromTimelineOffset(120, 100) == MediaTime{1'000});
    CHECK(range.TimeFromTimelineOffset(20, 0) == MediaTime::zero());
    CHECK(range.TimelineOffsetFromTime(MediaTime::zero(), 100) == 0);
    CHECK(range.TimelineOffsetFromTime(MediaTime{250}, 100) == 25);
    CHECK(range.TimelineOffsetFromTime(MediaTime{1'000}, 100) == 100);
    CHECK(range.TimelineOffsetFromTime(MediaTime{2'000}, 100) == 100);
    CHECK(range.TimelineOffsetFromTime(MediaTime{250}, 0) == 0);
}

void TestRecordingFinalizationPolicy() {
    CHECK(easnap::app::StateAfterRecordingFinalization(true, true) ==
          SessionState::RecordingTrimming);
    CHECK(easnap::app::StateAfterRecordingFinalization(false, true) ==
          SessionState::Idle);
    CHECK(easnap::app::StateAfterRecordingFinalization(true, false) ==
          SessionState::Idle);
    CHECK(easnap::app::IsSessionTransitionLegal(
        SessionState::RecordingTrimming, SessionState::Idle));
}

void TestPlaybackRestartsAndRangeReconciliation() {
    using easnap::app::TrimPlaybackAction;

    auto decision = easnap::app::PlanTrimPlaybackToggle(
        false, true, MediaTime{900}, MediaTime{200}, MediaTime{900});
    CHECK(decision.action == TrimPlaybackAction::SeekThenPlay);
    CHECK(decision.position == MediaTime{200});

    decision = easnap::app::PlanTrimPlaybackToggle(
        false, false, MediaTime{900}, MediaTime{200}, MediaTime{900});
    CHECK(decision.action == TrimPlaybackAction::SeekThenPlay);
    CHECK(decision.position == MediaTime{200});

    CHECK(easnap::app::PlanTrimPlaybackToggle(
              true, false, MediaTime{500}, MediaTime{200}, MediaTime{900})
              .action == TrimPlaybackAction::Pause);
    CHECK(easnap::app::PlanTrimPlaybackToggle(
              false, false, MediaTime{500}, MediaTime{200}, MediaTime{900})
              .action == TrimPlaybackAction::Play);

    decision = easnap::app::PlanTrimRangeEdit(
        MediaTime{700}, MediaTime{100}, MediaTime{500});
    CHECK(decision.action == TrimPlaybackAction::PauseAt);
    CHECK(decision.position == MediaTime{500});

    decision = easnap::app::PlanTrimRangeEdit(
        MediaTime{300}, MediaTime{450}, MediaTime{800});
    CHECK(decision.action == TrimPlaybackAction::Seek);
    CHECK(decision.position == MediaTime{450});

    decision = easnap::app::PlanTrimPlaybackProgress(
        true, MediaTime{500}, MediaTime{500});
    CHECK(decision.action == TrimPlaybackAction::PauseAt);
    CHECK(decision.position == MediaTime{500});
    CHECK(easnap::app::PlanTrimPlaybackProgress(
              false, MediaTime{500}, MediaTime{500})
              .action == TrimPlaybackAction::None);
}

void TestActiveTimelineSeekPolicy() {
    const MediaTime in{300};
    const MediaTime out{700};
    MediaTime current{500};
    bool play_requested = true;

    auto planned = easnap::app::PlanTrimTimelineSeek(
        play_requested, MediaTime{600}, in, out);
    CHECK(planned.has_value());
    if (planned.has_value()) {
        current = *planned;
    }
    CHECK(current == MediaTime{600});
    CHECK(play_requested);

    planned = easnap::app::PlanTrimTimelineSeek(
        play_requested, MediaTime{200}, in, out);
    CHECK(!planned.has_value());
    CHECK(current == MediaTime{600});
    CHECK(play_requested);

    planned = easnap::app::PlanTrimTimelineSeek(
        play_requested, out, in, out);
    CHECK(!planned.has_value());
    CHECK(current == MediaTime{600});
    CHECK(play_requested);

    planned = easnap::app::PlanTrimTimelineSeek(
        play_requested, MediaTime{900}, in, out);
    CHECK(!planned.has_value());
    CHECK(current == MediaTime{600});
    CHECK(play_requested);

    CHECK(easnap::app::PlanTrimTimelineSeek(
              false, MediaTime{200}, in, out)
              .has_value());
    CHECK(easnap::app::IsFullTrimRange(
        MediaTime::zero(), MediaTime{1000}, MediaTime{1000}));
    CHECK(!easnap::app::IsFullTrimRange(
        MediaTime{1}, MediaTime{1000}, MediaTime{1000}));
    CHECK(!easnap::app::IsFullTrimRange(
        MediaTime::zero(), MediaTime{999}, MediaTime{1000}));
}

void TestTerminalActionMediaPlanning() {
    using easnap::app::PlanTrimMediaPreparation;
    using easnap::app::TrimMediaPreparation;
    using easnap::app::TrimTerminalAction;
    const MediaTime duration{1000};

    CHECK(PlanTrimMediaPreparation(
              TrimTerminalAction::Save, MediaTime::zero(), duration,
              duration, true, false) ==
          TrimMediaPreparation::ReuseFinalizedSource);
    CHECK(PlanTrimMediaPreparation(
              TrimTerminalAction::Copy, MediaTime::zero(), duration,
              duration, true, false) ==
          TrimMediaPreparation::ReuseFinalizedSource);
    CHECK(PlanTrimMediaPreparation(
              TrimTerminalAction::Save, MediaTime{100}, MediaTime{900},
              duration, true, false) == TrimMediaPreparation::Export);
    CHECK(PlanTrimMediaPreparation(
              TrimTerminalAction::Copy, MediaTime{100}, MediaTime{900},
              duration, true, false) == TrimMediaPreparation::Export);
    CHECK(PlanTrimMediaPreparation(
              TrimTerminalAction::Save, MediaTime{100}, MediaTime{900},
              duration, true, true) ==
          TrimMediaPreparation::ReuseTrimArtifact);
    CHECK(PlanTrimMediaPreparation(
              TrimTerminalAction::Save, MediaTime::zero(), duration,
              duration, true, false, easnap::media::RecordingFormat::Gif) ==
          TrimMediaPreparation::Export);
    CHECK(PlanTrimMediaPreparation(
              TrimTerminalAction::Copy, MediaTime::zero(), duration,
              duration, true, false, easnap::media::RecordingFormat::Gif) ==
          TrimMediaPreparation::Export);
    CHECK(PlanTrimMediaPreparation(
              TrimTerminalAction::Copy, MediaTime{100}, MediaTime{900},
              duration, true, true, easnap::media::RecordingFormat::Gif) ==
          TrimMediaPreparation::ReuseTrimArtifact);
}

void TestPreviewTransportSerializesAndCoalescesSeeks() {
    easnap::media::PreviewTransport transport;
    transport.Reset(false);
    transport.RequestPlayFrom(MediaTime{100});
    auto command = transport.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{100});

    transport.RequestPlayFrom(MediaTime{300});
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(transport.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    command = transport.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{300});

    // A stale/unmatched callback cannot release the active seek.
    CHECK(!transport.Complete(PreviewTransportCommandType::Play, PreviewNativeState::Paused));
    CHECK(transport.in_flight() == PreviewTransportCommandType::Seek);
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(transport.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(transport.Complete(PreviewTransportCommandType::Play, PreviewNativeState::Playing));
    CHECK(transport.playing());
    CHECK(!transport.command_pending());
}

void TestPreviewOrderingRegressions() {
    using easnap::media::PreviewTransport;
    // A STOP is a state observation, not the completion of our pending Pause.
    PreviewTransport stopped;
    stopped.Reset(true);
    stopped.RequestPauseAt(MediaTime{900});
    CHECK(stopped.TakeNext().type == PreviewTransportCommandType::Pause);
    stopped.Ended(PreviewNativeState::Stopped);
    CHECK(stopped.in_flight() == PreviewTransportCommandType::Pause);

    // A failed old Pause is still benign when the user now wants Play.
    PreviewTransport toggled;
    toggled.Reset(true);
    toggled.RequestPause();
    CHECK(toggled.TakeNext().type == PreviewTransportCommandType::Pause);
    toggled.RequestPlayFrom(MediaTime{200});
    CHECK(toggled.ReconcilePauseWhileStopped());
    CHECK(toggled.play_requested());
    CHECK(toggled.TakeNext().type == PreviewTransportCommandType::Seek);

    // An unowned Play event during a paused seek must not invent playback.
    PreviewTransport stale;
    stale.RequestPlayFrom(MediaTime{200});
    CHECK(stale.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(!stale.Complete(PreviewTransportCommandType::Play, PreviewNativeState::Paused));
    CHECK(!stale.playing());
    CHECK(stale.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(stale.TakeNext().type == PreviewTransportCommandType::Play);

    // This is the old TrimWindow timer boundary: after TakeNext consumes the
    // In target, only the old native Out position is visible to progress policy.
    PreviewTransport timer;
    timer.RequestPlayFrom(MediaTime{200});
    CHECK(timer.TakeNext().type == PreviewTransportCommandType::Seek);
    const MediaTime displayed = timer.ResolvePosition(MediaTime{900});
    CHECK(displayed == MediaTime{200});
    CHECK(easnap::app::PlanTrimPlaybackProgress(timer.play_requested(), displayed,
                                               MediaTime{900}).action ==
          easnap::app::TrimPlaybackAction::None);

    // The second click must cancel Play even while the old Ended/Out snapshot
    // is still visible; otherwise rapid toggles are not last-intent-wins.
    CHECK(easnap::app::PlanTrimPlaybackToggle(
              true, true, MediaTime{900}, MediaTime{200}, MediaTime{900})
              .action == easnap::app::TrimPlaybackAction::Pause);
}

void TestPreviewCallbackPermutations() {
    using easnap::media::PreviewTransport;
    // Move old STOP/ENDED across Pause completion, restart intent, In seek and
    // Play completion. Native state is independent of callback delivery order.
    for (unsigned terminal_slot = 0; terminal_slot < 5; ++terminal_slot) {
        for (bool last_play : {false, true}) {
            for (bool pause_failed_stopped : {false, true}) {
                PreviewTransport transport;
                transport.Reset(true);
                transport.RequestPauseAt(MediaTime{900});
                CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);
                if (terminal_slot == 0) transport.Ended(PreviewNativeState::Stopped);
                transport.RequestPlayFrom(MediaTime{200});
                transport.RequestPause();
                transport.RequestPlay();
                if (terminal_slot == 1) transport.Ended(PreviewNativeState::Stopped);
                if (pause_failed_stopped) {
                    CHECK(transport.ReconcilePauseWhileStopped());
                } else {
                    CHECK(transport.Complete(PreviewTransportCommandType::Pause,
                                             PreviewNativeState::Paused));
                }
                if (terminal_slot == 2) transport.Ended(PreviewNativeState::Stopped);
                auto command = transport.TakeNext();
                CHECK(command.type == PreviewTransportCommandType::Seek);
                CHECK(command.position == MediaTime{200});
                CHECK(!transport.Complete(PreviewTransportCommandType::Play,
                                          PreviewNativeState::Paused));
                CHECK(!transport.playing());
                CHECK(!transport.Complete(PreviewTransportCommandType::Pause,
                                          PreviewNativeState::Paused));
                CHECK(transport.in_flight() == PreviewTransportCommandType::Seek);
                CHECK(transport.ResolvePosition(MediaTime{900}) == MediaTime{200});
                transport.RequestPause();
                transport.RequestPlay();
                if (!last_play) transport.RequestPause();
                if (terminal_slot == 3) transport.Ended(PreviewNativeState::Stopped);
                CHECK(transport.Complete(PreviewTransportCommandType::Seek,
                                         PreviewNativeState::Paused));
                if (last_play) {
                    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
                    // A historical PAUSE cannot claim the newer native Play is
                    // paused, or complete that Play's callback slot.
                    CHECK(!transport.Complete(PreviewTransportCommandType::Pause,
                                              PreviewNativeState::Playing));
                    CHECK(transport.in_flight() == PreviewTransportCommandType::Play);
                    CHECK(transport.Complete(PreviewTransportCommandType::Play,
                                             PreviewNativeState::Playing));
                }
                if (terminal_slot == 4) transport.Ended(last_play
                    ? PreviewNativeState::Playing : PreviewNativeState::Paused);
                CHECK(transport.playing() == last_play);
                CHECK(transport.play_requested() == last_play);
                CHECK(!transport.command_pending());
                CHECK(transport.in_flight() == PreviewTransportCommandType::None);
            }
        }
    }

    PreviewTransport stopped_seek;
    stopped_seek.RequestPlayFrom(MediaTime{200});
    CHECK(stopped_seek.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(stopped_seek.Complete(PreviewTransportCommandType::Seek,
                                PreviewNativeState::Stopped));
    CHECK(stopped_seek.ResolvePosition(MediaTime{900}) == MediaTime{200});
    CHECK(stopped_seek.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(stopped_seek.Complete(PreviewTransportCommandType::Play,
                                PreviewNativeState::Playing));
    CHECK(stopped_seek.ResolvePosition(MediaTime{215}) == MediaTime{215});
    CHECK(!stopped_seek.ReconcilePauseWhileStopped()); // not a global ignore
    stopped_seek.Failed();
    CHECK(!stopped_seek.command_pending());
    CHECK(!stopped_seek.play_requested());

    PreviewTransport paused_endpoint;
    paused_endpoint.RequestPauseAt(MediaTime{900});
    CHECK(paused_endpoint.TakeNext().type == PreviewTransportCommandType::Seek);
    paused_endpoint.Ended(PreviewNativeState::Stopped);
    CHECK(paused_endpoint.Complete(PreviewTransportCommandType::Seek,
                                   PreviewNativeState::Paused));
    CHECK(!paused_endpoint.command_pending());
    CHECK(paused_endpoint.ResolvePosition(MediaTime::zero()) == MediaTime{900});
}

void TestGeneratedPreviewOrderings() {
    using easnap::media::PreviewTransport;
    using easnap::media::PreviewTransportCommand;
    constexpr unsigned seeds = 512;
    for (unsigned seed = 1; seed <= seeds; ++seed) {
        uint32_t random = seed;
        const auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
        PreviewTransport transport;
        PreviewTransportCommand outstanding;
        PreviewNativeState native = PreviewNativeState::Paused;
        MediaTime native_position{500};
        MediaTime last_position{500};
        bool wanted = false;
        const auto dispatch = [&] {
            const auto command = transport.TakeNext();
            if (outstanding.type != PreviewTransportCommandType::None) {
                CHECK(command.type == PreviewTransportCommandType::None);
            } else if (command.type != PreviewTransportCommandType::None) {
                if (command.type == PreviewTransportCommandType::Seek) {
                    CHECK(native != PreviewNativeState::Playing);
                }
                outstanding = command;
            }
        };
        const auto complete = [&] {
            if (outstanding.type == PreviewTransportCommandType::None) return;
            if (outstanding.type == PreviewTransportCommandType::Seek) {
                native_position = outstanding.position;
            } else {
                native = outstanding.type == PreviewTransportCommandType::Play
                    ? PreviewNativeState::Playing : PreviewNativeState::Paused;
            }
            CHECK(transport.Complete(outstanding.type, native));
            outstanding = {};
        };
        for (unsigned step = 0; step < 128; ++step) {
            switch ((next() >> 24) % 8) {
                case 0: wanted = true; transport.RequestPlay(); break;
                case 1: wanted = false; transport.RequestPause(); break;
                case 2:
                    last_position = MediaTime{100 + static_cast<int64_t>(next() % 700)};
                    transport.RequestSeek(last_position);
                    break;
                case 3:
                    wanted = true;
                    last_position = MediaTime{200};
                    transport.RequestPlayFrom(last_position);
                    break;
                case 4:
                    wanted = false;
                    last_position = MediaTime{900};
                    transport.RequestPauseAt(last_position);
                    break;
                case 5: complete(); break;
                case 6:
                    if (outstanding.type == PreviewTransportCommandType::Pause) {
                        native = PreviewNativeState::Stopped;
                        transport.Ended(native);
                        CHECK(transport.in_flight() == PreviewTransportCommandType::Pause);
                        CHECK(transport.ReconcilePauseWhileStopped());
                        outstanding = {};
                    }
                    break;
                case 7:
                    // Unmatched historical callbacks are observations only.
                    if (outstanding.type == PreviewTransportCommandType::Seek) {
                        CHECK(!transport.Complete(PreviewTransportCommandType::Play, native));
                        CHECK(!transport.Complete(PreviewTransportCommandType::Pause, native));
                    }
                    break;
            }
            CHECK(transport.play_requested() == wanted);
            dispatch();
        }
        // Fair delivery: one completion per issued call. With no more user
        // requests, at most Pause + final Seek + Play can remain.
        for (unsigned drain = 0; drain < 4; ++drain) { complete(); dispatch(); }
        CHECK(outstanding.type == PreviewTransportCommandType::None);
        CHECK(!transport.command_pending());
        CHECK(transport.playing() == wanted);
        CHECK(transport.play_requested() == wanted);
        CHECK(native_position == last_position);
    }
    std::printf("Preview generated orderings: %u seeds x 128 steps\n", seeds);
}

void TestPreviewLogicalEndWaitsForRealSeekCompletion() {
    using easnap::media::PreviewTransport;
    PreviewTransport endpoint;
    endpoint.Reset(true);
    endpoint.RequestPauseAt(MediaTime{900});
    CHECK(endpoint.position_pending());
    CHECK(endpoint.position_override() == MediaTime{900});
    CHECK(endpoint.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(endpoint.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(endpoint.Complete(PreviewTransportCommandType::Pause,
                            PreviewNativeState::Paused));
    auto command = endpoint.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{900});
    // The adapter uses the measured last video sample as its native target.
    // Logical Out must retain ownership until that actual seek completes.
    CHECK(endpoint.ResolvePosition(MediaTime{880}) == MediaTime{900});
    CHECK(endpoint.position_pending());
    CHECK(endpoint.command_pending());
    CHECK(endpoint.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(!endpoint.Complete(PreviewTransportCommandType::Pause,
                             PreviewNativeState::Paused));
    CHECK(endpoint.in_flight() == PreviewTransportCommandType::Seek);
    CHECK(endpoint.Complete(PreviewTransportCommandType::Seek,
                            PreviewNativeState::Paused));
    CHECK(endpoint.in_flight() == PreviewTransportCommandType::None);
    CHECK(!endpoint.play_requested());
    CHECK(!endpoint.command_pending());
    CHECK(!endpoint.position_pending());
    CHECK(endpoint.ResolvePosition(MediaTime{880}) == MediaTime{900});

    endpoint.RequestPlayFrom(MediaTime{200});
    command = endpoint.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{200});
    CHECK(endpoint.Complete(PreviewTransportCommandType::Seek,
                            PreviewNativeState::Paused));
    CHECK(endpoint.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(endpoint.Complete(PreviewTransportCommandType::Play,
                            PreviewNativeState::Playing));
    CHECK(!endpoint.ended());
    CHECK(!endpoint.position_override());

    // An endpoint request cannot retire an unrelated owned Seek, and newest
    // restart intent can supersede it while the endpoint Seek is pending.
    endpoint.RequestSeek(MediaTime{450});
    CHECK(endpoint.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(endpoint.play_requested());
    CHECK(endpoint.Complete(PreviewTransportCommandType::Pause,
                            PreviewNativeState::Paused));
    CHECK(endpoint.TakeNext().type == PreviewTransportCommandType::Seek);
    endpoint.RequestPauseAt(MediaTime{900});
    CHECK(endpoint.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(endpoint.in_flight() == PreviewTransportCommandType::Seek);
    CHECK(endpoint.Complete(PreviewTransportCommandType::Seek,
                            PreviewNativeState::Paused));
    command = endpoint.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{900});
    endpoint.RequestPlayFrom(MediaTime{100});
    endpoint.RequestPause();
    endpoint.RequestPlay();
    CHECK(endpoint.Complete(PreviewTransportCommandType::Seek,
                            PreviewNativeState::Paused));
    command = endpoint.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{100});
    CHECK(endpoint.Complete(PreviewTransportCommandType::Seek,
                            PreviewNativeState::Paused));
    CHECK(endpoint.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(endpoint.Complete(PreviewTransportCommandType::Play,
                            PreviewNativeState::Playing));
    CHECK(endpoint.play_requested());
    CHECK(!endpoint.command_pending());

}

void TestPreviewPlaybackPositionFloor() {
    easnap::media::PreviewTransport transport;
    const MediaTime last_video_sample{119666666};
    const MediaTime tail_target{120050000};
    const MediaTime earlier_target{10000000};
    transport.Reset(true);
    transport.RequestSeek(tail_target);
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(transport.play_requested());
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(transport.Complete(PreviewTransportCommandType::Pause,
                             PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(transport.Complete(PreviewTransportCommandType::Seek,
                             PreviewNativeState::Paused));
    CHECK(transport.position_override() == tail_target);
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(transport.Complete(PreviewTransportCommandType::Play,
                             PreviewNativeState::Playing));
    CHECK(!transport.position_pending());
    CHECK(!transport.position_override());
    CHECK(transport.ResolvePosition(last_video_sample) == tail_target);
    CHECK(transport.ResolvePosition(tail_target - MediaTime{1}) == tail_target);
    CHECK(transport.ResolvePosition(tail_target + MediaTime{1000}) ==
          tail_target + MediaTime{1000});

    // Pausing before the native clock catches up must not move the logical
    // playhead back outside the requested selected interval.
    transport.RequestPause();
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(transport.Complete(PreviewTransportCommandType::Pause,
                             PreviewNativeState::Paused));
    CHECK(transport.ResolvePosition(last_video_sample) == tail_target);
    CHECK(transport.ResolvePosition(tail_target + MediaTime{1000}) ==
          tail_target + MediaTime{1000});

    // A newer backwards seek owns its own position immediately. Neither the
    // old floor nor an unmatched callback can hide that newer target.
    transport.RequestSeek(earlier_target);
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(!transport.Complete(PreviewTransportCommandType::Play,
                              PreviewNativeState::Paused));
    CHECK(transport.ResolvePosition(last_video_sample) == earlier_target);
    CHECK(transport.Complete(PreviewTransportCommandType::Seek,
                             PreviewNativeState::Paused));
    transport.RequestPlay();
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(transport.Complete(PreviewTransportCommandType::Play,
                             PreviewNativeState::Playing));
    CHECK(transport.ResolvePosition(earlier_target) == earlier_target);
    CHECK(transport.ResolvePosition(earlier_target + MediaTime{1000}) ==
          earlier_target + MediaTime{1000});
    transport.Ended(PreviewNativeState::Stopped);
    CHECK(!transport.play_requested());
    CHECK(transport.ResolvePosition(MediaTime::zero()) == MediaTime::zero());

    transport.RequestPlayFrom(tail_target);
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(transport.Complete(PreviewTransportCommandType::Seek,
                             PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(transport.Complete(PreviewTransportCommandType::Play,
                             PreviewNativeState::Playing));
    transport.RequestPause();
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);
    transport.Ended(PreviewNativeState::Stopped);
    CHECK(transport.ResolvePosition(MediaTime::zero()) == tail_target);
    transport.RequestPlayFrom(earlier_target);
    CHECK(!transport.Complete(PreviewTransportCommandType::Play,
                              PreviewNativeState::Stopped));
    CHECK(transport.ResolvePosition(MediaTime::zero()) == earlier_target);
    CHECK(transport.Complete(PreviewTransportCommandType::Pause,
                             PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(transport.Complete(PreviewTransportCommandType::Seek,
                             PreviewNativeState::Playing));
    CHECK(transport.ResolvePosition(MediaTime::zero()) == earlier_target);
    transport.Failed();
    CHECK(transport.ResolvePosition(MediaTime::zero()) == MediaTime::zero());

    transport.Reset(true);
    transport.RequestSeek(tail_target);
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(transport.Complete(PreviewTransportCommandType::Pause,
                             PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(transport.Complete(PreviewTransportCommandType::Seek,
                             PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(transport.Complete(PreviewTransportCommandType::Play,
                             PreviewNativeState::Playing));
    transport.Reset();
    CHECK(transport.ResolvePosition(MediaTime::zero()) == MediaTime::zero());
}

void TestMissingEndpointSeekCompletionPreservesOwnership() {
    using easnap::media::PreviewLiveness;
    using easnap::media::PreviewLivenessFailure;
    easnap::media::PreviewTransport transport;
    PreviewLiveness liveness;
    liveness.Reset(7);
    // Exact original audio/video trace: Pause succeeds; a native seek to the
    // exclusive duration moves position but never delivers POSITION_SET.
    const MediaTime out{120106458};
    const MediaTime in{10000000};
    transport.Reset(true);
    transport.RequestPauseAt(out);
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(transport.Complete(PreviewTransportCommandType::Pause,
                             PreviewNativeState::Paused));
    const auto command = transport.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == out);
    const auto observe = [&](uint64_t now) {
        return liveness.Observe(now, 7, 59, transport.in_flight(),
            transport.play_requested(), transport.playing(), out,
            transport.position_pending());
    };
    CHECK(observe(100) == PreviewLivenessFailure::None);
    transport.RequestPlayFrom(in);
    transport.RequestPause();
    transport.RequestPlay();
    CHECK(observe(2099) == PreviewLivenessFailure::None);
    CHECK(observe(2100) == PreviewLivenessFailure::CommandTimeout);
    CHECK(transport.in_flight() == PreviewTransportCommandType::Seek);
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(transport.play_requested());
    CHECK(transport.ResolvePosition(out) == in);

    // Detection grants no completion authority. Only after replacement may
    // the owner reset policy and restore the saved newest position/intent.
    const auto restart_position = transport.ResolvePosition(out);
    const bool restart_playing = transport.play_requested();
    transport.Reset();
    liveness.Reset(8);
    transport.RequestSeek(restart_position);
    if (restart_playing) transport.RequestPlay();
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(liveness.Observe(2101, 8, 60, transport.in_flight(), true, false,
                           MediaTime::zero(), true) == PreviewLivenessFailure::None);
    CHECK(liveness.Observe(99999, 7, 59, PreviewTransportCommandType::Seek,
                           true, false, out, true) == PreviewLivenessFailure::None);
    CHECK(liveness.Observe(2102, 8, 60, transport.in_flight(), true, false,
                           MediaTime::zero(), true) == PreviewLivenessFailure::None);
    CHECK(transport.Complete(PreviewTransportCommandType::Seek,
                             PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(transport.Complete(PreviewTransportCommandType::Play,
                             PreviewNativeState::Playing));
    CHECK(transport.ResolvePosition(in + MediaTime{1000}) == in + MediaTime{1000});
    CHECK(transport.play_requested());
    CHECK(!transport.command_pending());
}

void TestPreviewLivenessDeadlinesAndNativeProgress() {
    using easnap::media::PreviewLiveness;
    using easnap::media::PreviewLivenessFailure;
    PreviewLiveness monitor;
    monitor.Reset(1);
    const auto idle = PreviewTransportCommandType::None;
    const auto pause = PreviewTransportCommandType::Pause;
    const auto seek = PreviewTransportCommandType::Seek;
    uint64_t settled_sequence = 1;
    const auto observe = [&](uint64_t now, bool desired, bool playing,
                             std::optional<MediaTime> native) {
        return monitor.Observe(now, 1, settled_sequence, idle, desired, playing, native, false);
    };
    CHECK(observe(0, true, true, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(1999, true, true, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(2000, true, true, MediaTime{100}) == PreviewLivenessFailure::PlaybackStalled);
    monitor.Reset(1);
    CHECK(observe(0, true, true, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(1500, true, true, MediaTime{110}) == PreviewLivenessFailure::None);
    CHECK(observe(3499, true, true, MediaTime{110}) == PreviewLivenessFailure::None);
    CHECK(observe(3500, true, true, MediaTime{110}) == PreviewLivenessFailure::PlaybackStalled);
    monitor.Reset(1);
    CHECK(observe(0, false, false, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(10000, false, false, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(10001, true, false, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(12001, true, false, MediaTime{100}) == PreviewLivenessFailure::PlaybackStalled);
    monitor.Reset(1);
    CHECK(observe(0, false, true, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(1999, false, true, MediaTime{500}) == PreviewLivenessFailure::None);
    CHECK(observe(2000, false, true, MediaTime{600}) == PreviewLivenessFailure::PlaybackStalled);
    // A newer opposite intent gets a convergence interval of its own. Its
    // native state cannot postpone that interval by advancing against intent.
    monitor.Reset(1);
    CHECK(observe(0, true, true, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(1900, false, true, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(3899, false, true, MediaTime{500}) == PreviewLivenessFailure::None);
    CHECK(observe(3900, false, true, MediaTime{600}) == PreviewLivenessFailure::PlaybackStalled);
    CHECK(observe(3901, false, false, MediaTime{600}) == PreviewLivenessFailure::None);
    CHECK(observe(10000, false, false, MediaTime{600}) == PreviewLivenessFailure::None);
    monitor.Reset(1);
    CHECK(observe(0, true, true, std::nullopt) == PreviewLivenessFailure::None);
    CHECK(observe(2000, true, true, std::nullopt) == PreviewLivenessFailure::PlaybackStalled);
    monitor.Reset(1);
    CHECK(observe(0, true, true, MediaTime{100}) == PreviewLivenessFailure::None);
    CHECK(observe(1500, true, true, MediaTime{90}) == PreviewLivenessFailure::None);
    CHECK(observe(2000, true, true, MediaTime{100}) == PreviewLivenessFailure::PlaybackStalled);

    monitor.Reset(1);
    CHECK(monitor.Observe(0, 1, 10, pause, false, true, MediaTime{100}, false) ==
          PreviewLivenessFailure::None);
    CHECK(monitor.Observe(1000, 1, 10, pause, true, false, MediaTime{300}, true) ==
          PreviewLivenessFailure::None);
    CHECK(monitor.Observe(2000, 1, 10, pause, false, false, MediaTime{500}, true) ==
          PreviewLivenessFailure::CommandTimeout);
    // A different legitimately issued command gets its own deadline, while a
    // fresh seek invalidates the old playback clock baseline.
    CHECK(monitor.Observe(2001, 1, 11, seek, true, false, MediaTime{500}, true) ==
          PreviewLivenessFailure::None);
    CHECK(monitor.Observe(4000, 1, 11, seek, true, false, MediaTime{50}, true) ==
          PreviewLivenessFailure::None);
    settled_sequence = 11;
    CHECK(observe(4001, true, true, MediaTime{50}) == PreviewLivenessFailure::None);
    CHECK(observe(5000, true, true, MediaTime{51}) == PreviewLivenessFailure::None);
    CHECK(observe(6999, true, true, MediaTime{51}) == PreviewLivenessFailure::None);
    CHECK(observe(7000, true, true, MediaTime{51}) == PreviewLivenessFailure::PlaybackStalled);
}

void TestPreviewLivenessCompletedSeeksBetweenPolls() {
    using easnap::media::PreviewLiveness;
    using easnap::media::PreviewLivenessFailure;
    const auto idle = PreviewTransportCommandType::None;
    // Every Pause/Seek/Play callback can complete between watchdog samples.
    // Exercise the real transport policy instead of assuming those commands.
    for (uint32_t seed : {1u, 17u, 0x1234u, 0xA53Cu}) {
        easnap::media::PreviewTransport transport;
        PreviewLiveness monitor;
        transport.Reset(true);
        monitor.Reset(1);
        uint64_t sequence = 1;
        uint64_t now = 0;
        MediaTime native{80'000'000};
        CHECK(monitor.Observe(now, 1, sequence, idle, true, true, native, false) ==
              PreviewLivenessFailure::None);
        for (unsigned sample = 0; sample < 80; ++sample) {
            seed = seed * 1664525u + 1013904223u;
            const MediaTime target{2'000'000 + static_cast<int64_t>(seed % 43'000'000u)};
            transport.RequestSeek(target);
            CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);
            ++sequence;
            CHECK(transport.Complete(PreviewTransportCommandType::Pause,
                                     PreviewNativeState::Paused));
            CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
            ++sequence;
            CHECK(transport.Complete(PreviewTransportCommandType::Seek,
                                     PreviewNativeState::Paused));
            CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
            ++sequence;
            CHECK(transport.Complete(PreviewTransportCommandType::Play,
                                     PreviewNativeState::Playing));
            CHECK(!transport.command_pending());
            CHECK(transport.play_requested());
            now += 17 + seed % 183u;
            native = target;
            CHECK(monitor.Observe(now, 1, sequence, transport.in_flight(),
                                  transport.play_requested(), transport.playing(),
                                  native, transport.position_pending()) ==
                  PreviewLivenessFailure::None);
            // Native progress belongs to the new seek, even below the clock
            // high-water mark from before that seek.
            now += 7;
            native += MediaTime{70'000};
            CHECK(monitor.Observe(now, 1, sequence, idle, true, true, native, false) ==
                  PreviewLivenessFailure::None);
        }
        // Continuous completed seeks must not disable detection after input ends.
        CHECK(monitor.Observe(now + PreviewLiveness::timeout_ms - 1, 1, sequence,
                              idle, true, true, native, false) ==
              PreviewLivenessFailure::None);
        CHECK(monitor.Observe(now + PreviewLiveness::timeout_ms, 1, sequence,
                              idle, true, true, native, false) ==
              PreviewLivenessFailure::PlaybackStalled);
    }

    PreviewLiveness monitor;
    monitor.Reset(2);
    CHECK(monitor.Observe(0, 2, 5, idle, true, true, MediaTime{80'000'000}, false) ==
          PreviewLivenessFailure::None);
    // An unsolicited backwards clock with no issued command remains a stall.
    CHECK(monitor.Observe(PreviewLiveness::timeout_ms, 2, 5, idle, true, true,
                          MediaTime{20'000'000}, false) ==
          PreviewLivenessFailure::PlaybackStalled);

    easnap::media::PreviewTransport transport;
    transport.Reset();
    transport.RequestPlayFrom(MediaTime{20'000'000});
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
    monitor.Reset(3);
    CHECK(monitor.Observe(0, 3, 20, transport.in_flight(), true, false,
                          MediaTime{20'000'000}, true) == PreviewLivenessFailure::None);
    for (uint64_t now = 100; now < PreviewLiveness::timeout_ms; now += 100) {
        transport.RequestPlayFrom(MediaTime{static_cast<int64_t>(now) * 10'000});
        CHECK(transport.TakeNext().type == idle);
        CHECK(monitor.Observe(now, 3, 20, transport.in_flight(), true, false,
                              MediaTime{static_cast<int64_t>(now) * 10'000}, true) ==
              PreviewLivenessFailure::None);
    }
    // New input and native-clock movement cannot complete or extend an owned
    // command whose real completion callback never arrived.
    CHECK(monitor.Observe(PreviewLiveness::timeout_ms, 3, 20, transport.in_flight(),
                          true, false, MediaTime{40'000'000}, true) ==
          PreviewLivenessFailure::CommandTimeout);
}

void TestTimedPreviewIntentConvergence() {
    using easnap::app::TrimPlaybackAction;
    using easnap::media::PreviewTransport;
    using easnap::media::PreviewTransportCommand;
    constexpr MediaTime millisecond{10'000};
    constexpr unsigned seeds = 64;
    constexpr int64_t starts_ms[] = {
        4500, 8500, 8750, 8900, 8990, 9000, 9050, 11900, 12000};
    for (unsigned seed = 1; seed <= seeds; ++seed) {
        for (const int64_t start_ms : starts_ms) {
            uint32_t random = seed;
            const auto next = [&] {
                random = random * 1664525u + 1013904223u;
                return random;
            };
            PreviewTransport transport;
            PreviewTransportCommand outstanding;
            PreviewNativeState native = (seed % 2) != 0
                ? PreviewNativeState::Playing : PreviewNativeState::Paused;
            transport.Reset(native == PreviewNativeState::Playing);
            MediaTime native_position = millisecond * start_ms;
            const MediaTime duration = millisecond * 12000;
            const MediaTime selected_in = millisecond * 1000;
            const MediaTime selected_out = start_ms >= 11900
                ? duration : millisecond * 9000;
            unsigned now = 0;
            unsigned apply_at = 0;
            unsigned callback_at = 0;
            unsigned user_at = 10;
            bool applied = false;
            std::optional<unsigned> ended_at;
            const auto dispatch = [&] {
                const auto command = transport.TakeNext();
                if (outstanding.type != PreviewTransportCommandType::None) {
                    CHECK(command.type == PreviewTransportCommandType::None);
                } else if (command.type != PreviewTransportCommandType::None) {
                    if (command.type == PreviewTransportCommandType::Seek) {
                        CHECK(native != PreviewNativeState::Playing);
                    }
                    outstanding = command;
                    applied = false;
                    apply_at = now + 5 + next() % 80;
                    callback_at = apply_at + 1 + next() % 110;
                }
            };
            const auto apply_decision = [&](easnap::app::TrimPlaybackDecision decision) {
                switch (decision.action) {
                    case TrimPlaybackAction::Play: transport.RequestPlay(); break;
                    case TrimPlaybackAction::Pause: transport.RequestPause(); break;
                    case TrimPlaybackAction::SeekThenPlay:
                        transport.RequestPlayFrom(decision.position); break;
                    case TrimPlaybackAction::PauseAt:
                        transport.RequestPauseAt(decision.position); break;
                    default: break;
                }
                dispatch();
            };
            const auto tick = [&](bool interact) {
                ++now;
                if (native == PreviewNativeState::Playing) {
                    native_position += millisecond;
                    if (native_position >= duration) {
                        native_position = duration;
                        native = PreviewNativeState::Stopped;
                        ended_at = now + 1 + next() % 100;
                    }
                }
                // Native state changes before its callback is delivered. User
                // input and endpoint observations can occur in that interval.
                if (outstanding.type != PreviewTransportCommandType::None &&
                    !applied && now >= apply_at) {
                    applied = true;
                    if (outstanding.type == PreviewTransportCommandType::Seek) {
                        native_position = outstanding.position;
                    } else {
                        native = outstanding.type == PreviewTransportCommandType::Play
                            ? PreviewNativeState::Playing : PreviewNativeState::Paused;
                    }
                }
                if (ended_at && now >= *ended_at) {
                    ended_at.reset();
                    transport.Ended(native);
                    dispatch();
                }
                if (outstanding.type != PreviewTransportCommandType::None &&
                    applied && now >= callback_at) {
                    CHECK(transport.in_flight() == outstanding.type);
                    CHECK(transport.Complete(outstanding.type, native));
                    outstanding = {};
                    dispatch();
                }
                if (interact && now >= user_at) {
                    switch ((next() >> 24) % 6) {
                        case 0:
                        case 1:
                            apply_decision(easnap::app::PlanTrimPlaybackToggle(
                                transport.play_requested(), transport.ended(),
                                transport.ResolvePosition(native_position),
                                selected_in, selected_out));
                            break;
                        case 2: transport.RequestPause(); break;
                        case 3: transport.RequestPlayFrom(selected_in); break;
                        case 4: {
                            const auto target = millisecond *
                                (1000 + static_cast<int64_t>(next() % 8500));
                            const auto accepted = easnap::app::PlanTrimTimelineSeek(
                                transport.play_requested(), target,
                                selected_in, selected_out);
                            if (accepted) transport.RequestSeek(*accepted);
                            break;
                        }
                        default: transport.RequestPlay(); break;
                    }
                    user_at = now + 10 + next() % 291;
                    dispatch();
                }
                // The actual TrimWindow timer runs every 100 ms. Keep it
                // active during final convergence as well as user activity.
                if (now % 100 == 0) {
                    apply_decision(easnap::app::PlanTrimPlaybackProgress(
                        transport.play_requested(),
                        transport.ResolvePosition(native_position), selected_out));
                }
            };
            for (unsigned elapsed = 0; elapsed < 2500; ++elapsed) tick(true);

            const auto converge = [&](bool playing) {
                dispatch();
                const unsigned deadline = now + 1500;
                while (now < deadline && (transport.command_pending() ||
                       transport.playing() != playing || ended_at)) tick(false);
                const bool settled = !transport.command_pending() &&
                    transport.play_requested() == playing &&
                    transport.playing() == playing &&
                    outstanding.type == PreviewTransportCommandType::None &&
                    (native == PreviewNativeState::Playing) == playing;
                if (!settled) {
                    std::fprintf(stderr,
                        "Timed preview liveness seed=%u start_ms=%lld now=%u "
                        "wanted=%u observed=%u native=%u owned=%u logical=%lld "
                        "native_position=%lld\n", seed, start_ms, now,
                        static_cast<unsigned>(transport.play_requested()),
                        static_cast<unsigned>(transport.playing()),
                        static_cast<unsigned>(native),
                        static_cast<unsigned>(transport.in_flight()),
                        transport.ResolvePosition(native_position).count(),
                        native_position.count());
                }
                CHECK(settled);
                return settled;
            };
            // Final Play must converge and advance both clocks, even when it
            // supersedes an old selected-Out Pause or pending Seek.
            transport.RequestPlayFrom(selected_in);
            if (!converge(true)) continue;
            auto previous_native = native_position;
            auto previous_logical = transport.ResolvePosition(native_position);
            for (unsigned elapsed = 0; elapsed < 250; ++elapsed) tick(false);
            CHECK(native_position >= previous_native + millisecond * 200);
            CHECK(transport.ResolvePosition(native_position) >=
                  previous_logical + millisecond * 200);

            transport.RequestPause();
            if (!converge(false)) continue;
            previous_native = native_position;
            for (unsigned elapsed = 0; elapsed < 250; ++elapsed) tick(false);
            CHECK(native_position == previous_native);
            // Reuse the same transport for Pause -> Seek -> Play without
            // settling the seek before expressing the newest Play intent.
            transport.RequestSeek(selected_in + millisecond * 500);
            dispatch();
            transport.RequestPlay();
            if (!converge(true)) continue;
            previous_native = native_position;
            previous_logical = transport.ResolvePosition(native_position);
            for (unsigned elapsed = 0; elapsed < 250; ++elapsed) tick(false);
            CHECK(native_position >= previous_native + millisecond * 200);
            CHECK(transport.ResolvePosition(native_position) >=
                  previous_logical + millisecond * 200);
        }
    }
    std::printf("Timed preview convergence: %u seeds x 9 stress regions\n", seeds);
}

void TestPreviewTransportRecoversAcrossPendingPause() {
    easnap::media::PreviewTransport active_seek;
    active_seek.Reset(true);
    active_seek.RequestSeek(MediaTime{450});
    CHECK(active_seek.TakeNext().type == PreviewTransportCommandType::Pause);
    active_seek.RequestPause();
    active_seek.RequestPlay();
    CHECK(active_seek.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(active_seek.Complete(PreviewTransportCommandType::Pause,
                               PreviewNativeState::Paused));
    CHECK(active_seek.TakeNext().type == PreviewTransportCommandType::Seek);
    CHECK(active_seek.Complete(PreviewTransportCommandType::Seek,
                               PreviewNativeState::Paused));
    CHECK(active_seek.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(active_seek.Complete(PreviewTransportCommandType::Play,
                               PreviewNativeState::Playing));
    CHECK(active_seek.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(active_seek.playing());

    easnap::media::PreviewTransport moved_out;
    moved_out.Reset(true);
    moved_out.RequestPauseAt(MediaTime{500});
    CHECK(moved_out.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(moved_out.Complete(PreviewTransportCommandType::Pause, PreviewNativeState::Paused));
    auto out_command = moved_out.TakeNext();
    CHECK(out_command.type == PreviewTransportCommandType::Seek);
    CHECK(out_command.position == MediaTime{500});
    CHECK(moved_out.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(!moved_out.playing());
    CHECK(!moved_out.command_pending());

    easnap::media::PreviewTransport transport;
    transport.Reset(true);
    transport.RequestPause();
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);

    // Play at Out while Pause is still asynchronous: seek is deferred until
    // PAUSE completes, then Play is deferred until POSITION_SET completes.
    transport.RequestPlayFrom(MediaTime{125});
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(transport.Complete(PreviewTransportCommandType::Pause, PreviewNativeState::Paused));
    auto command = transport.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{125});
    CHECK(transport.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(transport.Complete(PreviewTransportCommandType::Play, PreviewNativeState::Playing));
    CHECK(transport.playing());
}

void TestPreviewNaturalEndKeepsLogicalPosition() {
    easnap::media::PreviewTransport transport;
    transport.Reset(true);
    transport.Ended(PreviewNativeState::Stopped, MediaTime{900});
    CHECK(transport.ended());
    CHECK(!transport.play_requested());
    CHECK(!transport.command_pending());
    CHECK(transport.ResolvePosition(MediaTime::zero()) == MediaTime{900});
    transport.RequestPlayFrom(MediaTime{200});
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Seek);
    transport.Ended(PreviewNativeState::Stopped, MediaTime{900});
    CHECK(transport.position_pending());
    CHECK(transport.play_requested());
    CHECK(transport.ResolvePosition(MediaTime::zero()) == MediaTime{200});
    CHECK(transport.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(transport.Complete(PreviewTransportCommandType::Play, PreviewNativeState::Playing));
    transport.Ended(PreviewNativeState::Playing, MediaTime{900});
    CHECK(transport.play_requested());
    CHECK(transport.ResolvePosition(MediaTime{300}) == MediaTime{300});
}

void TestPreviewTransportKeepsRestartAcrossLateEndedEvent() {
    easnap::media::PreviewTransport natural_end;
    natural_end.Reset(true);
    natural_end.Ended(PreviewNativeState::Stopped);
    CHECK(!natural_end.play_requested());
    CHECK(!natural_end.command_pending());

    easnap::media::PreviewTransport transport;
    transport.Reset(true);
    transport.RequestPauseAt(MediaTime{900});
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Pause);
    transport.RequestPlayFrom(MediaTime{200});

    // EndOfPresentation from the old playback can arrive after the user has
    // already requested restart. It must not erase the queued In seek.
    transport.Ended(PreviewNativeState::Stopped);
    CHECK(transport.Complete(PreviewTransportCommandType::Pause, PreviewNativeState::Paused));
    auto command = transport.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{200});
    CHECK(transport.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(transport.TakeNext().type == PreviewTransportCommandType::Play);
}

void TestPreviewTransportReconcilesStoppedPauseRace() {
    easnap::media::PreviewTransport direct_pause;
    direct_pause.Reset(true);
    direct_pause.RequestPause();
    CHECK(direct_pause.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(direct_pause.ReconcilePauseWhileStopped());
    CHECK(!direct_pause.playing());
    CHECK(!direct_pause.play_requested());
    CHECK(direct_pause.in_flight() == PreviewTransportCommandType::None);
    CHECK(!direct_pause.command_pending());

    easnap::media::PreviewTransport pause_at_out;
    pause_at_out.Reset(true);
    pause_at_out.RequestPauseAt(MediaTime{900});
    CHECK(pause_at_out.TakeNext().type == PreviewTransportCommandType::Pause);

    // STOP/PLAYBACK_ENDED is only an observation. The Pause callback still owns
    // the command slot, so its late failure cannot be misattributed to Seek.
    pause_at_out.Ended(PreviewNativeState::Stopped);
    CHECK(!pause_at_out.playing());
    CHECK(pause_at_out.in_flight() == PreviewTransportCommandType::Pause);
    CHECK(pause_at_out.TakeNext().type == PreviewTransportCommandType::None);
    CHECK(pause_at_out.ReconcilePauseWhileStopped());
    auto command = pause_at_out.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{900});

    // A late Pause callback cannot release the newer position command.
    CHECK(!pause_at_out.Complete(PreviewTransportCommandType::Pause, PreviewNativeState::Paused));
    CHECK(pause_at_out.in_flight() == PreviewTransportCommandType::Seek);
    CHECK(pause_at_out.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(!pause_at_out.playing());
    CHECK(!pause_at_out.command_pending());

    // The reconciled transport remains reusable for repeated restart/Out cycles.
    pause_at_out.RequestPlayFrom(MediaTime{100});
    command = pause_at_out.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(pause_at_out.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(pause_at_out.TakeNext().type == PreviewTransportCommandType::Play);
    CHECK(pause_at_out.Complete(PreviewTransportCommandType::Play, PreviewNativeState::Playing));
    pause_at_out.RequestPauseAt(MediaTime{700});
    CHECK(pause_at_out.TakeNext().type == PreviewTransportCommandType::Pause);
    CHECK(pause_at_out.ReconcilePauseWhileStopped());
    command = pause_at_out.TakeNext();
    CHECK(command.type == PreviewTransportCommandType::Seek);
    CHECK(command.position == MediaTime{700});
    CHECK(pause_at_out.Complete(PreviewTransportCommandType::Seek, PreviewNativeState::Paused));
    CHECK(!pause_at_out.command_pending());

}

void CheckLayout(int width, int height, float scale) {
    const auto layout =
        easnap::ui::CalculateTrimWindowLayout(width, height, scale);
    CHECK(easnap::ui::IsTrimWindowLayoutContained(layout, width, height));
    CHECK(layout.video.bottom() <= layout.timeline.y);
    CHECK(layout.video.y==static_cast<int>(std::lround(40*scale)));
    CHECK(layout.video.x==0 && layout.video.width==width);
    CHECK(layout.timeline.bottom()<=layout.time_label.y);
    CHECK(layout.time_label.bottom()<=layout.range_label.y);
    CHECK(layout.timeline.bottom() <= layout.help.y);
    CHECK(layout.help.bottom() <= layout.range_label.y);
    CHECK(layout.range_label.bottom() <= layout.play_pause.y);
    CHECK(layout.play_pause.bottom() <= layout.status.y);
    CHECK(layout.status.bottom() <= layout.progress.y);
    CHECK(layout.progress.bottom() <= layout.save_or_cancel.y);
    CHECK(layout.play_pause.bottom() <= height);
    CHECK(layout.discard.right() <= width);
    CHECK(layout.save_or_cancel.width == layout.discard.width);
    CHECK(layout.copy.width == layout.discard.width);
    CHECK(layout.status.height == 0 && layout.progress.height == 0);
}

void TestTrimLayoutContainmentAcrossDpiAndResize() {
    CheckLayout(720, 540, 1.0f);
    CheckLayout(900, 675, 1.25f);
    CheckLayout(1'440, 1'080, 2.0f);
    CheckLayout(1'200, 700, 1.0f);
    CheckLayout(400, 320, 1.0f);
    CheckLayout(800, 640, 2.0f);
    CheckLayout(1024, 700, 2.0f);
    const auto narrow = easnap::ui::CalculateTrimWindowLayout(800, 640, 2.0f);
    CHECK(narrow.save_or_cancel.height == 80); // Actual DPI; no text/target shrink.
    CHECK(narrow.play_pause.height == 80);
    const auto message=easnap::ui::CalculateTrimWindowLayout(720,540,1,52);
    const auto busy=easnap::ui::CalculateTrimWindowLayout(720,540,1,0,true);
    CHECK(message.status.height==52 && message.progress.height==0);
    CHECK(busy.status.height==0 && busy.progress.height==28);
    CHECK(message.status.bottom()<=message.save_or_cancel.y);
    CHECK(busy.progress.bottom()<=busy.save_or_cancel.y);
    for(const float scale:{1.0f,1.25f,1.5f,2.0f}) {
        const auto dip=[scale](int value) { return static_cast<int>(std::lround(value*scale)); };
        for(const auto size:{std::pair{480,400},std::pair{1200,400},std::pair{480,800}}) {
            for(const bool exporting:{false,true}) {
                for(const int status:{0,52,10'000}) {
                    // The range readout can wrap at the supported minimum.
                    // No text or dynamic row may borrow caption/timeline space.
                    const easnap::ui::TrimTextMetrics font{dip(17),dip(22),dip(34)};
                    const auto current=easnap::ui::CalculateTrimWindowLayout(
                        dip(size.first),dip(size.second),scale,dip(status),exporting,font);
                    CHECK(easnap::ui::IsTrimWindowLayoutContained(current,dip(size.first),dip(size.second)));
                    CHECK(current.video.y==dip(easnap::ui::kTrimChromeHeightDip));
                    CHECK(current.video.height>=dip(24));
                    CHECK(current.video.bottom()<=current.timeline.y);
                    CHECK(current.timeline_label_height>=font.line_height);
                    CHECK(current.timeline_track_center-dip(23)>=current.timeline_label_height);
                    CHECK(current.timeline_track_center+dip(23)<=current.timeline.height);
                    CHECK(current.timeline.bottom()<=current.time_label.y);
                    CHECK(current.time_label.height>=font.time_height);
                    CHECK(current.time_label.bottom()<=current.range_label.y);
                    CHECK(current.range_label.height>=font.range_height);
                    CHECK(current.range_label.bottom()<=current.play_pause.y);
                    CHECK(current.play_pause.bottom()<=current.status.y);
                    CHECK(current.status.bottom()<=current.progress.y);
                    CHECK(current.progress.bottom()<=current.save_or_cancel.y);
                    CHECK(current.status.height<=dip(status));
                    CHECK(status==0 || current.status.height>0);
                }
            }
        }
    }
    for(const auto source: {std::pair{1920,1080},std::pair{1080,1920},std::pair{800,800},std::pair{3440,1440},std::pair{10000,100},std::pair{100,10000}}) {
        const auto initial=easnap::ui::InitialTrimWindowSize(1920,1040,1,source.first,source.second);
        CHECK(initial.width>=560 && initial.width<=1920 && initial.height<=1040);
        const auto layout=easnap::ui::CalculateTrimWindowLayout(initial.width,initial.height,1);
        const auto image=easnap::ui::FitTrimVideo(layout.video,source.first,source.second);
        // Independent pixel rounding can magnify a subpixel width difference
        // on an extreme portrait source. Bound the error in both source axes.
        CHECK(std::abs(static_cast<double>(image.width)/source.first-static_cast<double>(image.height)/source.second)
            <=0.5/source.first+0.5/source.second);
        CHECK(image.width<=layout.video.width && image.height<=layout.video.height);
        CHECK(initial.height>=easnap::ui::kTrimMinClientHeightDip);
    }
    const auto wide=easnap::ui::FitTrimVideo({0,0,1200,400},800,800);
    const auto less_bar=easnap::ui::FitTrimVideo({0,0,600,400},800,800);
    const auto narrow_image=easnap::ui::FitTrimVideo({0,0,300,400},800,800);
    CHECK(wide.width==400 && less_bar.width==400 && narrow_image.width==300 && narrow_image.height==300);
}

// Display matrix: work areas from a small laptop panel to 4K and ultrawide at
// every Windows scale factor that leaves a usable desktop. The initial window
// must fit the work area and its controls must stay inside the client.
void TestTrimLayoutAcrossDisplayMatrix() {
    const std::pair<int,int> work_areas[]{{1366,728},{1280,680},{1600,860},{1920,1040},{2560,1400},
                                          {3440,1400},{3840,2120},{5120,1400},{1080,1880},{2160,3800}};
    const float scales[]{1.0f,1.25f,1.5f,1.75f,2.0f,2.25f,2.5f,3.0f,3.5f};
    const std::pair<int,int> sources[]{{1920,1080},{640,360},{100,100},{3840,2160},{200,1200},{5120,1440}};
    int combinations=0;
    for(const auto work:work_areas) for(const float scale:scales) {
        // Windows does not offer a scale that leaves less than about 500
        // effective rows or 600 effective columns.
        if(work.second/scale<500 || work.first/scale<600) { continue; }
        for(const auto source:sources) {
            ++combinations;
            const auto initial=easnap::ui::InitialTrimWindowSize(work.first,work.second,scale,source.first,source.second);
            CHECK(initial.width>0 && initial.height>0);
            CHECK(initial.width<=work.first && initial.height<=work.second);
            const auto layout=easnap::ui::CalculateTrimWindowLayout(initial.width,initial.height,scale);
            CHECK(easnap::ui::IsTrimWindowLayoutContained(layout,initial.width,initial.height));
            CHECK(layout.video.height>0 && layout.video.bottom()<=layout.timeline.y);
            CHECK(layout.play_pause.bottom()<=layout.save_or_cancel.y+layout.save_or_cancel.height);
            const auto image=easnap::ui::FitTrimVideo(layout.video,source.first,source.second);
            CHECK(image.width>0 && image.height>0);
            CHECK(image.width<=layout.video.width && image.height<=layout.video.height);
        }
    }
    CHECK(combinations>=300);
}

void TestSuccessfulExportAndDuplicatePrevention() {
    auto probe = std::make_shared<ExportProbe>();
    probe->block = true;
    std::atomic<int> completions{0};
    TrimExportController controller(
        std::make_unique<ControlledExporter>(probe),
        [&] { completions.fetch_add(1, std::memory_order_relaxed); });
    TrimExportRequest request = ValidRequest();
    request.options.format = easnap::media::RecordingFormat::Gif;
    request.destination_path = L"C:\\temp\\recording.gif";
    request.replace_existing = true;
    CHECK(controller.Start(request));
    WaitUntilStarted(probe);
    CHECK(probe->started);
    CHECK(controller.Snapshot().status == TrimExportStatus::Running);
    CHECK(!controller.Start(request));
    Release(probe);
    controller.Wait();

    const auto result = controller.Snapshot();
    CHECK(result.status == TrimExportStatus::Succeeded);
    CHECK(result.progress == 1.0f);
    CHECK(result.result_path == request.destination_path);
    CHECK(completions.load(std::memory_order_relaxed) == 1);
    CHECK(probe->calls == 1);
    CHECK(probe->source == request.source_path);
    CHECK(probe->destination == request.destination_path);
    CHECK(probe->start == request.start);
    CHECK(probe->end == request.end);
    CHECK(probe->format == easnap::media::RecordingFormat::Gif);
    CHECK(probe->replace_existing);
}

void TestGifFullAndInteriorIntervalsInvokeOneExportEach() {
    auto probe = std::make_shared<ExportProbe>();
    TrimExportController controller(
        std::make_unique<ControlledExporter>(probe));

    TrimExportRequest full = ValidRequest();
    full.start = MediaTime::zero();
    full.end = MediaTime{1000};
    full.destination_path = L"C:\\temp\\full.gif";
    full.options.format = easnap::media::RecordingFormat::Gif;
    CHECK(controller.Start(full));
    controller.Wait();
    CHECK(controller.Snapshot().status == TrimExportStatus::Succeeded);
    CHECK(probe->calls == 1);
    CHECK(probe->start == MediaTime::zero());
    CHECK(probe->end == MediaTime{1000});
    CHECK(probe->format == easnap::media::RecordingFormat::Gif);

    TrimExportRequest interior = full;
    interior.start = MediaTime{125};
    interior.end = MediaTime{875};
    interior.destination_path = L"C:\\temp\\interior.gif";
    CHECK(controller.Start(interior));
    controller.Wait();
    CHECK(controller.Snapshot().status == TrimExportStatus::Succeeded);
    CHECK(probe->calls == 2);
    CHECK(probe->start == MediaTime{125});
    CHECK(probe->end == MediaTime{875});
}

void TestCancellationWaitsForWorker() {
    auto probe = std::make_shared<ExportProbe>();
    probe->block = true;
    std::atomic<int> completions{0};
    TrimExportController controller(
        std::make_unique<ControlledExporter>(probe),
        [&] { completions.fetch_add(1, std::memory_order_relaxed); });
    CHECK(controller.Start(ValidRequest()));
    WaitUntilStarted(probe);
    controller.CancelAndWait();
    CHECK(controller.Snapshot().status == TrimExportStatus::Cancelled);
    CHECK(completions.load(std::memory_order_relaxed) == 1);

    probe->block = false;
    TrimExportRequest retry = ValidRequest();
    retry.destination_path = L"C:\\temp\\trimmed-retry.mp4";
    CHECK(controller.Start(retry));
    controller.Wait();
    CHECK(controller.Snapshot().status == TrimExportStatus::Succeeded);
    CHECK(controller.Snapshot().result_path == retry.destination_path);
    CHECK(completions.load(std::memory_order_relaxed) == 2);
    CHECK(probe->calls == 2);
}

void TestFailureAndInvalidRequests() {
    auto probe = std::make_shared<ExportProbe>();
    probe->fail = true;
    probe->failure.stage =
        easnap::media::TrimExportFailureStage::MediaTypeNegotiation;
    probe->failure.has_hresult = true;
    probe->failure.hresult = static_cast<int32_t>(0xC00D36B4u);
    probe->failure.detail = L"Unsupported decoded media type";
    TrimExportController controller(
        std::make_unique<ControlledExporter>(probe));
    CHECK(controller.Start(ValidRequest()));
    controller.Wait();
    const auto failed = controller.Snapshot();
    CHECK(failed.status == TrimExportStatus::Failed);
    CHECK(failed.result_path.empty());
    CHECK(failed.failure.stage ==
          easnap::media::TrimExportFailureStage::MediaTypeNegotiation);
    CHECK(failed.failure.has_hresult);
    CHECK(failed.failure.hresult == static_cast<int32_t>(0xC00D36B4u));
    CHECK(failed.failure.detail == L"Unsupported decoded media type");

    TrimExportRequest invalid = ValidRequest();
    invalid.end = invalid.start;
    CHECK(!controller.Start(invalid));
    invalid = ValidRequest();
    invalid.source_path.clear();
    CHECK(!controller.Start(invalid));
}

}  // namespace

int main() {
    TestRangeInitializationAndValidation();
    TestRangeClampingAndTimelineMapping();
    TestRecordingFinalizationPolicy();
    TestPlaybackRestartsAndRangeReconciliation();
    TestActiveTimelineSeekPolicy();
    TestTerminalActionMediaPlanning();
    TestPreviewTransportSerializesAndCoalescesSeeks();
    TestPreviewOrderingRegressions();
    TestPreviewCallbackPermutations();
    TestGeneratedPreviewOrderings();
    TestPreviewLogicalEndWaitsForRealSeekCompletion();
    TestPreviewPlaybackPositionFloor();
    TestMissingEndpointSeekCompletionPreservesOwnership();
    TestPreviewLivenessDeadlinesAndNativeProgress();
    TestPreviewLivenessCompletedSeeksBetweenPolls();
    TestTimedPreviewIntentConvergence();
    TestPreviewTransportRecoversAcrossPendingPause();
        TestPreviewNaturalEndKeepsLogicalPosition();
        TestPreviewTransportKeepsRestartAcrossLateEndedEvent();
        TestPreviewTransportReconcilesStoppedPauseRace();
    TestTrimLayoutContainmentAcrossDpiAndResize();
    TestTrimLayoutAcrossDisplayMatrix();
    TestSuccessfulExportAndDuplicatePrevention();
    TestGifFullAndInteriorIntervalsInvokeOneExportEach();
    TestCancellationWaitsForWorker();
    TestFailureAndInvalidRequests();

    if (g_fail == 0) {
        std::printf("PASSED  %d/%d tests\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
