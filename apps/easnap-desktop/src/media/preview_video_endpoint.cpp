#include "media/preview_video_endpoint.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>

namespace easnap::media {

HRESULT ReadPreviewVideoEndpoint(
    const std::wstring& path, MediaTime presentation_duration,
    PreviewVideoEndpoint& endpoint) noexcept {
    endpoint = {};
    if (path.empty() || presentation_duration <= MediaTime::zero()) {
        return E_INVALIDARG;
    }

    constexpr DWORD video_stream =
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    constexpr uint32_t max_read_events = 4'096;
    constexpr ULONGLONG max_scan_ms = 2'000;
    const ULONGLONG scan_start = ::GetTickCount64();
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT result = ::MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(result)) return result;

    Microsoft::WRL::ComPtr<IMFMediaType> native_type;
    result = reader->GetNativeMediaType(video_stream, 0, &native_type);
    if (FAILED(result)) return result;
    GUID major_type{};
    result = native_type->GetGUID(MF_MT_MAJOR_TYPE, &major_type);
    if (FAILED(result)) return result;
    BOOL compressed = FALSE;
    result = native_type->IsCompressedFormat(&compressed);
    if (FAILED(result)) return result;
    if (major_type != MFMediaType_Video || !compressed) {
        return MF_E_INVALIDMEDIATYPE;
    }
    // Selecting the source's native type explicitly prevents decoder creation.
    result = reader->SetCurrentMediaType(video_stream, nullptr, native_type.Get());
    if (FAILED(result)) return result;
    result = reader->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(result)) return result;
    result = reader->SetStreamSelection(video_stream, TRUE);
    if (FAILED(result)) return result;

    PROPVARIANT position{};
    result = ::InitPropVariantFromInt64(presentation_duration.count(), &position);
    if (FAILED(result)) return result;
    result = reader->SetCurrentPosition(GUID_NULL, position);
    ::PropVariantClear(&position);
    if (FAILED(result)) return result;

    PreviewVideoEndpoint candidate;
    LONGLONG last_timestamp = -1;
    LONGLONG video_end = -1;
    for (uint32_t read = 0; read < max_read_events; ++read) {
        if (::GetTickCount64() - scan_start >= max_scan_ms) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        DWORD flags = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        result = reader->ReadSample(video_stream, 0, nullptr, &flags, nullptr,
                                    &sample);
        if (FAILED(result)) return result;
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            return MF_E_INVALID_FILE_FORMAT;
        }
        if ((flags & (MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED |
                      MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) != 0) {
            return MF_E_INVALIDMEDIATYPE;
        }
        if (sample) {
            LONGLONG timestamp = 0;
            LONGLONG duration = 0;
            result = sample->GetSampleTime(&timestamp);
            if (FAILED(result)) return result;
            result = sample->GetSampleDuration(&duration);
            if (FAILED(result)) return result;
            if (timestamp < 0 || duration <= 0 ||
                timestamp >= presentation_duration.count() ||
                duration > (std::numeric_limits<LONGLONG>::max)() - timestamp) {
                return MF_E_INVALID_FILE_FORMAT;
            }
            // Compressed H.264 samples may arrive in decode order. The final
            // displayed frame has the greatest presentation timestamp.
            last_timestamp = (std::max)(last_timestamp, timestamp);
            video_end = (std::max)(video_end, timestamp + duration);
            ++candidate.samples_examined;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            if (last_timestamp < 0) return MF_E_INVALID_FILE_FORMAT;
            candidate.last_frame_position = MediaTime{last_timestamp};
            candidate.video_end = MediaTime{video_end};
            endpoint = candidate;
            return S_OK;
        }
    }
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

}  // namespace easnap::media
