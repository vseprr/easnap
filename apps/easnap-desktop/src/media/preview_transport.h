// Easnap - serialized asynchronous preview transport policy.
#pragma once

#include <cstdint>
#include <optional>

#include "media/media_types.h"

namespace easnap::media {

enum class PreviewTransportCommandType { None, Play, Pause, Seek };
enum class PreviewNativeState { Stopped, Playing, Paused };

struct PreviewTransportCommand final {
    PreviewTransportCommandType type{PreviewTransportCommandType::None};
    MediaTime position{0};
};

enum class PreviewLivenessFailure { None, CommandTimeout, PlaybackStalled };

/// Detects a backend that no longer makes progress without inventing a native
/// completion. Only the owner may act on a failure, by invalidating/replacing
/// the player generation or surfacing an explicit error. Time is monotonic and
/// supplied by that owner, so operation deadlines are independent of UI input.
class PreviewLiveness final {
public:
    static constexpr uint64_t timeout_ms = 2000;

    /// Activates a new player generation and discards only monitor history.
    void Reset(uint64_t generation) noexcept;
    /// Call in observation order while the player is active. The monotonically
    /// increasing command_sequence identifies actual native dispatch, not user
    /// intent; entire command cycles may finish between periodic samples.
    /// A mismatched generation has no authority to change monitor history.
    [[nodiscard]] PreviewLivenessFailure Observe(
        uint64_t now_ms, uint64_t generation, uint64_t command_sequence,
        PreviewTransportCommandType in_flight, bool desired_playing,
        bool observed_playing, std::optional<MediaTime> native_position,
        bool position_pending) noexcept;

private:
    std::optional<uint64_t> generation_;
    PreviewTransportCommandType command_type_{PreviewTransportCommandType::None};
    uint64_t command_sequence_{0};
    uint64_t command_started_ms_{0};
    std::optional<uint64_t> playback_progress_ms_;
    std::optional<MediaTime> playback_position_;
    std::optional<bool> playback_desired_;
};

/// Coalesces desired preview state into one asynchronous MFPlay command at a
/// time. The owner must synchronize access and call Complete only for callbacks
/// received from the same player instance.
class PreviewTransport final {
public:
    void Reset(bool playing = false) noexcept;
    void RequestPlay() noexcept;
    void RequestPause() noexcept;
    void RequestSeek(MediaTime position) noexcept;
    void RequestPlayFrom(MediaTime position) noexcept;
    void RequestPauseAt(MediaTime position) noexcept;

    /// Returns and marks the next command in flight, or None while waiting for
    /// the completion callback of an earlier command.
    [[nodiscard]] PreviewTransportCommand TakeNext() noexcept;
    /// The event identifies completion, NOT current playback state. The owner
    /// supplies a fresh GetState observation, serialized with command dispatch.
    /// No other event (including Ended) may retire an outstanding command.
    [[nodiscard]] bool Complete(PreviewTransportCommandType type,
                                PreviewNativeState observed) noexcept;
    void Observe(PreviewNativeState observed) noexcept;
    /// Reconciles only the known MFPlay race where a dispatched Pause observes
    /// that the underlying session has already stopped. Pending seek intent is
    /// deliberately retained.
    [[nodiscard]] bool ReconcilePauseWhileStopped() noexcept;
    void Ended(PreviewNativeState observed,
               std::optional<MediaTime> endpoint = std::nullopt) noexcept;
    void Failed() noexcept;

    [[nodiscard]] bool play_requested() const noexcept {
        return desired_playing_;
    }
    [[nodiscard]] bool playing() const noexcept { return actual_playing_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] bool position_pending() const noexcept {
        return desired_position_.has_value() ||
               in_flight_ == PreviewTransportCommandType::Seek;
    }
    /// A requested/in-flight seek owns the logical position until its own
    /// completion. Retain an acknowledged paused/stopped target until Play:
    /// MFPlay may reset its native clock to zero at the exact media endpoint.
    [[nodiscard]] MediaTime ResolvePosition(MediaTime native) const noexcept {
        if (position_override_) return *position_override_;
        // A seek in the final video's duration or the trailing audio can use
        // an earlier native sample. Do not display a position before the
        // acknowledged logical target while the native clock catches up.
        return playback_position_floor_ && native < *playback_position_floor_
            ? *playback_position_floor_ : native;
    }
    [[nodiscard]] bool command_pending() const noexcept {
        return in_flight_ != PreviewTransportCommandType::None ||
               desired_position_.has_value() ||
               desired_playing_ != actual_playing_;
    }
    [[nodiscard]] PreviewTransportCommandType in_flight() const noexcept {
        return in_flight_;
    }
    [[nodiscard]] std::optional<MediaTime> desired_position() const noexcept {
        return desired_position_;
    }
    [[nodiscard]] std::optional<MediaTime> position_override() const noexcept {
        return position_override_;
    }
    [[nodiscard]] std::optional<MediaTime> playback_position_floor() const noexcept {
        return playback_position_floor_;
    }

private:
    bool actual_playing_{false};
    bool desired_playing_{false};
    std::optional<MediaTime> desired_position_;
    std::optional<MediaTime> position_override_;
    std::optional<MediaTime> playback_position_floor_;
    PreviewTransportCommandType in_flight_{PreviewTransportCommandType::None};
    bool ended_{false};
};

}  // namespace easnap::media
