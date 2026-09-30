// Easnap - disk-backed MP4 preview boundary.
#pragma once

#include <memory>
#include <cstdint>
#include <string>

#include "media/media_types.h"
#include "platform/platform_utils.h"

namespace easnap::media {

enum class MediaPreviewState { Closed, Opening, Ready, Playing, Paused, Ended, Failed };

enum class MediaPreviewOperation {
    None,
    Open,
    Play,
    Pause,
    Seek,
    Playback,
};

struct MediaPreviewSnapshot final {
    MediaPreviewState state{MediaPreviewState::Closed};
    MediaTime duration{0};
    HRESULT error{S_OK};
    MediaPreviewOperation failed_operation{MediaPreviewOperation::None};
    bool play_requested{false};
    bool command_pending{false};
    // Coherent logical position: queued/in-flight or acknowledged paused seek
    // target, otherwise native playback position. Pending is not seek success.
    MediaTime position{0};
    bool position_pending{false};
    MediaTime native_position{0};
    bool native_position_valid{false};
    bool native_playing{false};
    uint64_t player_generation{0};
    uint64_t command_sequence{0};
    uint64_t command_age_ms{0};
    uint64_t intent_sequence{0};
    // Only native liveness detection requests recovery. A genuine HRESULT
    // failure, including a source-read timeout, remains terminal.
    bool recovery_requested{false};
    uint32_t backend_process_id{0};
    uint32_t backend_thread_id{0};
};

/// Asynchronous, disk-backed playback into a caller-owned child window.
/// State-change notifications are posted to notify_window using notify_message.
/// Create/open, control, query and destroy on one thread with a Win32 message
/// loop. MFPlay completions and the independent liveness timer use that owner.
class IMediaPreview {
public:
    virtual ~IMediaPreview() = default;

    virtual bool Open(const std::wstring& source_path, HWND video_window,
                      HWND notify_window, UINT notify_message) = 0;
    virtual void Close() noexcept = 0;
    virtual bool Play() = 0;
    virtual bool Pause() = 0;
    virtual bool Seek(MediaTime position) = 0;
    /// Serializes a seek followed by Play; safe from Paused, Ended, or a
    /// still-pending Pause/Seek callback.
    virtual bool PlayFrom(MediaTime position) = 0;
    /// Serializes Pause followed by positioning at the exclusive Out boundary.
    /// With exclusive_end, retain the last actual frame before Out. Ordinary
    /// paused seeks use false and show the frame covering the requested time.
    virtual bool PauseAt(MediaTime position, bool exclusive_end = true) = 0;
    virtual void UpdateVideo() noexcept = 0;
    [[nodiscard]] virtual MediaTime position() const noexcept = 0;
    [[nodiscard]] virtual MediaPreviewSnapshot snapshot() const noexcept = 0;
    /// Bounded, source-path-free transition history and current native snapshot.
    [[nodiscard]] virtual std::wstring diagnostics() const = 0;
};

std::unique_ptr<IMediaPreview> CreateMfPlayPreview();
// Native implementation used only by EasnapPreviewHost. Desktop callers use
// the process-isolated public factory above.
std::unique_ptr<IMediaPreview> CreateInProcessMfPlayPreview();

}  // namespace easnap::media
