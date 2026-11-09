#include "media/video_support.h"

#include <array>

namespace easnap::media {
namespace {

/// Media Foundation subtypes are a FourCC or wave format tag inside one base
/// GUID (DEFINE_MEDIATYPE_GUID in mfapi.h). Building them here keeps the table
/// a compile-time constant; the Fast test compares each against mfapi.h.
constexpr GUID MediaSubtype(uint32_t code) noexcept {
    return GUID{code, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}

constexpr uint32_t FourCc(const char (&text)[5]) noexcept {
    return static_cast<uint32_t>(static_cast<unsigned char>(text[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(text[1])) << 8u) |
           (static_cast<uint32_t>(static_cast<unsigned char>(text[2])) << 16u) |
           (static_cast<uint32_t>(static_cast<unsigned char>(text[3])) << 24u);
}

constexpr GUID kH264 = MediaSubtype(FourCc("H264"));
constexpr GUID kMp4v = MediaSubtype(FourCc("MP4V"));
constexpr GUID kM4s2 = MediaSubtype(FourCc("M4S2"));
constexpr GUID kMjpg = MediaSubtype(FourCc("MJPG"));
constexpr GUID kWmv3 = MediaSubtype(FourCc("WMV3"));
constexpr GUID kWvc1 = MediaSubtype(FourCc("WVC1"));
constexpr GUID kHevc = MediaSubtype(FourCc("HEVC"));
constexpr GUID kHevcEs = MediaSubtype(FourCc("HEVS"));
constexpr GUID kH265 = MediaSubtype(FourCc("H265"));
constexpr GUID kVp90 = MediaSubtype(FourCc("VP90"));
constexpr GUID kAv1 = MediaSubtype(FourCc("AV01"));

constexpr GUID kPcm = MediaSubtype(0x0001);        // WAVE_FORMAT_PCM
constexpr GUID kMp3 = MediaSubtype(0x0055);        // WAVE_FORMAT_MPEGLAYER3
constexpr GUID kWmaV8 = MediaSubtype(0x0161);      // WAVE_FORMAT_WMAUDIO2
constexpr GUID kWmaV9 = MediaSubtype(0x0162);      // WAVE_FORMAT_WMAUDIO3
constexpr GUID kAac = MediaSubtype(0x1610);        // WAVE_FORMAT_MPEG_HEAAC

constexpr std::array kMpeg4Video{kH264, kMp4v, kM4s2, kMjpg};
constexpr std::array kMpeg4Audio{kAac, kMp3, kPcm};
constexpr std::array kAsfVideo{kWmv3, kWvc1};
constexpr std::array kAsfAudio{kWmaV8, kWmaV9};
constexpr std::array kAviVideo{kH264, kMp4v, kM4s2, kMjpg};
constexpr std::array kAviAudio{kAac, kMp3, kPcm};

constexpr std::array<VideoContainerSupport, 3> kContainers{{
    {VideoContainer::Mpeg4, L"video/mp4", kMpeg4Video, kMpeg4Audio},
    {VideoContainer::Asf, L"video/x-ms-asf", kAsfVideo, kAsfAudio},
    {VideoContainer::Avi, L"video/avi", kAviVideo, kAviAudio},
}};

struct ExtensionRow final {
    std::wstring_view extension;
    VideoContainer container;
};

constexpr std::array<ExtensionRow, 6> kExtensions{{
    {L".mp4", VideoContainer::Mpeg4},
    {L".m4v", VideoContainer::Mpeg4},
    {L".mov", VideoContainer::Mpeg4},
    {L".wmv", VideoContainer::Asf},
    {L".asf", VideoContainer::Asf},
    {L".avi", VideoContainer::Avi},
}};

constexpr std::array<std::wstring_view, kExtensions.size()> kExtensionNames{
    kExtensions[0].extension, kExtensions[1].extension, kExtensions[2].extension,
    kExtensions[3].extension, kExtensions[4].extension, kExtensions[5].extension};

constexpr std::array<std::wstring_view, 2> kKnownUnsupportedExtensions{L".mkv", L".webm"};

constexpr wchar_t AsciiLower(wchar_t character) noexcept {
    return character >= L'A' && character <= L'Z'
               ? static_cast<wchar_t>(character - L'A' + L'a')
               : character;
}

/// `candidate` is a lower-case table extension with its dot; `extension` is
/// what the caller passed, with or without the dot.
bool SameExtension(std::wstring_view candidate, std::wstring_view extension) noexcept {
    if (!extension.empty() && extension.front() == L'.') {
        extension.remove_prefix(1);
    }
    candidate.remove_prefix(1);
    if (extension.size() != candidate.size()) {
        return false;
    }
    for (std::size_t index = 0; index < extension.size(); ++index) {
        if (AsciiLower(extension[index]) != candidate[index]) {
            return false;
        }
    }
    return true;
}

bool Contains(std::span<const GUID> subtypes, const GUID& subtype) noexcept {
    for (const GUID& allowed : subtypes) {
        if (allowed == subtype) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::span<const std::wstring_view> SupportedVideoExtensions() noexcept {
    return kExtensionNames;
}

std::span<const std::wstring_view> KnownUnsupportedVideoExtensions() noexcept {
    return kKnownUnsupportedExtensions;
}

std::wstring_view VideoFileExtension(std::wstring_view path) noexcept {
    const std::size_t separator = path.find_last_of(L"\\/");
    const std::wstring_view name =
        separator == std::wstring_view::npos ? path : path.substr(separator + 1);
    const std::size_t dot = name.find_last_of(L'.');
    return dot == std::wstring_view::npos ? std::wstring_view{} : name.substr(dot);
}

const VideoContainerSupport* FindVideoContainer(std::wstring_view extension) noexcept {
    for (const ExtensionRow& row : kExtensions) {
        if (SameExtension(row.extension, extension)) {
            return &VideoContainerEntry(row.container);
        }
    }
    return nullptr;
}

const VideoContainerSupport* FindVideoContainerForPath(std::wstring_view path) noexcept {
    const std::wstring_view extension = VideoFileExtension(path);
    return extension.empty() ? nullptr : FindVideoContainer(extension);
}

const VideoContainerSupport& VideoContainerEntry(VideoContainer container) noexcept {
    for (const VideoContainerSupport& entry : kContainers) {
        if (entry.container == container) {
            return entry;
        }
    }
    return kContainers.front();
}

bool IsKnownUnsupportedVideoExtension(std::wstring_view extension) noexcept {
    for (const std::wstring_view known : kKnownUnsupportedExtensions) {
        if (SameExtension(known, extension)) {
            return true;
        }
    }
    return false;
}

bool IsVideoSubtypeAllowed(VideoContainer container, const GUID& subtype) noexcept {
    return Contains(VideoContainerEntry(container).video_subtypes, subtype);
}

bool IsAudioSubtypeAllowed(VideoContainer container, const GUID& subtype) noexcept {
    return Contains(VideoContainerEntry(container).audio_subtypes, subtype);
}

UnsupportedVideoCodec KnownUnsupportedVideoCodec(const GUID& subtype) noexcept {
    if (subtype == kHevc || subtype == kHevcEs || subtype == kH265) {
        return UnsupportedVideoCodec::Hevc;
    }
    if (subtype == kVp90) {
        return UnsupportedVideoCodec::Vp9;
    }
    if (subtype == kAv1) {
        return UnsupportedVideoCodec::Av1;
    }
    return UnsupportedVideoCodec::None;
}

}  // namespace easnap::media
