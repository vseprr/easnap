#include "media/preview_transport.h"

namespace easnap::media {

void PreviewLiveness::Reset(uint64_t generation) noexcept {
    generation_ = generation;
    command_type_ = PreviewTransportCommandType::None;
    command_sequence_ = 0;
    command_started_ms_ = 0;
    playback_progress_ms_.reset();
    playback_position_.reset();
    playback_desired_.reset();
}

PreviewLivenessFailure PreviewLiveness::Observe(
    uint64_t now_ms, uint64_t generation, uint64_t command_sequence,
    PreviewTransportCommandType in_flight, bool desired_playing,
    bool observed_playing, std::optional<MediaTime> native_position,
    bool position_pending) noexcept {
    if (!generation_ || generation != *generation_) {
        return PreviewLivenessFailure::None;
    }
    if (in_flight != PreviewTransportCommandType::None) {
        if (command_type_ != in_flight || command_sequence_ != command_sequence) {
            command_type_ = in_flight;
            command_sequence_ = command_sequence;
            command_started_ms_ = now_ms;
        }
        playback_progress_ms_.reset();
        playback_position_.reset();
        playback_desired_.reset();
        if (now_ms >= command_started_ms_ &&
            now_ms - command_started_ms_ >= timeout_ms) {
            return PreviewLivenessFailure::CommandTimeout;
        }
        return PreviewLivenessFailure::None;
    }
    // A complete Pause/Seek/Play cycle can fit between periodic samples.
    // Its new native command sequence invalidates the old playback clock even
    // when we never observed the command in flight. User intents alone cannot
    // change this sequence or extend an outstanding command's deadline above.
    if (command_sequence_ != command_sequence) {
        playback_progress_ms_.reset();
        playback_position_.reset();
        playback_desired_.reset();
    }
    command_type_ = PreviewTransportCommandType::None;
    command_sequence_ = command_sequence;
    if ((!desired_playing && !observed_playing) || position_pending) {
        playback_progress_ms_.reset();
        playback_position_.reset();
        playback_desired_.reset();
        return PreviewLivenessFailure::None;
    }
    if (playback_desired_ != desired_playing) {
        playback_desired_ = desired_playing;
        playback_progress_ms_ = now_ms;
        playback_position_.reset();
    }
    if (desired_playing && observed_playing && native_position) {
        if (!playback_position_) {
            playback_position_ = native_position;
        } else if (*native_position > *playback_position_) {
            playback_position_ = native_position;
            playback_progress_ms_ = now_ms;
        }
    }
    // A settled desired/observed mismatch is also a liveness failure. Neither
    // changing the reported playback state nor advancing unwanted playback can
    // keep this deadline alive.
    if (now_ms >= *playback_progress_ms_ &&
        now_ms - *playback_progress_ms_ >= timeout_ms) {
        return PreviewLivenessFailure::PlaybackStalled;
    }
    return PreviewLivenessFailure::None;
}

void PreviewTransport::Reset(bool playing) noexcept {
    actual_playing_ = playing;
    desired_playing_ = playing;
    desired_position_.reset();
    position_override_.reset();
    playback_position_floor_.reset();
    in_flight_ = PreviewTransportCommandType::None;
    ended_ = false;
}

void PreviewTransport::RequestPlay() noexcept { desired_playing_ = true; }

void PreviewTransport::RequestPause() noexcept { desired_playing_ = false; }

void PreviewTransport::RequestSeek(MediaTime position) noexcept {
    desired_position_ = position;
    position_override_ = position;
    playback_position_floor_.reset();
}

void PreviewTransport::RequestPlayFrom(MediaTime position) noexcept {
    RequestSeek(position);
    desired_playing_ = true;
}

void PreviewTransport::RequestPauseAt(MediaTime position) noexcept {
    RequestSeek(position);
    desired_playing_ = false;
}

PreviewTransportCommand PreviewTransport::TakeNext() noexcept {
    if (in_flight_ != PreviewTransportCommandType::None) {
        return {};
    }
    // Quiesce playback before every decoder reposition. Desired Play survives
    // this internal Pause and is reconciled only after the real Seek callback.
    if (actual_playing_ && (!desired_playing_ || desired_position_.has_value())) {
        in_flight_ = PreviewTransportCommandType::Pause;
        return {in_flight_, MediaTime::zero()};
    }
    if (desired_position_.has_value()) {
        const MediaTime position = *desired_position_;
        desired_position_.reset();
        in_flight_ = PreviewTransportCommandType::Seek;
        return {in_flight_, position};
    }
    if (desired_playing_ != actual_playing_) {
        in_flight_ = desired_playing_ ? PreviewTransportCommandType::Play
                                      : PreviewTransportCommandType::Pause;
        return {in_flight_, MediaTime::zero()};
    }
    return {};
}

void PreviewTransport::Observe(PreviewNativeState observed) noexcept {
    actual_playing_ = observed == PreviewNativeState::Playing;
    if (observed != PreviewNativeState::Stopped) ended_ = false;
}

bool PreviewTransport::Complete(PreviewTransportCommandType type,
                                PreviewNativeState observed) noexcept {
    Observe(observed);
    if (type == PreviewTransportCommandType::None || type != in_flight_) {
        return false;
    }
    in_flight_ = PreviewTransportCommandType::None;
    if ((type == PreviewTransportCommandType::Seek ||
        type == PreviewTransportCommandType::Play) &&
        !desired_position_ && observed == PreviewNativeState::Playing) {
        if (position_override_) playback_position_floor_ = position_override_;
        position_override_.reset();
    }
    if (type == PreviewTransportCommandType::Seek) ended_ = false;
    return true;
}

bool PreviewTransport::ReconcilePauseWhileStopped() noexcept {
    if (in_flight_ != PreviewTransportCommandType::Pause) {
        return false;
    }
    actual_playing_ = false;
    in_flight_ = PreviewTransportCommandType::None;
    return true;
}

void PreviewTransport::Ended(PreviewNativeState observed,
                             std::optional<MediaTime> endpoint) noexcept {
    const bool restart_pending = desired_position_.has_value() ||
                                 in_flight_ !=
                                     PreviewTransportCommandType::None ||
                                 (desired_playing_ && !actual_playing_);
    Observe(observed);
    if (observed != PreviewNativeState::Stopped) return;
    ended_ = true;
    if (!restart_pending) {
        desired_playing_ = false;
        playback_position_floor_.reset();
        // Natural EOS may reset MFPlay's native clock. Retain the observed
        // logical end without inventing a seek completion or replacing input.
        if (endpoint) position_override_ = *endpoint;
    }
}

void PreviewTransport::Failed() noexcept {
    actual_playing_ = false;
    desired_playing_ = false;
    desired_position_.reset();
    position_override_.reset();
    playback_position_floor_.reset();
    in_flight_ = PreviewTransportCommandType::None;
    ended_ = false;
}

}  // namespace easnap::media
