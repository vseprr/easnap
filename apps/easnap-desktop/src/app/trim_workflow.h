// Easnap - pure trim-range state and background export orchestration.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "app/session_transitions.h"
#include "media/media_interfaces.h"

namespace easnap::app {

/// Exact half-open trim range used by both the UI and ITrimExporter.
class TrimRangeModel final {
public:
    [[nodiscard]] bool Initialize(media::MediaTime source_duration) noexcept;
    [[nodiscard]] bool SetIn(media::MediaTime value) noexcept;
    [[nodiscard]] bool SetOut(media::MediaTime value) noexcept;
    void SetCurrent(media::MediaTime value) noexcept;

    [[nodiscard]] media::MediaTime TimeFromTimelineOffset(
        uint32_t offset, uint32_t width) const noexcept;
    [[nodiscard]] uint32_t TimelineOffsetFromTime(
        media::MediaTime value, uint32_t width) const noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] media::MediaTime source_duration() const noexcept {
        return source_duration_;
    }
    [[nodiscard]] media::MediaTime in() const noexcept { return in_; }
    [[nodiscard]] media::MediaTime out() const noexcept { return out_; }
    [[nodiscard]] media::MediaTime current() const noexcept { return current_; }

private:
    media::MediaTime source_duration_{0};
    media::MediaTime in_{0};
    media::MediaTime out_{0};
    media::MediaTime current_{0};
};

enum class TrimPlaybackAction {
    None,
    Play,
    Pause,
    Seek,
    SeekThenPlay,
    PauseAt,
};

struct TrimPlaybackDecision final {
    TrimPlaybackAction action{TrimPlaybackAction::None};
    media::MediaTime position{0};
};

/// Resolves Play/Pause without relying on lagging asynchronous player state.
/// When Play is requested, toggle cancels it even during a pending restart.
/// Otherwise Ended or out-of-range playback restarts from In, never loops.
[[nodiscard]] TrimPlaybackDecision PlanTrimPlaybackToggle(
    bool play_requested, bool ended, media::MediaTime current,
    media::MediaTime in, media::MediaTime out) noexcept;

/// Reconciles the playhead after an In/Out edit. Seek preserves the requested
/// play/pause state; PauseAt establishes the exclusive Out boundary.
[[nodiscard]] TrimPlaybackDecision PlanTrimRangeEdit(
    media::MediaTime current, media::MediaTime in,
    media::MediaTime out) noexcept;

/// Stops active playback when it reaches the half-open Out boundary.
[[nodiscard]] TrimPlaybackDecision PlanTrimPlaybackProgress(
    bool play_requested, media::MediaTime current,
    media::MediaTime out) noexcept;

/// Active playback accepts timeline seeks only inside the selected half-open
/// interval. Paused seeking retains the existing whole-source behavior.
[[nodiscard]] std::optional<media::MediaTime> PlanTrimTimelineSeek(
    bool play_requested, media::MediaTime requested, media::MediaTime in,
    media::MediaTime out) noexcept;

[[nodiscard]] bool IsFullTrimRange(media::MediaTime start,
                                   media::MediaTime end,
                                   media::MediaTime source_duration) noexcept;

enum class TrimTerminalAction { Save, Copy };
enum class TrimMediaPreparation { ReuseFinalizedSource, ReuseTrimArtifact, Export };

/// Shared Save/Copy path policy. Exact full-range actions reuse the finalized
/// source; exact cached trims are reused; all other ranges require one export.
[[nodiscard]] TrimMediaPreparation PlanTrimMediaPreparation(
    TrimTerminalAction action, media::MediaTime start, media::MediaTime end,
    media::MediaTime source_duration, bool finalized_source_available,
    bool matching_trim_available,
    media::RecordingFormat final_format = media::RecordingFormat::Mp4) noexcept;

enum class TrimExportStatus { Idle, Running, Succeeded, Cancelled, Failed };

struct TrimExportRequest final {
    std::wstring source_path;
    std::wstring destination_path;
    media::MediaTime start{0};
    media::MediaTime end{0};
    media::RecordingOptions options{};
    bool replace_existing{false};
};

struct TrimExportSnapshot final {
    TrimExportStatus status{TrimExportStatus::Idle};
    float progress{0.0f};
    std::wstring result_path;
    media::TrimExportFailure failure;
};

/// Owns one joinable export worker. All lifecycle methods are called by one
/// controller thread; Snapshot and Cancel are safe from any thread.
class TrimExportController final {
public:
    using CompletionCallback = std::function<void()>;

    explicit TrimExportController(
        std::unique_ptr<media::ITrimExporter> exporter,
        CompletionCallback on_completed = {});
    ~TrimExportController();

    TrimExportController(const TrimExportController&) = delete;
    TrimExportController& operator=(const TrimExportController&) = delete;

    /// Starts one asynchronous export. Returns false for invalid requests,
    /// absent exporters, duplicate running work, or worker-start failure.
    [[nodiscard]] bool Start(TrimExportRequest request);
    void Cancel() noexcept;
    void CancelAndWait() noexcept;
    void Wait() noexcept;
    [[nodiscard]] TrimExportSnapshot Snapshot() const;

private:
    void Run(TrimExportRequest request) noexcept;

    std::unique_ptr<media::ITrimExporter> exporter_;
    CompletionCallback on_completed_;
    std::thread worker_;
    std::atomic<bool> cancel_requested_{false};
    mutable std::mutex state_mutex_;
    TrimExportSnapshot snapshot_{};
};

/// Production policy used when recording finalization returns to the UI thread.
[[nodiscard]] SessionState StateAfterRecordingFinalization(
    bool finalized, bool source_available) noexcept;

}  // namespace easnap::app
