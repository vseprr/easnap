// Easnap - the one table of video files Easnap opens (ADR-040).
//
// Extension -> container -> allowed video and audio subtypes. The probe, both
// trim exporters and the installer's "Open with" list read this table; nothing
// else lists extensions or codecs. Widening it is one edit here plus one test
// file.
#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace easnap::media {

enum class VideoContainer { Mpeg4,
                            Asf,
                            Avi };

/// One container row of the table.
struct VideoContainerSupport final {
    VideoContainer container;
    /// MIME type handed to the Media Foundation source resolver. It names the
    /// byte-stream handler without resolving a URL.
    std::wstring_view content_type;
    std::span<const GUID> video_subtypes;
    /// A file without audio is always allowed.
    std::span<const GUID> audio_subtypes;
};

/// Codecs Windows may decode that v1 deliberately does not open. The table
/// names them so a refusal can say "format not supported" instead of
/// "unknown".
enum class UnsupportedVideoCodec { None,
                                   Hevc,
                                   Vp9,
                                   Av1 };

/// Every extension the table opens, lower case with the dot, in table order.
[[nodiscard]] std::span<const std::wstring_view> SupportedVideoExtensions() noexcept;

/// Extensions of containers v1 knows but does not open (".mkv", ".webm").
[[nodiscard]] std::span<const std::wstring_view> KnownUnsupportedVideoExtensions() noexcept;

/// The extension of the last path component, with its dot (".mp4"), or an
/// empty view when the name has none. Pure text; the file is not touched.
[[nodiscard]] std::wstring_view VideoFileExtension(std::wstring_view path) noexcept;

/// The table row for an extension, given with or without the dot and in any
/// case (ASCII letters only, so no locale can change the answer). Null when the
/// table does not open that extension.
[[nodiscard]] const VideoContainerSupport* FindVideoContainer(std::wstring_view extension) noexcept;

/// The table row for the extension of `path`; null when it is not supported.
[[nodiscard]] const VideoContainerSupport* FindVideoContainerForPath(std::wstring_view path) noexcept;

[[nodiscard]] const VideoContainerSupport& VideoContainerEntry(VideoContainer container) noexcept;

/// True for ".mkv" and ".webm", with or without the dot, in any case.
[[nodiscard]] bool IsKnownUnsupportedVideoExtension(std::wstring_view extension) noexcept;

[[nodiscard]] bool IsVideoSubtypeAllowed(VideoContainer container, const GUID& subtype) noexcept;
[[nodiscard]] bool IsAudioSubtypeAllowed(VideoContainer container, const GUID& subtype) noexcept;

/// Which of the codecs v1 does not open a video subtype is; None for any other.
[[nodiscard]] UnsupportedVideoCodec KnownUnsupportedVideoCodec(const GUID& subtype) noexcept;

/// False for a picture no H.264 encoder may produce: up to level 5.2 a frame
/// holds at most 36,864 macroblocks of 16 x 16 pixels and measures at most
/// 8,688 pixels on a side. An MP4 export of a larger video cannot succeed; a
/// GIF export can.
[[nodiscard]] constexpr bool FrameFitsH264(int64_t width, int64_t height) noexcept {
    constexpr int64_t kMaximumMacroblocks = 36864;
    constexpr int64_t kMaximumSide = 8688;
    if (width <= 0 || height <= 0 || width > kMaximumSide || height > kMaximumSide) {
        return false;
    }
    return ((width + 15) / 16) * ((height + 15) / 16) <= kMaximumMacroblocks;
}

}  // namespace easnap::media
