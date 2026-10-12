// Easnap - placement of the Scrollshot island and live preview.
//
// Pure geometry over virtual-screen pixels. A surface that sits inside the
// capture frame would cover what is being captured and would take the wheel
// and the pointer away from the content, so both surfaces go outside the
// frame whenever any monitor has room for them.
#pragma once

#include <vector>

#include "platform/platform_utils.h"

namespace easnap::ui {

struct ScrollshotPlacement {
    RECT island{};
    RECT preview{};
    /// The surface could only be placed over the frame. Such a preview must be
    /// transparent to input; the island stays clickable because the four
    /// actions must remain reachable.
    bool island_over_frame{false};
    bool preview_over_frame{false};
};

/// `island` is the island's outer size. The preview is `preview_width` wide
/// and as tall as the space next to the frame allows, up to
/// `preview_max_height`. `gap` separates a surface from the frame and from a
/// work-area edge.
[[nodiscard]] ScrollshotPlacement PlaceScrollshotSurfaces(
    const RECT& frame, const std::vector<platform::MonitorInfo>& monitors, SIZE island,
    LONG preview_width, LONG preview_max_height, LONG gap);

}  // namespace easnap::ui
