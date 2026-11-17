// Easnap - what a finished image copy or save means for the window that ran it.
//
// The editor and the Scrollshot island export the same way (ImageExportJob,
// then the clipboard published on the UI thread) and answer the result the
// same way: success ends the session, a failed quick save offers the Save
// dialog, anything else keeps the image and says why. The choice is pure; each
// window carries it out with its own notices.
#pragma once

#include "platform/image_export.h"

namespace easnap::ui {

/// How a finished copy or save turned out.
enum class ImageExportOutcome {
    Copied,           ///< The image is on the clipboard.
    Saved,            ///< The file was written.
    RetryWithDialog,  ///< Quick save could not write: offer the Save dialog for the same image.
    CopyFailed,       ///< Not on the clipboard (preparing it failed, or the clipboard could not be opened).
    SaveFailed,       ///< The file could not be written.
    Cancelled,        ///< The export was dropped; nothing to report.
};

/// The outcome of an export with `status` and `target`. `clipboard_published`
/// says whether a prepared copy reached the clipboard; `quick` whether quick
/// save chose the file; `can_offer_dialog` whether the window can still open
/// the Save dialog.
[[nodiscard]] ImageExportOutcome ChooseImageExportOutcome(platform::ImageExportStatus status,
                                                          platform::ImageExportTarget target,
                                                          bool clipboard_published,
                                                          bool quick,
                                                          bool can_offer_dialog) noexcept;

}  // namespace easnap::ui
