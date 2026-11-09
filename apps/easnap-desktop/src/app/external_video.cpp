// Easnap - opening a video file the user owns in the trim view (ADR-040).
#include "app/external_video.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <cstring>
#include <limits>
#include <utility>

#include "app/recording_workflow.h"
#include "media/video_support.h"
#include "platform/platform_utils.h"
#include "strings/strings.h"

namespace easnap::app {
namespace {

UINT NotSupportedYetMessageId(media::VideoNotSupportedYetCause cause) noexcept {
    switch (cause) {
        case media::VideoNotSupportedYetCause::Rotation:
            return IDS_VIDEO_REFUSED_ROTATION;
        case media::VideoNotSupportedYetCause::PixelAspect:
            return IDS_VIDEO_REFUSED_PIXEL_ASPECT;
        case media::VideoNotSupportedYetCause::Interlaced:
            return IDS_VIDEO_REFUSED_INTERLACED;
        case media::VideoNotSupportedYetCause::HighBitDepth:
            return IDS_VIDEO_REFUSED_HIGH_BIT_DEPTH;
        case media::VideoNotSupportedYetCause::None:
            break;
    }
    return IDS_VIDEO_REFUSED_NOT_YET;
}

/// Where the probe found the file to be somewhere slow or not on this PC.
bool RemoteLocation(media::VideoPathProblem problem) noexcept {
    return problem == media::VideoPathProblem::Remote || problem == media::VideoPathProblem::Offline ||
           problem == media::VideoPathProblem::Url;
}

}  // namespace

// --- The probe job -------------------------------------------------------------

VideoProbeJob::~VideoProbeJob() { Join(); }

bool VideoProbeJob::Start(HWND owner, UINT message, std::wstring path,
                          std::chrono::steady_clock::duration timeout) {
    if (worker_.joinable() || path.empty() ||
        generation_ == (std::numeric_limits<Generation>::max)()) {
        return false;
    }
    const Generation generation = ++generation_;
    completed_.store(0, std::memory_order_relaxed);
    outcome_ = VideoProbeOutcome{};
    outcome_.path = std::move(path);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    try {
        worker_ = std::thread([this, owner, message, generation, deadline] {
            try {
                outcome_.result = media::ProbeVideoFile(outcome_.path, deadline);
            } catch (...) {
                outcome_.worker_exception = true;
                outcome_.result = media::VideoProbeResult{};
                outcome_.result.reason = media::VideoRefusal::Damaged;
            }
            // Release publishes the outcome; TryTake joins before reading it.
            completed_.store(generation, std::memory_order_release);
            if (::PostMessageW(owner, message, static_cast<WPARAM>(generation), 0) == FALSE) {
                platform::LogMessage(L"[Probe] The probe result could not be posted (error %lu)",
                                     ::GetLastError());
            }
        });
    } catch (...) {
        platform::LogMessage(L"[Probe] The probe worker could not be started");
        return false;
    }
    return true;
}

std::optional<VideoProbeOutcome> VideoProbeJob::TryTake(Generation generation) {
    if (generation == 0 || generation != generation_ ||
        completed_.load(std::memory_order_acquire) != generation || !worker_.joinable()) {
        return std::nullopt;
    }
    worker_.join();
    completed_.store(0, std::memory_order_relaxed);
    std::optional<VideoProbeOutcome> taken{std::move(outcome_)};
    outcome_ = VideoProbeOutcome{};
    return taken;
}

void VideoProbeJob::Join() {
    if (worker_.joinable()) {
        worker_.join();
    }
    completed_.store(0, std::memory_order_relaxed);
}

// --- What the user is told -----------------------------------------------------

UINT VideoRefusalMessageId(const media::VideoProbeResult& result) noexcept {
    switch (result.reason) {
        case media::VideoRefusal::MediaFoundationUnavailable:
            // The same remedy Record names (ADR-034), said of videos.
            return IDS_VIDEO_REFUSED_MEDIA_FOUNDATION;
        case media::VideoRefusal::UnsupportedExtension:
            return IDS_VIDEO_REFUSED_EXTENSION;
        case media::VideoRefusal::FormatNotSupported:
            return IDS_VIDEO_REFUSED_FORMAT;
        case media::VideoRefusal::NeedsCodecExtension:
            return IDS_VIDEO_REFUSED_CODEC_EXTENSION;
        case media::VideoRefusal::Damaged:
            return IDS_VIDEO_REFUSED_DAMAGED;
        case media::VideoRefusal::Protected:
            return IDS_VIDEO_REFUSED_PROTECTED;
        case media::VideoRefusal::NoVideo:
            return IDS_VIDEO_REFUSED_NO_VIDEO;
        case media::VideoRefusal::NotSupportedYet:
            return NotSupportedYetMessageId(result.not_supported_yet);
        case media::VideoRefusal::NetworkOrOffline:
            return IDS_VIDEO_REFUSED_NETWORK;
        case media::VideoRefusal::InUse:
            return IDS_VIDEO_REFUSED_IN_USE;
        case media::VideoRefusal::NotFound:
            return IDS_VIDEO_REFUSED_NOT_FOUND;
        case media::VideoRefusal::NoAccess:
            return IDS_VIDEO_REFUSED_NO_ACCESS;
        case media::VideoRefusal::Timeout:
            // A file that is slow because of where it is, or one that is not.
            return RemoteLocation(result.facts.path_problem) ? IDS_VIDEO_REFUSED_NETWORK
                                                             : IDS_VIDEO_REFUSED_DAMAGED;
        case media::VideoRefusal::None:
        case media::VideoRefusal::TooLargeForMp4:
            break;
    }
    return IDS_VIDEO_OPEN_FAILED;
}

media::RecordingOptions ExternalTrimOptions(const TrimSource& source) noexcept {
    RecordingConfiguration configuration;
    configuration.format = source.mp4_allowed ? media::RecordingFormat::Mp4 : media::RecordingFormat::Gif;
    return MakeRecordingOptions(RECT{0, 0, static_cast<LONG>(source.facts.width), static_cast<LONG>(source.facts.height)},
                                configuration);
}

std::wstring VideoFileDisplayName(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring TrimmedVideoFileName(const std::wstring& source_path) {
    std::wstring name = VideoFileDisplayName(source_path);
    // Only the last extension goes: "clip.final.mp4" becomes "clip.final".
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        name.resize(dot);
    }
    return strings::Format(IDS_EXTERNAL_TRIMMED_FILE_NAME, {name});
}

// --- Easnap's own folder ----------------------------------------------------------

namespace {

/// `path` without a "\\?\" prefix ("\\?\UNC\server" becomes "\\server"),
/// with backward separators and no trailing one.
std::wstring ComparablePath(std::wstring_view path) {
    std::wstring result;
    if (path.size() >= 8 && (path.substr(0, 8) == L"\\\\?\\UNC\\" || path.substr(0, 8) == L"//?/UNC/")) {
        result = L"\\\\";
        result += path.substr(8);
    } else if (path.size() >= 4 && (path.substr(0, 4) == L"\\\\?\\" || path.substr(0, 4) == L"//?/")) {
        result = path.substr(4);
    } else {
        result = path;
    }
    for (wchar_t& character : result) {
        if (character == L'/') {
            character = L'\\';
        }
    }
    while (result.size() > 3 && result.back() == L'\\') {
        result.pop_back();
    }
    return result;
}

/// The path Windows reaches `path` by, or empty when it cannot be opened.
/// Attributes only: no sharing mode applies and nothing is recalled.
std::wstring FinalPath(const std::wstring& path) {
    const HANDLE opened = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (opened == INVALID_HANDLE_VALUE) {
        return {};
    }
    const platform::UniqueHandle handle(opened);
    std::wstring result(MAX_PATH, L'\0');
    DWORD length = ::GetFinalPathNameByHandleW(handle.get(), result.data(), static_cast<DWORD>(result.size()),
                                               FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= result.size()) {
        result.resize(static_cast<size_t>(length) + 1);
        length = ::GetFinalPathNameByHandleW(handle.get(), result.data(), static_cast<DWORD>(result.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    if (length == 0 || length >= result.size()) {
        return {};
    }
    result.resize(length);
    return result;
}

/// The full path as written, or `path` itself when Windows cannot form it.
std::wstring FullPath(const std::wstring& path) {
    const DWORD needed = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return path;
    }
    std::wstring result(needed, L'\0');
    const DWORD length = ::GetFullPathNameW(path.c_str(), needed, result.data(), nullptr);
    if (length == 0 || length >= needed) {
        return path;
    }
    result.resize(length);
    return result;
}

}  // namespace

bool PathIsInsideFolder(std::wstring_view path, std::wstring_view folder) noexcept {
    try {
        const std::wstring inner = ComparablePath(path);
        const std::wstring outer = ComparablePath(folder);
        if (inner.empty() || outer.empty()) {
            return false;
        }
        // "C:\" keeps its separator; any other folder is followed by one.
        const size_t prefix = outer.back() == L'\\' ? outer.size() - 1 : outer.size();
        if (inner.size() <= prefix + 1 || inner[prefix] != L'\\') {
            return false;
        }
        return ::CompareStringOrdinal(inner.c_str(), static_cast<int>(prefix), outer.c_str(),
                                      static_cast<int>(prefix), TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false;  // Out of memory: no answer is not an answer of "inside".
    }
}

bool IsInsideEasnapFolder(const std::wstring& path, const std::wstring& data_folder) {
    if (path.empty() || data_folder.empty() ||
        media::CheckVideoPathSyntax(path) != media::VideoPathProblem::None) {
        return false;  // A network or namespace path; the data folder is local.
    }
    const std::wstring root = ComparablePath(FullPath(path)).substr(0, 3);
    if (root.size() == 3 && ::GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) {
        return false;  // Never opened here: a server may not answer for a long time.
    }
    if (PathIsInsideFolder(FullPath(path), FullPath(data_folder))) {
        return true;
    }
    const std::wstring file = FinalPath(path);
    const std::wstring folder = FinalPath(data_folder);
    return !file.empty() && !folder.empty() && PathIsInsideFolder(file, folder);
}

// --- File identity -------------------------------------------------------------

std::optional<media::VideoFileIdentity> ReadVideoFileIdentity(const std::wstring& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    // Attributes only: no sharing mode applies, so a file another program
    // holds is still identified, and nothing is recalled from the network.
    const HANDLE opened = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (opened == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    const platform::UniqueHandle file(opened);
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(file.get(), &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    // The same identifiers the probe records (media/video_probe.cpp).
    media::VideoFileIdentity identity;
    FILE_ID_INFO id{};
    if (::GetFileInformationByHandleEx(file.get(), FileIdInfo, &id, sizeof(id)) != FALSE) {
        identity.volume_serial = id.VolumeSerialNumber;
        static_assert(sizeof(id.FileId.Identifier) == sizeof(identity.file_id));
        std::memcpy(identity.file_id.data(), id.FileId.Identifier, identity.file_id.size());
    } else {
        identity.volume_serial = information.dwVolumeSerialNumber;
        const uint64_t index =
            (static_cast<uint64_t>(information.nFileIndexHigh) << 32u) | information.nFileIndexLow;
        std::memcpy(identity.file_id.data(), &index, sizeof(index));
    }
    identity.size_bytes =
        (static_cast<uint64_t>(information.nFileSizeHigh) << 32u) | information.nFileSizeLow;
    identity.last_write_time =
        (static_cast<uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32u) |
        information.ftLastWriteTime.dwLowDateTime;
    return identity;
}

bool IsSameFile(const std::wstring& path, const media::VideoFileIdentity& identity) {
    const std::optional<media::VideoFileIdentity> other = ReadVideoFileIdentity(path);
    return other.has_value() && other->volume_serial == identity.volume_serial &&
           other->file_id == identity.file_id;
}

// --- The Open dialog -----------------------------------------------------------

std::optional<std::wstring> ShowOpenVideoDialog(HWND owner, const std::wstring& initial_directory,
                                                const OpenVideoDialogText& text) {
    platform::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    // "*.mp4;*.m4v;..." from the support table: nothing else lists them.
    std::wstring patterns;
    for (const std::wstring_view extension : media::SupportedVideoExtensions()) {
        if (!patterns.empty()) {
            patterns += L';';
        }
        patterns += L'*';
        patterns += extension;
    }
    const COMDLG_FILTERSPEC filters[] = {
        {text.video_type, patterns.c_str()},
        {text.all_files_type, L"*.*"},
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetTitle(text.title);
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    }
    if (!initial_directory.empty()) {
        platform::ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(initial_directory.c_str(), nullptr,
                                                    IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    } else if (const auto videos = platform::KnownFolder(FOLDERID_Videos)) {
        platform::ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(videos->c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetDefaultFolder(folder.Get());
        }
    }
    HRESULT result = E_FAIL;
    {
        const platform::ModalLoopScope modal;
        result = dialog->Show(owner);
    }
    platform::ComPtr<IShellItem> item;
    if (FAILED(result) || FAILED(dialog->GetResult(&item))) {
        return std::nullopt;
    }
    PWSTR raw_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || raw_path == nullptr) {
        return std::nullopt;
    }
    std::wstring chosen(raw_path);
    ::CoTaskMemFree(raw_path);
    return chosen;
}

}  // namespace easnap::app
