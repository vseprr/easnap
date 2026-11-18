#include "ui/image_export_outcome.h"

namespace easnap::ui {

ImageExportOutcome ChooseImageExportOutcome(platform::ImageExportStatus status,
                                            platform::ImageExportTarget target,
                                            bool clipboard_published,
                                            bool quick,
                                            bool can_offer_dialog) noexcept {
    const bool copy = target == platform::ImageExportTarget::Clipboard;
    if (status == platform::ImageExportStatus::Succeeded && (!copy || clipboard_published)) {
        return copy ? ImageExportOutcome::Copied : ImageExportOutcome::Saved;
    }
    if (!copy && status == platform::ImageExportStatus::Failed && quick && can_offer_dialog) {
        return ImageExportOutcome::RetryWithDialog;
    }
    if (status == platform::ImageExportStatus::Cancelled) {
        return ImageExportOutcome::Cancelled;
    }
    // A very tall image can exceed what the clipboard accepts, and a prepared
    // image can still find the clipboard held by another program.
    return copy ? ImageExportOutcome::CopyFailed : ImageExportOutcome::SaveFailed;
}

}  // namespace easnap::ui
