// Easnap - core media type aliases shared across the media subsystem.
//
// This header exists to break the include cycle between audio_format.h and
// media_interfaces.h: both need MediaTime, but media_interfaces.h also needs
// AudioPacket. Keeping the alias here lets each header include only what it
// requires.
#pragma once

#include <chrono>
#include <cstdint>

namespace easnap::media {

/// Non-negative, session-relative monotonic media time in Media Foundation's
/// 100-nanosecond units. Wall-clock time must never be stored in this type.
using MediaTime = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;

}  // namespace easnap::media
