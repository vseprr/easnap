// Easnap - opening a video file the user owns in the trim view (ADR-040).
//
// The coordinator asks for a file with the Open dialog, checks it with the
// probe on a worker thread of this job, and then either begins the trim flow
// with an external source or tells the user why the file cannot open. Nothing
// here writes, moves or deletes the user's file.
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "app/trim_workflow.h"
#include "media/media_interfaces.h"
#include "media/video_probe.h"

namespace easnap::app {

/// Posted by the probe worker when its result can be taken on the UI thread.
/// WPARAM carries the probe's generation.
inline constexpr UINT kMsgVideoProbeCompleted = WM_APP + 18;

/// How long the probe may take before a file counts as unreadable (ADR-040).
inline constexpr std::chrono::seconds kVideoProbeDeadline{5};

struct VideoProbeOutcome final {
    std::wstring path;
    media::VideoProbeResult result;
    /// The probe threw; `result` then refuses the file as damaged.
    bool worker_exception{false};
};

/// One probe at a time on a worker thread, like the recording finalizer: the
/// message is only a wakeup and the result is taken by generation. All public
/// operations belong to the owner (UI) thread. The probe cannot be
/// interrupted; Join waits for the step that runs, which the probe bounds by
/// refusing network and offline files before it reads them.
class VideoProbeJob final {
public:
    using Generation = UINT_PTR;

    VideoProbeJob() = default;
    ~VideoProbeJob();
    VideoProbeJob(const VideoProbeJob&) = delete;
    VideoProbeJob& operator=(const VideoProbeJob&) = delete;

    /// Starts probing `path` with a deadline of `timeout` from now and posts
    /// `message` to `owner` when done. False while a probe is running or when
    /// the worker cannot start.
    [[nodiscard]] bool Start(HWND owner, UINT message, std::wstring path,
                             std::chrono::steady_clock::duration timeout);
    /// The result of `generation` once its worker has finished; otherwise
    /// nothing. A result is taken once.
    [[nodiscard]] std::optional<VideoProbeOutcome> TryTake(Generation generation);
    void Join();
    /// A probe has been started and its result not yet taken.
    [[nodiscard]] bool busy() const noexcept { return worker_.joinable(); }
    /// A probe is still checking its file. Once it has finished, its result
    /// waits for TryTake whether or not its message arrives.
    [[nodiscard]] bool running() const noexcept {
        return worker_.joinable() && completed_.load(std::memory_order_acquire) != generation_;
    }
    [[nodiscard]] Generation generation() const noexcept { return generation_; }

private:
    std::thread worker_;
    Generation generation_{0};
    std::atomic<Generation> completed_{0};
    VideoProbeOutcome outcome_{};
};

/// The string that tells the user why `result` cannot open, with %1 for the
/// file name (ADR-040, "What the user is told"). Pure; one string per answer.
/// An accepted result has no reason and yields the generic failure string.
[[nodiscard]] UINT VideoRefusalMessageId(const media::VideoProbeResult& result) noexcept;

/// The export settings of an opened video: the picture's own size, MP4 unless
/// only GIF can be made, and the GIF quality a recording gets by default.
[[nodiscard]] media::RecordingOptions ExternalTrimOptions(const TrimSource& source) noexcept;

/// True when `path` lies inside `folder`, compared as text: letter case,
/// forward or backward separators, a trailing separator and the "\\?\"
/// prefix make no difference. The folder itself is not inside itself. Pure.
[[nodiscard]] bool PathIsInsideFolder(std::wstring_view path, std::wstring_view folder) noexcept;

/// True when the file at `path` lies inside `data_folder`, Easnap's own data
/// folder, where Easnap deletes its temporary sessions: opened from there, the
/// file could be deleted under the user (ADR-040). A local path and the folder
/// are first resolved to their final paths, so short names, links and
/// substituted drives give the same answer; a network path is never inside
/// and is not touched here (the probe refuses it).
[[nodiscard]] bool IsInsideEasnapFolder(const std::wstring& path, const std::wstring& data_folder);

/// The last component of `path`, as the user sees the file's name.
[[nodiscard]] std::wstring VideoFileDisplayName(const std::wstring& path);

/// The name a trimmed copy of `source_path` is offered under, without an
/// extension: "<name> (trimmed)" in the active language.
[[nodiscard]] std::wstring TrimmedVideoFileName(const std::wstring& source_path);

/// The identity of the file at `path` as the probe records it (volume serial
/// and file identifier, size, modification time); nothing when it cannot be
/// opened for its attributes.
[[nodiscard]] std::optional<media::VideoFileIdentity> ReadVideoFileIdentity(const std::wstring& path);

/// True when `path` names the file `identity` describes: the same volume and
/// file identifier, whatever the path says (a hard link, another spelling).
[[nodiscard]] bool IsSameFile(const std::wstring& path, const media::VideoFileIdentity& identity);

/// The Open dialog's text in the active language; the caller has the tables.
struct OpenVideoDialogText final {
    const wchar_t* title{L""};
    const wchar_t* video_type{L""};
    const wchar_t* all_files_type{L""};
};

/// Asks for one video file. The first filter lists every extension the
/// support table opens; "All files" lets the user pick anything, and the probe
/// then says why it does not open. Declares a ModalLoopScope.
[[nodiscard]] std::optional<std::wstring> ShowOpenVideoDialog(HWND owner,
                                                              const std::wstring& initial_directory,
                                                              const OpenVideoDialogText& text);

}  // namespace easnap::app
