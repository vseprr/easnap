// Easnap - the message line of the Scrollshot island.
//
// Which sentence the island shows, and whether it is a warning, follows from
// the running capture's condition and the notice the coordinator set. The
// choice is pure; the coordinator applies it to the island together with the
// image size.
#pragma once

#include <windows.h>

#include <optional>

#include "scrollshot/scrollshot_session.h"

namespace easnap::app {

/// What the island's message line reports besides the capture's own condition.
enum class ScrollshotNotice { None,
                              WheelBlocked,
                              DisplayChanged,
                              Stopping,
                              Copying,
                              Saving,
                              CopyFailed,
                              SaveFailed };

/// The island's message: a string table identifier and whether it warns.
struct ScrollshotStatus {
    UINT message{0};
    bool warning{false};
};

/// The message for `notice` over the capture's condition. `capture` is the
/// running capture's condition; empty once the capture has stopped.
[[nodiscard]] ScrollshotStatus ChooseScrollshotStatus(std::optional<scrollshot::ScrollshotCondition> capture,
                                                      ScrollshotNotice notice) noexcept;

}  // namespace easnap::app
