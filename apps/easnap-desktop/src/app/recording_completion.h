// Easnap - durable recording completion and clipboard lifetime ownership.
#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "media/media_types.h"
#include "media/media_interfaces.h"
#include "media/temp_session.h"

namespace easnap::app {

struct TrimArtifact final {
    std::wstring path;
    media::MediaTime start{0};
    media::MediaTime end{0};

    [[nodiscard]] bool Matches(media::MediaTime requested_start,
                               media::MediaTime requested_end) const noexcept;
    void Clear() noexcept;
};

enum class TrimCloseAction {
    KeepOpen,
    DiscardNow,
    CancelExportThenDiscard,
};

[[nodiscard]] TrimCloseAction PlanTrimClose(bool discard_confirmed,
                                            bool export_running) noexcept;

class IRecordingCompletionBackend {
public:
    virtual ~IRecordingCompletionBackend() = default;

    virtual bool PersistArtifact(const std::wstring& source_path,
                                 const std::wstring& destination_path,
                                 media::RecordingFormat format,
                                 bool replace_existing) = 0;
    virtual bool MoveArtifact(const std::wstring& source_path,
                              const std::wstring& destination_path,
                              media::RecordingFormat format) = 0;
    virtual bool ApplyClipboardSecurity(const std::wstring& path) = 0;
    [[nodiscard]] virtual bool IsNonemptyArtifact(
        const std::wstring& path, media::RecordingFormat format) const = 0;
    virtual bool PutFileOnClipboard(HWND owner,
                                    const std::wstring& path) = 0;
    /// Nullopt means the clipboard could not be inspected; callers must retain
    /// any file whose lifetime might still be owned by it.
    [[nodiscard]] virtual std::optional<std::vector<std::wstring>>
    ClipboardFilePaths() const = 0;
};

[[nodiscard]] std::unique_ptr<IRecordingCompletionBackend>
CreateRecordingCompletionBackend();

[[nodiscard]] bool IsValidRecordingDestination(
    const std::wstring& path, media::RecordingFormat format) noexcept;
[[nodiscard]] bool PersistRecordingFile(const std::wstring& source_path,
                                        const std::wstring& destination_path,
                                        media::RecordingFormat format,
                                        bool replace_existing);
/// Applies a protected DACL granting full control only to the current process
/// user and LocalSystem. No broad group (including Everyone) is added.
[[nodiscard]] bool ApplyClipboardPublicationSecurity(
    const std::wstring& path);

/// Owns files whose path has been published to the Shell clipboard. The
/// recording coordinator continues to own the active source/trim session.
class RecordingCompletionController final {
public:
    RecordingCompletionController(
        media::ITemporarySessionStore& session_store,
        std::unique_ptr<IRecordingCompletionBackend> backend);
    ~RecordingCompletionController();

    RecordingCompletionController(const RecordingCompletionController&) = delete;
    RecordingCompletionController& operator=(
        const RecordingCompletionController&) = delete;

    [[nodiscard]] bool Save(const std::wstring& source_path,
                            const std::wstring& destination_path,
                            media::RecordingFormat format,
                            bool replace_existing);
    [[nodiscard]] bool IsAllowedSaveDestination(
        const std::wstring& destination_path,
        media::RecordingFormat format,
        bool replace_existing) const;
    /// Allocates current-user-readable clipboard publication storage. A trim
    /// exporter may write directly to the returned path.
    [[nodiscard]] std::optional<std::wstring>
    PrepareClipboardDestination(media::RecordingFormat format);
    /// Moves a disposable derived artifact where safe, otherwise copies it.
    /// The returned file remains recoverable even if clipboard publication fails.
    [[nodiscard]] std::optional<std::wstring> StageClipboardArtifact(
        const std::wstring& source_path, media::RecordingFormat format,
        bool allow_move);
    /// Applies the file ACL and publishes the already-prepared non-empty file.
    [[nodiscard]] bool PublishClipboardArtifact(HWND owner,
                                                const std::wstring& path,
                                                media::RecordingFormat format);
    [[nodiscard]] bool IsPreparedClipboardArtifact(
        const std::wstring& path) const noexcept;
    void AbandonPreparedClipboardArtifact();
    [[nodiscard]] bool Discard(const media::TemporarySession& session);

    /// Removes stale sessions while protecting any directory referenced by the
    /// current CF_HDROP payload. If the clipboard is busy, cleanup is deferred.
    int CleanupStaleSessions();
    /// Called for WM_CLIPBOARDUPDATE. Releases the clipboard session only after
    /// the clipboard no longer references its file.
    void OnClipboardChanged();
    /// Preserves a still-referenced clipboard session across process shutdown;
    /// it becomes eligible for protected stale cleanup on the next launch.
    void Shutdown();

    [[nodiscard]] const std::optional<media::TemporarySession>&
    clipboard_session() const noexcept {
        return clipboard_session_;
    }
    [[nodiscard]] const std::optional<media::TemporarySession>&
    prepared_clipboard_session() const noexcept {
        return prepared_clipboard_session_;
    }

private:
    [[nodiscard]] std::optional<bool> ClipboardReferences(
        const std::wstring& path) const;

    media::ITemporarySessionStore& session_store_;
    std::unique_ptr<IRecordingCompletionBackend> backend_;
    std::optional<media::TemporarySession> prepared_clipboard_session_;
    std::wstring prepared_clipboard_path_;
    std::optional<media::TemporarySession> clipboard_session_;
    std::wstring clipboard_path_;
    bool shutdown_{false};
};

/// The dialog's text comes from the caller, which has the string tables.
struct SaveRecordingDialogText {
    const wchar_t* title{L"Save recording"};
    const wchar_t* mp4_type{L"MP4 video"};
    const wchar_t* gif_type{L"Animated GIF"};
};

[[nodiscard]] std::optional<std::wstring> ShowSaveRecordingDialog(
    HWND owner, media::RecordingFormat format,
    const std::wstring& suggested_file_name,
    const std::wstring& initial_directory,
    const SaveRecordingDialogText& text = {});

}  // namespace easnap::app
