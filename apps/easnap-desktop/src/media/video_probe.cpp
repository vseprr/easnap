#include "media/video_probe.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

#include "platform/platform_utils.h"

namespace easnap::media {
namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

constexpr DWORD kMaximumSourceStreams = 32;
/// ReadSample calls the first frame may take: a decoder can ask for several
/// compressed samples, and a source may deliver stream ticks first.
constexpr int kMaximumFirstFrameReads = 256;
/// H.264 profile_idc values for High 10, High 4:2:2, High 4:4:4 Predictive and
/// CAVLC 4:4:4 Intra.
constexpr uint32_t kHighBitDepthH264Profiles[]{110, 122, 244, 44};

void LogProbe(const wchar_t* operation, HRESULT result) {
    platform::LogHresult(operation, result);
}

/// COM and Media Foundation for the duration of one probe. A thread that has
/// already chosen a single-threaded apartment keeps it; Media Foundation
/// objects are free-threaded.
class ProbeRuntime final {
public:
    ProbeRuntime() noexcept {
        const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized_ = SUCCEEDED(com_result);
        if (!com_initialized_ && com_result != RPC_E_CHANGED_MODE) {
            result_ = com_result;
            return;
        }
        // The probe reads a local file through its own byte stream; it never
        // needs the network stack.
        result_ = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
        mf_started_ = SUCCEEDED(result_);
    }

    ~ProbeRuntime() {
        if (mf_started_) {
            static_cast<void>(::MFShutdown());
        }
        if (com_initialized_) {
            ::CoUninitialize();
        }
    }

    ProbeRuntime(const ProbeRuntime&) = delete;
    ProbeRuntime& operator=(const ProbeRuntime&) = delete;

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool com_initialized_{false};
    bool mf_started_{false};
};

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (valid()) {
            ::CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, INVALID_HANDLE_VALUE); }
    void reset() noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

/// A read-only IStream over a file handle Easnap opened. Media Foundation
/// wraps it in a byte stream (MFCreateMFByteStreamOnStream), so the Source
/// Reader reads exactly the file the probe checked, with no URL resolution.
/// Each read names its offset, so the handle's file pointer is never shared
/// state.
class HandleReadStream final : public IStream {
public:
    HandleReadStream(HANDLE file, uint64_t size) noexcept : file_(file), size_(size) {}

    /// Closes the file now. Media Foundation can keep the stream after a
    /// failed open; the user's file must not stay open with it. Later reads
    /// fail.
    void Close() noexcept {
        const std::scoped_lock lock(mutex_);
        file_.reset();
    }

    HandleReadStream(const HandleReadStream&) = delete;
    HandleReadStream& operator=(const HandleReadStream&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (id == __uuidof(IUnknown) || id == __uuidof(ISequentialStream) ||
            id == __uuidof(IStream)) {
            *object = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG requested, ULONG* read) override {
        if (read != nullptr) {
            *read = 0;
        }
        if (buffer == nullptr && requested != 0) {
            return STG_E_INVALIDPOINTER;
        }
        const std::scoped_lock lock(mutex_);
        if (!file_.valid()) {
            return STG_E_REVERTED;
        }
        if (position_ >= size_ || requested == 0) {
            return requested == 0 ? S_OK : S_FALSE;
        }
        OVERLAPPED offset{};
        offset.Offset = static_cast<DWORD>(position_);
        offset.OffsetHigh = static_cast<DWORD>(position_ >> 32u);
        DWORD transferred = 0;
        if (::ReadFile(file_.get(), buffer, requested, &transferred, &offset) == FALSE) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_HANDLE_EOF) {
                return HRESULT_FROM_WIN32(error);
            }
            transferred = 0;
        }
        position_ += transferred;
        if (read != nullptr) {
            *read = transferred;
        }
        return transferred == requested ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                   ULARGE_INTEGER* new_position) override {
        const std::scoped_lock lock(mutex_);
        int64_t base = 0;
        switch (origin) {
            case STREAM_SEEK_SET:
                base = 0;
                break;
            case STREAM_SEEK_CUR:
                base = static_cast<int64_t>(position_);
                break;
            case STREAM_SEEK_END:
                base = static_cast<int64_t>(size_);
                break;
            default:
                return STG_E_INVALIDFUNCTION;
        }
        if ((move.QuadPart > 0 && base > (std::numeric_limits<int64_t>::max)() - move.QuadPart) ||
            base + move.QuadPart < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<uint64_t>(base + move.QuadPart);
        if (new_position != nullptr) {
            new_position->QuadPart = position_;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* statistics, DWORD) override {
        if (statistics == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        *statistics = {};
        statistics->type = STGTY_STREAM;
        statistics->cbSize.QuadPart = size_;
        statistics->grfMode = STGM_READ | STGM_SHARE_DENY_WRITE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ~HandleReadStream() = default;

    UniqueHandle file_;
    const uint64_t size_;
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    uint64_t position_{0};
};

bool IsAsciiLetter(wchar_t character) noexcept {
    return (character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z');
}

bool IsSeparator(wchar_t character) noexcept {
    return character == L'\\' || character == L'/';
}

/// RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ). Two characters at
/// least, so "C:" stays a drive.
bool IsUrlScheme(std::wstring_view text) noexcept {
    if (text.size() < 2 || !IsAsciiLetter(text.front())) {
        return false;
    }
    for (const wchar_t character : text) {
        if (!IsAsciiLetter(character) && !(character >= L'0' && character <= L'9') &&
            character != L'+' && character != L'-' && character != L'.') {
            return false;
        }
    }
    return true;
}

bool IsFieldInterlaced(uint32_t interlace_mode) noexcept {
    switch (interlace_mode) {
        case MFVideoInterlace_FieldInterleavedUpperFirst:
        case MFVideoInterlace_FieldInterleavedLowerFirst:
        case MFVideoInterlace_FieldSingleUpper:
        case MFVideoInterlace_FieldSingleLower:
            return true;
        default:
            return false;
    }
}

bool IsHighBitDepth(const VideoProbeFacts& facts) noexcept {
    if (facts.transfer_function == MFVideoTransFunc_2084 ||
        facts.transfer_function == MFVideoTransFunc_HLG) {
        return true;
    }
    if (facts.video_subtype != MFVideoFormat_H264) {
        return false;
    }
    for (const uint32_t profile : kHighBitDepthH264Profiles) {
        if (facts.video_profile == profile) {
            return true;
        }
    }
    return false;
}

bool IsCodecMissing(HRESULT error) noexcept {
    return error == MF_E_TOPO_CODEC_NOT_FOUND || error == MF_E_INVALIDMEDIATYPE;
}

bool IsProtectionError(HRESULT error) noexcept {
    switch (error) {
        case MF_E_DRM_UNSUPPORTED:
        case MF_E_LICENSE_INCORRECT_RIGHTS:
        case MF_E_LICENSE_OUTOFDATE:
        case MF_E_LICENSE_REQUIRED:
        case MF_E_DRM_HARDWARE_INCONSISTENT:
        case MF_E_NO_CONTENT_PROTECTION_MANAGER:
        case MF_E_LICENSE_RESTORE_NO_RIGHTS:
        case MF_E_BACKUP_RESTRICTED_LICENSE:
        case MF_E_LICENSE_RESTORE_NEEDS_INDIVIDUALIZATION:
        case MF_E_COMPONENT_REVOKED:
        case MF_E_POLICY_UNSUPPORTED:
        case MF_E_CONTENT_PROTECTION_SYSTEM_NOT_ENABLED:
        case MF_E_UNSUPPORTED_CONTENT_PROTECTION_SYSTEM:
            return true;
        default:
            return false;
    }
}

/// The answer for a Windows file-system error, from opening the file or from
/// a read that Media Foundation passed on. None for any other error.
VideoRefusal FileSystemRefusal(HRESULT error) noexcept {
    if (HRESULT_FACILITY(error) != FACILITY_WIN32 || SUCCEEDED(error)) {
        return VideoRefusal::None;
    }
    switch (HRESULT_CODE(error)) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_NOT_READY:
        case ERROR_DEV_NOT_EXIST:
            return VideoRefusal::NotFound;
        case ERROR_ACCESS_DENIED:
            return VideoRefusal::NoAccess;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return VideoRefusal::InUse;
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_NETNAME_DELETED:
        case ERROR_UNEXP_NET_ERR:
        case ERROR_NETWORK_UNREACHABLE:
        case ERROR_HOST_UNREACHABLE:
        case ERROR_NETWORK_ACCESS_DENIED:
        case ERROR_CANT_ACCESS_FILE:
        case ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING:
        case ERROR_FILE_OFFLINE:
            return VideoRefusal::NetworkOrOffline;
        default:
            return VideoRefusal::None;
    }
}

VideoRefusal PathRefusal(VideoPathProblem problem) noexcept {
    switch (problem) {
        case VideoPathProblem::None:
            return VideoRefusal::None;
        case VideoPathProblem::NotAbsolute:
        case VideoPathProblem::NoSuchDrive:
            return VideoRefusal::NotFound;
        case VideoPathProblem::Url:
        case VideoPathProblem::Remote:
        case VideoPathProblem::Offline:
            return VideoRefusal::NetworkOrOffline;
        case VideoPathProblem::NotRegularFile:
            return VideoRefusal::UnsupportedExtension;
    }
    return VideoRefusal::Damaged;
}

/// What the video and audio formats alone decide, once they are known.
std::optional<VideoProbeVerdict> FormatVerdict(const VideoProbeFacts& facts,
                                               HRESULT error) noexcept {
    if (!facts.container.has_value()) {
        return VideoProbeVerdict{VideoRefusal::UnsupportedExtension};
    }
    if (!IsVideoSubtypeAllowed(*facts.container, facts.video_subtype)) {
        // HEVC is refused either way; the wording tells the user whether
        // installing the extension is what Windows lacks (ADR-040).
        if (KnownUnsupportedVideoCodec(facts.video_subtype) == UnsupportedVideoCodec::Hevc &&
            facts.step == VideoProbeStep::ConfigureDecoder && IsCodecMissing(error)) {
            return VideoProbeVerdict{VideoRefusal::NeedsCodecExtension};
        }
        return VideoProbeVerdict{VideoRefusal::FormatNotSupported};
    }
    if (facts.has_audio && !IsAudioSubtypeAllowed(*facts.container, facts.audio_subtype)) {
        return VideoProbeVerdict{VideoRefusal::FormatNotSupported};
    }
    const auto not_yet = [](VideoNotSupportedYetCause cause) {
        return VideoProbeVerdict{VideoRefusal::NotSupportedYet, cause};
    };
    if (facts.rotation_degrees % 360u != 0) {
        return not_yet(VideoNotSupportedYetCause::Rotation);
    }
    if (facts.pixel_aspect_numerator != 0 && facts.pixel_aspect_denominator != 0 &&
        facts.pixel_aspect_numerator != facts.pixel_aspect_denominator) {
        return not_yet(VideoNotSupportedYetCause::PixelAspect);
    }
    if (IsFieldInterlaced(facts.interlace_mode) || facts.first_frame_interlaced) {
        return not_yet(VideoNotSupportedYetCause::Interlaced);
    }
    if (IsHighBitDepth(facts)) {
        return not_yet(VideoNotSupportedYetCause::HighBitDepth);
    }
    return std::nullopt;
}

/// The refusal for a failed step whose error names no more specific cause.
VideoProbeVerdict FailureVerdict(HRESULT error) noexcept {
    if (IsProtectionError(error)) {
        return {VideoRefusal::Protected};
    }
    const VideoRefusal file_system = FileSystemRefusal(error);
    return {file_system != VideoRefusal::None ? file_system : VideoRefusal::Damaged};
}

// ---------------------------------------------------------------------------
// The probe's steps
// ---------------------------------------------------------------------------

struct ProbeState final {
    VideoProbeFacts facts;
    Clock::time_point deadline;
};

VideoProbeResult Finish(ProbeState& state, VideoProbeStep step, HRESULT error) {
    state.facts.step = step;
    const VideoProbeVerdict verdict = ClassifyVideoProbe(state.facts, error);
    VideoProbeResult result;
    result.ok = verdict.reason == VideoRefusal::None ||
                verdict.reason == VideoRefusal::TooLargeForMp4;
    result.reason = verdict.reason;
    result.not_supported_yet = verdict.cause;
    result.error = error;
    result.facts = state.facts;
    if (result.ok) {
        platform::LogMessage(L"[Probe] Accepted a %ux%u video%s (reason %d)",
                             state.facts.width, state.facts.height,
                             state.facts.has_audio ? L" with audio" : L"",
                             static_cast<int>(verdict.reason));
    } else {
        platform::LogMessage(L"[Probe] Refused at step %d with reason %d (cause %d, 0x%08X)",
                             static_cast<int>(step), static_cast<int>(verdict.reason),
                             static_cast<int>(verdict.cause), static_cast<unsigned>(error));
    }
    return result;
}

/// True when the deadline has passed before `step`; the probe then stops.
bool Expired(ProbeState& state, VideoProbeStep step) noexcept {
    if (Clock::now() < state.deadline) {
        return false;
    }
    state.facts.step = step;
    state.facts.deadline_passed = true;
    return true;
}

HRESULT LastErrorResult() noexcept {
    const DWORD error = ::GetLastError();
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}

/// Drive type and attributes, read without opening the file: opening a cloud
/// placeholder or an offline file can start a recall from the network.
HRESULT CheckLocation(const std::wstring& path, VideoProbeFacts& facts) {
    const bool namespaced = path.size() > 4 && path[0] == L'\\';
    const std::wstring root{namespaced ? path.substr(4, 2) + L"\\" : path.substr(0, 2) + L"\\"};
    switch (::GetDriveTypeW(root.c_str())) {
        case DRIVE_REMOTE:
            facts.path_problem = VideoPathProblem::Remote;
            return S_OK;
        case DRIVE_NO_ROOT_DIR:
            facts.path_problem = VideoPathProblem::NoSuchDrive;
            return S_OK;
        default:
            break;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
        return LastErrorResult();
    }
    if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) != 0) {
        facts.path_problem = VideoPathProblem::NotRegularFile;
    } else if ((data.dwFileAttributes &
                (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                 FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0) {
        facts.path_problem = VideoPathProblem::Offline;
    }
    return S_OK;
}

/// Checks the opened handle and records the file's identity.
HRESULT InspectOpenFile(HANDLE file, VideoProbeFacts& facts) {
    if (::GetFileType(file) != FILE_TYPE_DISK) {
        facts.path_problem = VideoPathProblem::NotRegularFile;
        return S_OK;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(file, &information) == FALSE) {
        return LastErrorResult();
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        facts.path_problem = VideoPathProblem::NotRegularFile;
        return S_OK;
    }
    if ((information.dwFileAttributes &
         (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
          FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0) {
        facts.path_problem = VideoPathProblem::Offline;
        return S_OK;
    }
    // Only a file the network redirector serves answers this query; a local
    // path can still lead there through a link or a substituted drive.
    FILE_REMOTE_PROTOCOL_INFO protocol{};
    if (::GetFileInformationByHandleEx(file, FileRemoteProtocolInfo, &protocol,
                                       sizeof(protocol)) != FALSE) {
        facts.path_problem = VideoPathProblem::Remote;
        return S_OK;
    }

    VideoFileIdentity& identity = facts.identity;
    FILE_ID_INFO id{};
    if (::GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) != FALSE) {
        identity.volume_serial = id.VolumeSerialNumber;
        static_assert(sizeof(id.FileId.Identifier) == sizeof(identity.file_id));
        std::memcpy(identity.file_id.data(), id.FileId.Identifier, identity.file_id.size());
    } else {
        // File systems without 128-bit identifiers (FAT) still have the
        // 64-bit index.
        identity.volume_serial = information.dwVolumeSerialNumber;
        const uint64_t index = (static_cast<uint64_t>(information.nFileIndexHigh) << 32u) |
                               information.nFileIndexLow;
        std::memcpy(identity.file_id.data(), &index, sizeof(index));
    }
    identity.size_bytes = (static_cast<uint64_t>(information.nFileSizeHigh) << 32u) |
                          information.nFileSizeLow;
    identity.last_write_time =
        (static_cast<uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32u) |
        information.ftLastWriteTime.dwLowDateTime;
    return S_OK;
}

/// Closes the probe's stream when the probe returns, after the reader that
/// uses it is gone.
class StreamCloser final {
public:
    explicit StreamCloser(HandleReadStream* stream) noexcept : stream_(stream) {}
    ~StreamCloser() {
        if (stream_ != nullptr) {
            stream_->Close();
        }
    }
    StreamCloser(const StreamCloser&) = delete;
    StreamCloser& operator=(const StreamCloser&) = delete;

private:
    HandleReadStream* stream_;
};

HRESULT OpenSourceReader(IStream* stream, const VideoContainerSupport& container,
                         ComPtr<IMFSourceReader>& reader) {
    ComPtr<IMFByteStream> byte_stream;
    HRESULT result = ::MFCreateMFByteStreamOnStream(stream, &byte_stream);
    if (FAILED(result)) {
        return result;
    }
    // The MIME type picks the container's byte-stream handler. Without it the
    // resolver would still try every handler on the content.
    ComPtr<IMFAttributes> stream_attributes;
    if (SUCCEEDED(byte_stream.As(&stream_attributes))) {
        const std::wstring content_type{container.content_type};
        static_cast<void>(
            stream_attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, content_type.c_str()));
    }

    ComPtr<IMFAttributes> attributes;
    result = ::MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    }
    if (SUCCEEDED(result)) {
        // RGB32 output for a decoder that offers no NV12, as the exporters'
        // fallback does.
        result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = ::MFCreateSourceReaderFromByteStream(byte_stream.Get(), attributes.Get(), &reader);
    }
    return result;
}

/// What MFRequireProtectedEnvironment answers, read through the reader: a
/// presentation needs the protected media path when any stream says so. The
/// function itself lives in mf.dll, which Easnap does not import (ADR-034).
bool RequiresProtection(IMFSourceReader* reader) {
    for (DWORD stream = 0; stream < kMaximumSourceStreams; ++stream) {
        PROPVARIANT value;
        ::PropVariantInit(&value);
        const HRESULT result = reader->GetPresentationAttribute(stream, MF_SD_PROTECTED, &value);
        const bool is_protected = SUCCEEDED(result) && value.vt == VT_UI4 && value.ulVal != 0;
        ::PropVariantClear(&value);
        if (result == MF_E_INVALIDSTREAMNUMBER) {
            return false;
        }
        if (is_protected) {
            return true;
        }
    }
    return false;
}

struct Streams final {
    std::optional<DWORD> video;
    std::optional<DWORD> audio;
};

Streams FindStreams(IMFSourceReader* reader, VideoProbeFacts& facts) {
    Streams streams;
    for (DWORD stream = 0; stream < kMaximumSourceStreams; ++stream) {
        ComPtr<IMFMediaType> type;
        const HRESULT result = reader->GetNativeMediaType(stream, 0, &type);
        if (result == MF_E_INVALIDSTREAMNUMBER) {
            break;
        }
        GUID major{};
        GUID subtype{};
        if (FAILED(result) || FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) ||
            FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            continue;
        }
        if (major == MFMediaType_Video && !streams.video.has_value()) {
            streams.video = stream;
            facts.has_video = true;
            facts.video_subtype = subtype;
        } else if (major == MFMediaType_Audio && !streams.audio.has_value()) {
            streams.audio = stream;
            facts.has_audio = true;
            facts.audio_subtype = subtype;
        }
    }
    return streams;
}

HRESULT ReadVideoFormat(IMFSourceReader* reader, DWORD stream, VideoProbeFacts& facts) {
    ComPtr<IMFMediaType> type;
    HRESULT result = reader->GetNativeMediaType(stream, 0, &type);
    if (SUCCEEDED(result)) {
        result = ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &facts.width, &facts.height);
    }
    if (FAILED(result)) {
        return result;
    }
    if (facts.width == 0 || facts.height == 0) {
        return MF_E_INVALIDMEDIATYPE;
    }
    static_cast<void>(::MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                                            &facts.frame_rate_numerator,
                                            &facts.frame_rate_denominator));
    UINT32 numerator = 0;
    UINT32 denominator = 0;
    if (SUCCEEDED(::MFGetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &numerator,
                                        &denominator))) {
        facts.pixel_aspect_numerator = numerator;
        facts.pixel_aspect_denominator = denominator;
    }
    facts.interlace_mode = ::MFGetAttributeUINT32(type.Get(), MF_MT_INTERLACE_MODE, 0);
    facts.rotation_degrees = ::MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_ROTATION, 0);
    facts.video_profile = ::MFGetAttributeUINT32(type.Get(), MF_MT_MPEG2_PROFILE, 0);
    facts.transfer_function = ::MFGetAttributeUINT32(type.Get(), MF_MT_TRANSFER_FUNCTION, 0);
    return S_OK;
}

/// Asks for decoded NV12, then RGB32, as the exporters do. Returns the result
/// of the last attempt.
HRESULT ConfigureDecoder(IMFSourceReader* reader, DWORD stream) {
    HRESULT result = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),
                                                FALSE);
    if (SUCCEEDED(result)) {
        result = reader->SetStreamSelection(stream, TRUE);
    }
    if (FAILED(result)) {
        return result;
    }
    for (const GUID& subtype : {MFVideoFormat_NV12, MFVideoFormat_RGB32}) {
        ComPtr<IMFMediaType> type;
        result = ::MFCreateMediaType(&type);
        if (SUCCEEDED(result)) {
            result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(result)) {
            result = type->SetGUID(MF_MT_SUBTYPE, subtype);
        }
        if (SUCCEEDED(result)) {
            result = reader->SetCurrentMediaType(stream, nullptr, type.Get());
        }
        if (SUCCEEDED(result)) {
            return result;
        }
    }
    return result;
}

HRESULT DecodeFirstFrame(IMFSourceReader* reader, DWORD stream, ProbeState& state) {
    for (int attempt = 0; attempt < kMaximumFirstFrameReads; ++attempt) {
        if (attempt != 0 && Clock::now() >= state.deadline) {
            state.facts.deadline_passed = true;
            return S_OK;
        }
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT result = reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(result)) {
            return result;
        }
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            return E_FAIL;
        }
        if (sample != nullptr) {
            state.facts.first_frame_interlaced =
                ::MFGetAttributeUINT32(sample.Get(), MFSampleExtension_Interlaced, FALSE) != FALSE;
            return S_OK;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            return MF_E_END_OF_STREAM;
        }
    }
    return MF_E_INVALID_STREAM_DATA;
}

HRESULT ReadDuration(IMFSourceReader* reader, VideoProbeFacts& facts) {
    PROPVARIANT value;
    ::PropVariantInit(&value);
    HRESULT result = reader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value);
    if (SUCCEEDED(result)) {
        if (value.vt == VT_UI8 && value.uhVal.QuadPart != 0 &&
            value.uhVal.QuadPart <= static_cast<ULONGLONG>((std::numeric_limits<int64_t>::max)())) {
            facts.duration = MediaTime{static_cast<int64_t>(value.uhVal.QuadPart)};
        } else {
            result = MF_E_INVALID_TIMESTAMP;
        }
    }
    ::PropVariantClear(&value);
    return result;
}

}  // namespace

VideoPathProblem CheckVideoPathSyntax(std::wstring_view path) noexcept {
    if (path.empty() || path.find(L'\0') != std::wstring_view::npos) {
        return VideoPathProblem::NotAbsolute;
    }
    const std::size_t colon = path.find(L':');
    if (colon != std::wstring_view::npos && IsUrlScheme(path.substr(0, colon))) {
        return VideoPathProblem::Url;
    }
    if (path.size() >= 2 && IsSeparator(path[0]) && IsSeparator(path[1])) {
        // "\\?\" and "\\.\" are namespace prefixes; anything else is a server.
        if (path.size() < 4 || (path[2] != L'?' && path[2] != L'.') || !IsSeparator(path[3])) {
            return VideoPathProblem::Remote;
        }
        const std::wstring_view rest = path.substr(4);
        if (rest.size() >= 4 && (rest[0] == L'U' || rest[0] == L'u') &&
            (rest[1] == L'N' || rest[1] == L'n') && (rest[2] == L'C' || rest[2] == L'c') &&
            IsSeparator(rest[3])) {
            return VideoPathProblem::Remote;
        }
        if (path[2] == L'?' && rest.size() >= 3 && IsAsciiLetter(rest[0]) && rest[1] == L':' &&
            rest[2] == L'\\') {
            return VideoPathProblem::None;
        }
        return VideoPathProblem::NotRegularFile;
    }
    if (path.size() >= 3 && IsAsciiLetter(path[0]) && path[1] == L':' && IsSeparator(path[2])) {
        return VideoPathProblem::None;
    }
    return VideoPathProblem::NotAbsolute;
}

VideoRefusal CheckVideoLocation(const std::wstring& path) {
    VideoProbeFacts facts;
    facts.step = VideoProbeStep::Path;
    facts.path_problem = CheckVideoPathSyntax(path);
    HRESULT result = S_OK;
    if (facts.path_problem == VideoPathProblem::None) {
        result = CheckLocation(path, facts);
        if (SUCCEEDED(result) && facts.path_problem == VideoPathProblem::None) {
            return VideoRefusal::None;
        }
    }
    return ClassifyVideoProbe(facts, result).reason;
}

VideoProbeVerdict ClassifyVideoProbe(const VideoProbeFacts& facts, HRESULT error) noexcept {
    if (facts.deadline_passed) {
        return {VideoRefusal::Timeout};
    }
    switch (facts.step) {
        case VideoProbeStep::MediaFoundation:
            return {VideoRefusal::MediaFoundationUnavailable};
        case VideoProbeStep::Path:
        case VideoProbeStep::OpenFile:
            if (facts.path_problem != VideoPathProblem::None) {
                return {PathRefusal(facts.path_problem)};
            }
            return FailureVerdict(error);
        case VideoProbeStep::Extension:
            return {facts.extension_known_unsupported ? VideoRefusal::FormatNotSupported
                                                      : VideoRefusal::UnsupportedExtension};
        case VideoProbeStep::OpenSource:
        case VideoProbeStep::FindVideo:
            if (facts.is_protected) {
                return {VideoRefusal::Protected};
            }
            if (FAILED(error)) {
                return FailureVerdict(error);
            }
            return {facts.step == VideoProbeStep::FindVideo && !facts.has_video
                        ? VideoRefusal::NoVideo
                        : VideoRefusal::Damaged};
        default:
            break;
    }

    // From ReadFormat on, the formats are known and decide first.
    if (facts.is_protected || IsProtectionError(error)) {
        return {VideoRefusal::Protected};
    }
    if (const auto verdict = FormatVerdict(facts, error)) {
        return *verdict;
    }
    if (facts.step == VideoProbeStep::Complete) {
        return {FrameFitsH264(facts.width, facts.height) ? VideoRefusal::None
                                                         : VideoRefusal::TooLargeForMp4};
    }
    if (facts.step == VideoProbeStep::ConfigureDecoder && IsCodecMissing(error)) {
        return {VideoRefusal::NeedsCodecExtension};
    }
    return FailureVerdict(error);
}

VideoProbeResult ProbeVideoFile(const std::wstring& path, Clock::time_point deadline) {
    ProbeState state;
    state.deadline = deadline;
    VideoProbeFacts& facts = state.facts;

    if (!platform::MediaFoundationAvailable()) {
        return Finish(state, VideoProbeStep::MediaFoundation, S_OK);
    }
    const ProbeRuntime runtime;
    if (FAILED(runtime.result())) {
        LogProbe(L"Starting Media Foundation for the video probe", runtime.result());
        return Finish(state, VideoProbeStep::MediaFoundation, runtime.result());
    }

    if (Expired(state, VideoProbeStep::Path)) {
        return Finish(state, VideoProbeStep::Path, S_OK);
    }
    facts.path_problem = CheckVideoPathSyntax(path);
    if (facts.path_problem != VideoPathProblem::None) {
        return Finish(state, VideoProbeStep::Path, S_OK);
    }
    HRESULT result = CheckLocation(path, facts);
    if (FAILED(result) || facts.path_problem != VideoPathProblem::None) {
        return Finish(state, VideoProbeStep::Path, result);
    }

    const VideoContainerSupport* container = FindVideoContainerForPath(path);
    if (container == nullptr) {
        facts.extension_known_unsupported =
            IsKnownUnsupportedVideoExtension(VideoFileExtension(path));
        return Finish(state, VideoProbeStep::Extension, S_OK);
    }
    facts.container = container->container;

    if (Expired(state, VideoProbeStep::OpenFile)) {
        return Finish(state, VideoProbeStep::OpenFile, S_OK);
    }
    // Others may read the file meanwhile, but no one may change, rename or
    // delete it while it is checked. No recall: an offline file stays where
    // it is and the read fails instead of waiting for the network.
    UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_NO_RECALL,
                                    nullptr));
    if (!file.valid()) {
        return Finish(state, VideoProbeStep::OpenFile, LastErrorResult());
    }
    result = InspectOpenFile(file.get(), facts);
    if (FAILED(result) || facts.path_problem != VideoPathProblem::None) {
        return Finish(state, VideoProbeStep::OpenFile, result);
    }

    if (Expired(state, VideoProbeStep::OpenSource)) {
        return Finish(state, VideoProbeStep::OpenSource, S_OK);
    }
    ComPtr<HandleReadStream> stream;
    try {
        stream.Attach(new HandleReadStream(file.get(), facts.identity.size_bytes));
    } catch (const std::bad_alloc&) {
        return Finish(state, VideoProbeStep::OpenSource, E_OUTOFMEMORY);
    }
    static_cast<void>(file.release());
    const StreamCloser closer(stream.Get());
    ComPtr<IMFSourceReader> reader;
    result = OpenSourceReader(stream.Get(), *container, reader);
    if (FAILED(result)) {
        return Finish(state, VideoProbeStep::OpenSource, result);
    }
    facts.is_protected = RequiresProtection(reader.Get());
    if (facts.is_protected) {
        return Finish(state, VideoProbeStep::OpenSource, S_OK);
    }

    if (Expired(state, VideoProbeStep::FindVideo)) {
        return Finish(state, VideoProbeStep::FindVideo, S_OK);
    }
    const Streams streams = FindStreams(reader.Get(), facts);
    if (!streams.video.has_value()) {
        return Finish(state, VideoProbeStep::FindVideo, S_OK);
    }

    if (Expired(state, VideoProbeStep::ReadFormat)) {
        return Finish(state, VideoProbeStep::ReadFormat, S_OK);
    }
    result = ReadVideoFormat(reader.Get(), *streams.video, facts);
    if (FAILED(result)) {
        return Finish(state, VideoProbeStep::ReadFormat, result);
    }
    facts.step = VideoProbeStep::ReadFormat;
    const bool hevc =
        KnownUnsupportedVideoCodec(facts.video_subtype) == UnsupportedVideoCodec::Hevc;
    if (!hevc && FormatVerdict(facts, S_OK).has_value()) {
        return Finish(state, VideoProbeStep::ReadFormat, S_OK);
    }

    if (Expired(state, VideoProbeStep::ConfigureDecoder)) {
        return Finish(state, VideoProbeStep::ConfigureDecoder, S_OK);
    }
    result = ConfigureDecoder(reader.Get(), *streams.video);
    if (FAILED(result) || hevc) {
        // An HEVC file stops here either way; whether its decoder exists only
        // changes what the user is told.
        return Finish(state, VideoProbeStep::ConfigureDecoder, result);
    }

    if (Expired(state, VideoProbeStep::DecodeFirstFrame)) {
        return Finish(state, VideoProbeStep::DecodeFirstFrame, S_OK);
    }
    result = DecodeFirstFrame(reader.Get(), *streams.video, state);
    if (FAILED(result) || facts.deadline_passed) {
        return Finish(state, VideoProbeStep::DecodeFirstFrame, result);
    }

    if (Expired(state, VideoProbeStep::ReadDuration)) {
        return Finish(state, VideoProbeStep::ReadDuration, S_OK);
    }
    result = ReadDuration(reader.Get(), facts);
    if (FAILED(result)) {
        return Finish(state, VideoProbeStep::ReadDuration, result);
    }
    // Every step succeeded: the answer stands even if the last one ended
    // after the deadline.
    return Finish(state, VideoProbeStep::Complete, S_OK);
}

}  // namespace easnap::media
