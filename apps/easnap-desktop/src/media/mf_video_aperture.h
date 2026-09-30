#pragma once

#include <mfapi.h>

namespace easnap::media {

// Decoded buffers can include codec alignment padding. Only the declared
// display aperture is valid image data (shared by GIF and preview stills).
inline bool ResolveExpectedAperture(IMFMediaType* type, UINT32 buffer_width,
                                    UINT32 buffer_height, UINT32 expected_width,
                                    UINT32 expected_height, UINT32& crop_left,
                                    UINT32& crop_top) noexcept {
    if (!expected_width || !expected_height || expected_width > buffer_width ||
        expected_height > buffer_height) return false;
    if (buffer_width == expected_width && buffer_height == expected_height) {
        crop_left = crop_top = 0;
        return true;
    }
    const GUID* attributes[]{&MF_MT_MINIMUM_DISPLAY_APERTURE,
                             &MF_MT_GEOMETRIC_APERTURE, &MF_MT_PAN_SCAN_APERTURE};
    for (const GUID* attribute : attributes) {
        MFVideoArea aperture{};
        UINT32 bytes = 0;
        if (FAILED(type->GetBlob(*attribute, reinterpret_cast<UINT8*>(&aperture),
                                 sizeof(aperture), &bytes)) || bytes != sizeof(aperture) ||
            aperture.OffsetX.fract || aperture.OffsetY.fract ||
            aperture.OffsetX.value < 0 || aperture.OffsetY.value < 0 ||
            aperture.Area.cx != static_cast<LONG>(expected_width) ||
            aperture.Area.cy != static_cast<LONG>(expected_height)) continue;
        const auto left = static_cast<UINT32>(aperture.OffsetX.value);
        const auto top = static_cast<UINT32>(aperture.OffsetY.value);
        if (left <= buffer_width - expected_width && top <= buffer_height - expected_height) {
            crop_left = left;
            crop_top = top;
            return true;
        }
    }
    return false;
}

}  // namespace easnap::media
