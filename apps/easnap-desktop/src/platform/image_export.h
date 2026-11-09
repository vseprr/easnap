// Easnap - copying and saving a finished image away from the UI thread.
//
// Encoding a tall image takes seconds. Done on the UI thread, it froze the
// window that asked for it and Windows marked Easnap "Not responding". An
// ImageExportJob encodes and writes the file, or prepares the clipboard
// memory, on a thread of its own; the window that started it is told with
// kMsgImageExportCompleted and finishes on the UI thread: it puts the
// prepared memory on the clipboard, reports the result and closes
// (ADR-031, "Copying and saving a finished image").
#pragma once

#include "platform/platform_utils.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace easnap::platform {

/// Posted to the window that started an image export once its worker has
/// finished: the editor, a pin or the host window (Scrollshot). WPARAM
/// carries the export's generation; the result is taken with TryTake.
inline constexpr UINT kMsgImageExportCompleted = WM_APP + 15;

enum class ImageExportTarget { Clipboard,
                               File };
enum class ImageExportStatus { Succeeded,
                               Failed,
                               Cancelled };

struct ImageExportRequest final {
    /// Shared, not copied: the window that exports keeps the same pixels and
    /// must not change them until the result is taken.
    std::shared_ptr<const BgraImage> image;
    ImageExportTarget target{ImageExportTarget::Clipboard};
    /// File target: the destination that quick save or the Save dialog chose.
    std::wstring path;
};

struct ImageExportResult final {
    ImageExportStatus status{ImageExportStatus::Failed};
    ImageExportTarget target{ImageExportTarget::Clipboard};
    /// Clipboard target: the memory PublishClipboardImage hands over.
    ClipboardImage clipboard;
};

/// One image export on one joinable worker. Every member is called on the
/// owner's UI thread. The message is only a wakeup; the result is read after
/// the worker has been joined, and no timeout abandons a running worker.
class ImageExportJob final {
public:
    using Generation = UINT_PTR;

    ImageExportJob() = default;
    /// Cancels, waits for the worker and drops its result.
    ~ImageExportJob();
    ImageExportJob(const ImageExportJob&) = delete;
    ImageExportJob& operator=(const ImageExportJob&) = delete;

    /// Starts the export and posts `message` to `notify` when it has
    /// finished. False for an invalid request, when this job or any other
    /// image export of the process has not been taken yet, or when no thread
    /// could be started.
    [[nodiscard]] bool Start(HWND notify, UINT message, ImageExportRequest request);
    /// The result of the export with this generation once its worker has
    /// finished, exactly once. Nothing for an older generation or a dropped
    /// result.
    [[nodiscard]] std::optional<ImageExportResult> TryTake(Generation generation);
    /// Asks the worker to stop at the next band of rows. A cancelled export
    /// leaves no file and nothing for the clipboard.
    void Cancel() noexcept;
    /// Blocks until the worker has finished. The result stays to be taken.
    void Wait() noexcept;
    /// Windows is ending the session: a file being written is waited for, so
    /// it lands complete and no staged file is left; a copy is cancelled, as
    /// nothing will read the clipboard, also one already prepared. The
    /// result stays to be taken.
    void SettleForSessionEnd() noexcept;
    /// Cancels, waits and drops the result: its window no longer wants it.
    void Discard() noexcept;

    /// Started and its result not taken or dropped yet.
    [[nodiscard]] bool running() const noexcept { return pending_; }
    [[nodiscard]] Generation generation() const noexcept { return generation_; }

private:
    void Release() noexcept;

    std::thread worker_;
    Generation generation_{0};
    std::atomic<Generation> completed_{0};
    std::atomic<bool> cancel_{false};
    bool pending_{false};
    ImageExportTarget target_{ImageExportTarget::Clipboard};
    ImageExportResult result_{};
};

/// True from the start of an image export until its result is taken or
/// dropped. One export runs in the process at a time: two saves started in
/// the same second would choose the same file name, and two exports of the
/// largest stitched image hold about a gigabyte. Exit is refused and a
/// sign-out held back while one runs.
[[nodiscard]] bool ImageExportActive() noexcept;

}  // namespace easnap::platform
