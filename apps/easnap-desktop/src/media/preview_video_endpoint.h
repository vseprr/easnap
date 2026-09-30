// Easnap - exact final video position for disk-backed preview seeking.
#pragma once

#include <cstdint>
#include <string>

#include "media/media_types.h"
#include "platform/platform_utils.h"

namespace easnap::media {

struct PreviewVideoEndpoint final {
    MediaTime last_frame_position{0};
    MediaTime video_end{0};
    uint32_t samples_examined{0};
};

// Reads native compressed samples from the final seekable GOP through EOS.
// No decoder or whole-source scan is used. This does not change presentation
// duration: AAC can legitimately extend beyond the final video frame.
// The caller must initialize COM and Media Foundation. Reads are synchronous;
// an event-count bound and an inter-call deadline bound local scanning work.
// On failure endpoint remains empty; no guessed timestamp is returned.
[[nodiscard]] HRESULT ReadPreviewVideoEndpoint(
    const std::wstring& path, MediaTime presentation_duration,
    PreviewVideoEndpoint& endpoint) noexcept;

}  // namespace easnap::media
