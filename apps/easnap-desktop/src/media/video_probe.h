// Easnap - checks a video file before the trim view opens it (ADR-040).
//
// The probe opens the file on a handle Easnap opened itself, reads it with the
// Media Foundation Source Reader and decodes its first frame. It never writes,
// moves or deletes the file. It blocks: the caller runs it on a worker thread
// and posts the result to its window.
#pragma once

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "media/media_types.h"
#include "media/video_support.h"

namespace easnap::media {

/// Why a video cannot open, one value per answer the user is given
/// (ADR-040, "Probe and reasons"). A later slice maps each to a string.
enum class VideoRefusal {
    None,
    /// Windows has no Media Foundation (N editions without the Media Feature
    /// Pack); the same condition that keeps Record unavailable (ADR-034).
    MediaFoundationUnavailable,
    UnsupportedExtension,
    /// The extension is supported, the video or audio inside is not.
    FormatNotSupported,
    /// The codec is in the table but Windows has no decoder for it, or the file
    /// is HEVC and the HEVC Video Extension is missing.
    NeedsCodecExtension,
    Damaged,
    Protected,
    NoVideo,
    /// See VideoNotSupportedYetCause.
    NotSupportedYet,
    /// Over the H.264 size limit. The trim view still opens: GIF is allowed,
    /// MP4 export is not, and the message says so.
    TooLargeForMp4,
    NetworkOrOffline,
    InUse,
    NotFound,
    NoAccess,
    /// The deadline passed. The caller says "network or offline file" when the
    /// location says so and "damaged or unreadable" otherwise.
    Timeout,
};

enum class VideoNotSupportedYetCause {
    None,
    Rotation,
    PixelAspect,
    Interlaced,
    /// 10-bit or HDR: an H.264 High 10, 4:2:2 or 4:4:4 profile, or a PQ or HLG
    /// transfer function. Windows' H.264 decoder decodes none of them.
    HighBitDepth,
};

/// The probe's steps in the order ADR-040 gives them.
enum class VideoProbeStep {
    MediaFoundation,
    Path,
    Extension,
    OpenFile,
    OpenSource,
    FindVideo,
    ReadFormat,
    ConfigureDecoder,
    DecodeFirstFrame,
    ReadDuration,
    Complete,
};

enum class VideoPathProblem {
    None,
    /// Empty, relative or drive-relative ("C:video.mp4").
    NotAbsolute,
    /// A URL such as "https://..." or "file:///...".
    Url,
    /// A UNC path, a network drive, or a handle that the network redirector
    /// serves.
    Remote,
    /// The drive letter names no drive.
    NoSuchDrive,
    /// A directory, a device or a namespace path that is not a disk file.
    NotRegularFile,
    /// Offline, or a cloud placeholder whose data is not on this PC.
    Offline,
};

/// What identifies the file the user opened, so a later step can tell whether
/// it is still the same file and unchanged.
struct VideoFileIdentity final {
    uint64_t volume_serial{0};
    std::array<uint8_t, 16> file_id{};
    uint64_t size_bytes{0};
    /// Last write time as a FILETIME: 100 ns since 1601-01-01 UTC.
    uint64_t last_write_time{0};

    [[nodiscard]] bool operator==(const VideoFileIdentity&) const noexcept = default;
};

/// Everything the probe learned. The classifier decides from these facts and
/// the HRESULT of the step that stopped the probe.
struct VideoProbeFacts final {
    /// The step that failed, or Complete when every step passed.
    VideoProbeStep step{VideoProbeStep::MediaFoundation};
    /// The deadline had passed when `step` was about to start.
    bool deadline_passed{false};
    VideoPathProblem path_problem{VideoPathProblem::None};
    /// From the extension; empty when the table does not open it.
    std::optional<VideoContainer> container;
    bool extension_known_unsupported{false};
    /// A stream of the file declares MF_SD_PROTECTED: the file needs the
    /// protected media path.
    bool is_protected{false};

    bool has_video{false};
    GUID video_subtype{};
    /// Display size of the native video type.
    uint32_t width{0};
    uint32_t height{0};
    uint32_t frame_rate_numerator{0};
    uint32_t frame_rate_denominator{0};
    /// 1:1 when the file declares none.
    uint32_t pixel_aspect_numerator{1};
    uint32_t pixel_aspect_denominator{1};
    /// MFVideoInterlaceMode of the native type; 0 (unknown) when not declared.
    uint32_t interlace_mode{0};
    /// MFSampleExtension_Interlaced of the first decoded frame. An H.264 file
    /// declares "mixed" in its type, so only the frame tells.
    bool first_frame_interlaced{false};
    /// MF_MT_VIDEO_ROTATION in degrees.
    uint32_t rotation_degrees{0};
    /// MF_MT_MPEG2_PROFILE, the H.264 profile_idc; 0 when not declared.
    uint32_t video_profile{0};
    /// MF_MT_TRANSFER_FUNCTION; 0 (unknown) when not declared.
    uint32_t transfer_function{0};

    bool has_audio{false};
    GUID audio_subtype{};

    MediaTime duration{0};
    VideoFileIdentity identity;
};

struct VideoProbeVerdict final {
    VideoRefusal reason{VideoRefusal::None};
    VideoNotSupportedYetCause cause{VideoNotSupportedYetCause::None};

    [[nodiscard]] bool operator==(const VideoProbeVerdict&) const noexcept = default;
};

/// Pure: decides the answer from the facts and the HRESULT of `facts.step`
/// (S_OK when the step itself succeeded but the facts refuse the file).
[[nodiscard]] VideoProbeVerdict ClassifyVideoProbe(const VideoProbeFacts& facts,
                                                   HRESULT error) noexcept;

/// Pure: the path rules that need no file system access. Only "X:\..." and
/// "\\?\X:\..." are local absolute paths; every other "\\" form is remote or
/// not a disk file.
[[nodiscard]] VideoPathProblem CheckVideoPathSyntax(std::wstring_view path) noexcept;

/// The probe's path step alone: the spelling, the drive and the attributes.
/// None when `path` names an existing regular file on a local drive whose
/// data is on this PC; otherwise the answer the probe gives at that step.
/// Never opens the file, so it costs no more than reading the attributes of a
/// local file.
[[nodiscard]] VideoRefusal CheckVideoLocation(const std::wstring& path);

struct VideoProbeResult final {
    /// The trim view may open the file. `reason` is then None, or
    /// TooLargeForMp4 when only GIF export is possible.
    bool ok{false};
    VideoRefusal reason{VideoRefusal::None};
    VideoNotSupportedYetCause not_supported_yet{VideoNotSupportedYetCause::None};
    /// The failure of the step that stopped the probe; S_OK when the facts
    /// refused the file or the probe succeeded.
    HRESULT error{S_OK};
    VideoProbeFacts facts;
};

/// Checks `path` in ADR-040's order and stops at the first step that refuses
/// it. Blocking; never call it on a window thread. The deadline is checked
/// only between steps (and between the reads of the first frame): opening the
/// file or the source and a single ReadSample are not bounded by it. When the
/// deadline has passed before a step starts, the answer is Timeout; a step
/// that fails is classified by its HRESULT, and a file whose every step
/// succeeded is accepted, however late. Starts COM (multithreaded unless the
/// thread already chose) and Media Foundation for its own duration.
[[nodiscard]] VideoProbeResult ProbeVideoFile(const std::wstring& path,
                                              std::chrono::steady_clock::time_point deadline);

}  // namespace easnap::media
