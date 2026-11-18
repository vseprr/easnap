#include "app/scrollshot_status.h"

#include "string_ids.h"

namespace easnap::app {

ScrollshotStatus ChooseScrollshotStatus(std::optional<scrollshot::ScrollshotCondition> capture,
                                        ScrollshotNotice notice) noexcept {
    const bool capturing = capture.has_value();
    UINT message = capturing ? IDS_SCROLLSHOT_STATUS_SCROLL : IDS_SCROLLSHOT_STATUS_READY;
    bool warning = false;
    bool following = true;
    if (capturing) {
        following = *capture == scrollshot::ScrollshotCondition::Following;
        switch (*capture) {
            case scrollshot::ScrollshotCondition::Lost:
                message = IDS_SCROLLSHOT_STATUS_LOST;
                warning = true;
                break;
            case scrollshot::ScrollshotCondition::AtLimit:
                message = IDS_SCROLLSHOT_STATUS_LIMIT;
                break;
            case scrollshot::ScrollshotCondition::CaptureFailed:
                message = IDS_SCROLLSHOT_STATUS_CAPTURE_FAILED;
                warning = true;
                break;
            case scrollshot::ScrollshotCondition::Following:
                break;
        }
    }
    switch (notice) {
        case ScrollshotNotice::WheelBlocked:
            if (capturing && following) { message = IDS_SCROLLSHOT_STATUS_WHEEL; }
            break;
        case ScrollshotNotice::DisplayChanged: message = IDS_SCROLLSHOT_STATUS_DISPLAY_CHANGED; break;
        case ScrollshotNotice::Stopping:
            message = IDS_SCROLLSHOT_STATUS_STOPPING;
            warning = true;
            break;
        case ScrollshotNotice::Copying: message = IDS_SCROLLSHOT_STATUS_COPYING; break;
        case ScrollshotNotice::Saving: message = IDS_SCROLLSHOT_STATUS_SAVING; break;
        case ScrollshotNotice::CopyFailed:
            message = IDS_SCROLLSHOT_STATUS_COPY_FAILED;
            warning = true;
            break;
        case ScrollshotNotice::SaveFailed:
            message = IDS_SCROLLSHOT_STATUS_SAVE_FAILED;
            warning = true;
            break;
        case ScrollshotNotice::None: break;
    }
    return ScrollshotStatus{message, warning};
}

}  // namespace easnap::app
