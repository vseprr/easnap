// Easnap - the screenshot editor's export: annotations composited onto the image.
//
// Annotations stay vector commands while the image is edited; they become
// pixels only here, when the image is copied, saved or pinned. A copy or save
// then runs on a worker (platform/image_export.h); EditorExport holds its
// state while it runs. What the editor does before and after, choosing where
// to save, closing and the notices of the outcome, stays with
// ScreenshotEditor.
#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <vector>

#include "editor/annotation_document.h"
#include "platform/bgra_image.h"
#include "platform/image_export.h"
#include "ui/native_controls.h"

namespace easnap::editor {

/// A picture up to this height is one Direct2D bitmap and one composite pass,
/// as every screenshot is. A taller document is shown and exported in bands,
/// which keeps each bitmap within what Direct2D and WIC accept.
inline constexpr int kWholeBitmapRows = 8192;

/// `annotations` drawn onto `base_image` at its full resolution, opaque.
/// A picture taller than kWholeBitmapRows is composited in bands. The base
/// image itself when there is nothing to draw; empty on failure.
[[nodiscard]] platform::BgraImage CompositeAnnotations(const platform::BgraImage& base_image, const std::vector<Annotation>& annotations);

/// What a running export needs from the editor that owns it.
class EditorExportOwner {
public:
    /// A copy or save started: the islands stop changing the image.
    virtual void ExportStarted() = 0;
    /// It ended: the islands change the image again.
    virtual void ExportEnded() = 0;

protected:
    ~EditorExportOwner() = default;
};

/// A copy or save that finished, with what it was started with.
struct FinishedExport {
    platform::ImageExportResult result;                ///< How it ended.
    std::shared_ptr<const platform::BgraImage> image;  ///< The image it exported.
    ui::ImageSaveDestination destination;              ///< Where a save went; empty for a copy.
};

/// The copy or save of the finished image that is running: its worker, its
/// image and destination, and the notice shown while it lasts.
class EditorExport {
public:
    explicit EditorExport(EditorExportOwner& owner) noexcept : owner_(owner) {}
    EditorExport(const EditorExport&) = delete;
    EditorExport& operator=(const EditorExport&) = delete;

    /// A copy or save is running.
    [[nodiscard]] bool running() const noexcept { return running_; }
    /// Starts exporting `image` to `target` on a worker, which posts
    /// platform::kMsgImageExportCompleted to `notify` when it has finished,
    /// and shows "Copying..." or "Saving..." over `notice_region`. False,
    /// with nothing changed, when another image export is still running.
    bool Start(HWND notify, const RECT& notice_region, platform::ImageExportTarget target,
               std::shared_ptr<const platform::BgraImage> image, ui::ImageSaveDestination destination);
    /// Leaves the busy state: the notice closes, the image and destination are dropped.
    void End();
    /// The result of the export with `generation` once its worker has
    /// finished, which ends the busy state. Nothing for a dropped or older export.
    [[nodiscard]] std::optional<FinishedExport> TakeResult(platform::ImageExportJob::Generation generation);
    /// Cancels the worker, waits for it and drops its result.
    void Discard() noexcept { job_.Discard(); }
    /// Windows is ending the session: a save running is waited for, a copy
    /// is cancelled. Its result still arrives as a message.
    void SettleForSessionEnd() noexcept { job_.SettleForSessionEnd(); }

private:
    EditorExportOwner& owner_;
    /// A copy or save running on `job_`, of `image_`.
    bool running_{false};
    platform::ImageExportJob job_;
    std::shared_ptr<const platform::BgraImage> image_;
    ui::ImageSaveDestination destination_;
    /// "Copying..." or "Saving..." while the export lasts.
    HWND notice_{nullptr};
};

}  // namespace easnap::editor
