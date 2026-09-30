#include "app/trim_workflow.h"

#include <algorithm>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

#include "platform/platform_utils.h"

namespace easnap::app {
namespace {

media::MediaTime ClampTime(media::MediaTime value, media::MediaTime maximum) noexcept {
    return (std::clamp)(value, media::MediaTime::zero(), maximum);
}

bool ValidRequest(const TrimExportRequest& request) noexcept {
    return !request.source_path.empty() && !request.destination_path.empty() &&
           request.start >= media::MediaTime::zero() && request.end > request.start;
}

}  // namespace

bool TrimRangeModel::Initialize(media::MediaTime source_duration) noexcept {
    if (source_duration <= media::MediaTime::zero()) {
        source_duration_ = media::MediaTime::zero();
        in_ = media::MediaTime::zero();
        out_ = media::MediaTime::zero();
        current_ = media::MediaTime::zero();
        return false;
    }
    source_duration_ = source_duration;
    in_ = media::MediaTime::zero();
    out_ = source_duration;
    current_ = media::MediaTime::zero();
    return true;
}

bool TrimRangeModel::SetIn(media::MediaTime value) noexcept {
    if (source_duration_ <= media::MediaTime::zero() ||
        value < media::MediaTime::zero() || value >= out_) {
        return false;
    }
    in_ = value;
    return true;
}

bool TrimRangeModel::SetOut(media::MediaTime value) noexcept {
    if (source_duration_ <= media::MediaTime::zero() || value <= in_ ||
        value > source_duration_) {
        return false;
    }
    out_ = value;
    return true;
}

void TrimRangeModel::SetCurrent(media::MediaTime value) noexcept {
    current_ = ClampTime(value, source_duration_);
}

media::MediaTime TrimRangeModel::TimeFromTimelineOffset(
    uint32_t offset, uint32_t width) const noexcept {
    if (source_duration_ <= media::MediaTime::zero() || width == 0) {
        return media::MediaTime::zero();
    }
    const uint64_t bounded_offset = (std::min)(offset, width);
    const uint64_t duration = static_cast<uint64_t>(source_duration_.count());
    const uint64_t whole = duration / width * bounded_offset;
    const uint64_t remainder = duration % width * bounded_offset / width;
    const uint64_t scaled = whole + remainder;
    return media::MediaTime{
        static_cast<int64_t>((std::min)(
            scaled, static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())))};
}

uint32_t TrimRangeModel::TimelineOffsetFromTime(media::MediaTime value,
                                                uint32_t width) const noexcept {
    if (source_duration_ <= media::MediaTime::zero() || width == 0) {
        return 0;
    }
    const uint64_t bounded = static_cast<uint64_t>(
        ClampTime(value, source_duration_).count());
    const uint64_t duration = static_cast<uint64_t>(source_duration_.count());
    const uint64_t whole = bounded / duration * width;
    const uint64_t remainder = bounded % duration * width / duration;
    return static_cast<uint32_t>((std::min)(whole + remainder,
                                             static_cast<uint64_t>(width)));
}

bool TrimRangeModel::valid() const noexcept {
    return source_duration_ > media::MediaTime::zero() &&
           in_ >= media::MediaTime::zero() && in_ < out_ &&
           out_ <= source_duration_;
}

TrimPlaybackDecision PlanTrimPlaybackToggle(
    bool play_requested, bool ended, media::MediaTime current,
    media::MediaTime in, media::MediaTime out) noexcept {
    if (in < media::MediaTime::zero() || out <= in) {
        return {};
    }
    // The toggle always negates the last intent, including a restart whose
    // native Ended/Out state has not yet caught up with its In seek.
    if (play_requested) return {TrimPlaybackAction::Pause, current};
    if (ended || current < in || current >= out) {
        return {TrimPlaybackAction::SeekThenPlay, in};
    }
    return {play_requested ? TrimPlaybackAction::Pause
                           : TrimPlaybackAction::Play,
            current};
}

TrimPlaybackDecision PlanTrimRangeEdit(media::MediaTime current,
                                       media::MediaTime in,
                                       media::MediaTime out) noexcept {
    if (in < media::MediaTime::zero() || out <= in) {
        return {};
    }
    if (current < in) {
        return {TrimPlaybackAction::Seek, in};
    }
    if (current >= out) {
        return {TrimPlaybackAction::PauseAt, out};
    }
    return {};
}

TrimPlaybackDecision PlanTrimPlaybackProgress(
    bool play_requested, media::MediaTime current,
    media::MediaTime out) noexcept {
    if (play_requested && out > media::MediaTime::zero() && current >= out) {
        return {TrimPlaybackAction::PauseAt, out};
    }
    return {};
}

std::optional<media::MediaTime> PlanTrimTimelineSeek(
    bool play_requested, media::MediaTime requested, media::MediaTime in,
    media::MediaTime out) noexcept {
    if (in < media::MediaTime::zero() || out <= in) {
        return std::nullopt;
    }
    if (play_requested && (requested < in || requested >= out)) {
        return std::nullopt;
    }
    return requested;
}

bool IsFullTrimRange(media::MediaTime start, media::MediaTime end,
                     media::MediaTime source_duration) noexcept {
    return source_duration > media::MediaTime::zero() &&
           start == media::MediaTime::zero() && end == source_duration;
}

TrimMediaPreparation PlanTrimMediaPreparation(
    TrimTerminalAction action, media::MediaTime start, media::MediaTime end,
    media::MediaTime source_duration, bool finalized_source_available,
    bool matching_trim_available, media::RecordingFormat final_format) noexcept {
    if (action != TrimTerminalAction::Save &&
        action != TrimTerminalAction::Copy) {
        return TrimMediaPreparation::Export;
    }
    if (final_format == media::RecordingFormat::Mp4 &&
        finalized_source_available &&
        IsFullTrimRange(start, end, source_duration)) {
        return TrimMediaPreparation::ReuseFinalizedSource;
    }
    return matching_trim_available ? TrimMediaPreparation::ReuseTrimArtifact
                                   : TrimMediaPreparation::Export;
}

TrimExportController::TrimExportController(
    std::unique_ptr<media::ITrimExporter> exporter,
    CompletionCallback on_completed)
    : exporter_(std::move(exporter)), on_completed_(std::move(on_completed)) {}

TrimExportController::~TrimExportController() { CancelAndWait(); }

bool TrimExportController::Start(TrimExportRequest request) {
    if (exporter_ == nullptr || !ValidRequest(request)) {
        return false;
    }
    if (worker_.joinable()) {
        const TrimExportSnapshot current = Snapshot();
        if (current.status == TrimExportStatus::Running) {
            return false;
        }
        worker_.join();
    }

    cancel_requested_.store(false, std::memory_order_release);
    {
        const std::scoped_lock lock(state_mutex_);
        snapshot_.status = TrimExportStatus::Running;
        snapshot_.progress = 0.0f;
        snapshot_.result_path.clear();
        snapshot_.failure = {};
    }
    try {
        worker_ = std::thread(&TrimExportController::Run, this,
                              std::move(request));
    } catch (const std::system_error&) {
        const std::scoped_lock lock(state_mutex_);
        snapshot_.status = TrimExportStatus::Failed;
        snapshot_.failure.stage = media::TrimExportFailureStage::Unknown;
        snapshot_.failure.detail = L"Could not start the export worker thread";
        platform::LogMessage(L"[Trim] Could not start the export worker thread");
        return false;
    } catch (...) {
        const std::scoped_lock lock(state_mutex_);
        snapshot_.status = TrimExportStatus::Failed;
        snapshot_.failure.stage = media::TrimExportFailureStage::Unknown;
        snapshot_.failure.detail = L"Could not prepare background export work";
        platform::LogMessage(L"[Trim] Could not prepare background export work");
        return false;
    }
    return true;
}

void TrimExportController::Cancel() noexcept {
    cancel_requested_.store(true, std::memory_order_release);
}

void TrimExportController::CancelAndWait() noexcept {
    Cancel();
    Wait();
}

void TrimExportController::Wait() noexcept {
    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (...) {
            platform::LogMessage(L"[Trim] Could not join the export worker");
        }
    }
}

TrimExportSnapshot TrimExportController::Snapshot() const {
    const std::scoped_lock lock(state_mutex_);
    try {
        return snapshot_;
    } catch (const std::bad_alloc&) {
        return TrimExportSnapshot{TrimExportStatus::Failed, 0.0f, {}, {}};
    }
}

void TrimExportController::Run(TrimExportRequest request) noexcept {
    bool succeeded = false;
    media::TrimExportFailure failure;
    try {
        succeeded = exporter_->Export(
            request.source_path, request.destination_path, request.start,
            request.end, request.options, [this](float progress) {
                {
                    const std::scoped_lock lock(state_mutex_);
                    snapshot_.progress = (std::clamp)(progress, 0.0f, 1.0f);
                }
                return !cancel_requested_.load(std::memory_order_acquire);
            }, request.replace_existing);
        if (!succeeded &&
            !cancel_requested_.load(std::memory_order_acquire)) {
            failure = exporter_->LastFailure();
        }
    } catch (...) {
        failure.stage = media::TrimExportFailureStage::Unknown;
        failure.detail = L"Unexpected exception from the trim exporter";
        platform::LogMessage(L"[Trim] Unexpected exception from the trim exporter");
    }

    {
        const std::scoped_lock lock(state_mutex_);
        if (succeeded) {
            snapshot_.status = TrimExportStatus::Succeeded;
            snapshot_.progress = 1.0f;
            snapshot_.failure = {};
            try {
                snapshot_.result_path = request.destination_path;
            } catch (const std::bad_alloc&) {
                snapshot_.status = TrimExportStatus::Failed;
                snapshot_.result_path.clear();
            }
        } else if (cancel_requested_.load(std::memory_order_acquire)) {
            snapshot_.status = TrimExportStatus::Cancelled;
            snapshot_.result_path.clear();
            snapshot_.failure = {};
        } else {
            snapshot_.status = TrimExportStatus::Failed;
            snapshot_.result_path.clear();
            snapshot_.failure = std::move(failure);
        }
    }

    if (on_completed_) {
        try {
            on_completed_();
        } catch (...) {
            platform::LogMessage(L"[Trim] Export completion notification failed");
        }
    }
}

SessionState StateAfterRecordingFinalization(bool finalized,
                                             bool source_available) noexcept {
    return finalized && source_available ? SessionState::RecordingTrimming
                                         : SessionState::Idle;
}

}  // namespace easnap::app
