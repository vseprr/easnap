#include "media/media_preview.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>

#include "media/mf_video_aperture.h"
#include "ui/paint_buffer.h"
#include "media/preview_transport.h"
#include "media/preview_video_endpoint.h"

namespace easnap::media {
namespace {

using Microsoft::WRL::ComPtr;

MediaPreviewOperation OperationForCommand(
    PreviewTransportCommandType command) noexcept {
    switch (command) {
        case PreviewTransportCommandType::Play:
            return MediaPreviewOperation::Play;
        case PreviewTransportCommandType::Pause:
            return MediaPreviewOperation::Pause;
        case PreviewTransportCommandType::Seek:
            return MediaPreviewOperation::Seek;
        default:
            return MediaPreviewOperation::Playback;
    }
}

const wchar_t* TransportCommandName(
    PreviewTransportCommandType command) noexcept {
    switch (command) {
        case PreviewTransportCommandType::Play:
            return L"play";
        case PreviewTransportCommandType::Pause:
            return L"pause";
        case PreviewTransportCommandType::Seek:
            return L"seek";
        default:
            return L"none";
    }
}

PreviewNativeState NativeState(MFP_MEDIAPLAYER_STATE state) noexcept {
    if (state == MFP_MEDIAPLAYER_STATE_PLAYING) return PreviewNativeState::Playing;
    if (state == MFP_MEDIAPLAYER_STATE_PAUSED) return PreviewNativeState::Paused;
    return PreviewNativeState::Stopped;
}

std::optional<MediaTime> MediaTimeFromVariant(const PROPVARIANT& value) noexcept {
    if (value.vt == VT_I8 && value.hVal.QuadPart >= 0) {
        return MediaTime{value.hVal.QuadPart};
    }
    if (value.vt == VT_UI8 &&
        value.uhVal.QuadPart <=
            static_cast<ULONGLONG>((std::numeric_limits<int64_t>::max)())) {
        return MediaTime{static_cast<int64_t>(value.uhVal.QuadPart)};
    }
    return std::nullopt;
}

// Decode only the GOP surrounding a requested still. Out is half-open: choose
// the greatest actual PTS strictly before it, never a frame-rate approximation.
// The supervised helper owns all synchronous reader calls; work and storage are
// bounded independently of recording duration.
HRESULT DecodePreviewFrame(const std::wstring& path, MediaTime position, bool exclusive_end,
                           platform::BgraImage& image, MediaTime& frame_position,
                           MediaTime* frame_end = nullptr) noexcept {
    constexpr DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 1);
    if (FAILED(result)) return result;
    // Microsoft's Source Reader software conversion is intended for small
    // numbers of frames, such as this still image, rather than video playback.
    result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(result)) return result;
    ComPtr<IMFSourceReader> reader;
    result = ::MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
    if (FAILED(result)) return result;
    result = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(result)) result = reader->SetStreamSelection(video, TRUE);
    ComPtr<IMFMediaType> native_type;
    UINT32 visible_width = 0, visible_height = 0;
    if (SUCCEEDED(result)) result = reader->GetNativeMediaType(video, 0, &native_type);
    if (SUCCEEDED(result)) result = ::MFGetAttributeSize(native_type.Get(), MF_MT_FRAME_SIZE,
                                                         &visible_width, &visible_height);
    ComPtr<IMFMediaType> type;
    if (SUCCEEDED(result)) result = ::MFCreateMediaType(&type);
    if (SUCCEEDED(result)) result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result)) result = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(result)) result = reader->SetCurrentMediaType(video, nullptr, type.Get());
    if (FAILED(result)) return result;
    const MediaTime target{(std::max)(int64_t{0}, position.count() - (exclusive_end ? 1 : 0))};
    if (target > MediaTime::zero()) {
        PROPVARIANT seek{};
        seek.vt = VT_I8;
        seek.hVal.QuadPart = target.count();
        result = reader->SetCurrentPosition(GUID_NULL, seek);
        if (FAILED(result)) return result;
    }
    const ULONGLONG started = ::GetTickCount64();
    ComPtr<IMFSample> sample;
    bool selected = false;
    for (unsigned attempt = 0; attempt < 4'096; ++attempt) {
        if (::GetTickCount64() - started >= 2'000) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        DWORD flags = 0;
        ComPtr<IMFSample> next;
        LONGLONG timestamp = -1;
        result = reader->ReadSample(video, 0, nullptr, &flags, &timestamp, &next);
        if (FAILED(result)) return result;
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) return MF_E_INVALID_FILE_FORMAT;
        if (next) {
            if (timestamp < 0) return MF_E_INVALID_FILE_FORMAT;
            if (timestamp > target.count()) { selected = sample != nullptr; break; }
            sample = std::move(next);
            frame_position = MediaTime{timestamp};
            if (timestamp == target.count()) { selected = true; break; }
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) { selected = sample != nullptr; break; }
    }
    if (!selected) return sample ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : MF_E_INVALID_FILE_FORMAT;
    if (frame_end) {
        LONGLONG duration = 0;
        result = sample->GetSampleDuration(&duration);
        if (FAILED(result) || duration <= 0 || frame_position.count() >
            (std::numeric_limits<int64_t>::max)() - duration) return MF_E_INVALID_FILE_FORMAT;
        *frame_end = frame_position + MediaTime{duration};
    }
    type.Reset();
    result = reader->GetCurrentMediaType(video, &type);
    UINT32 width = 0, height = 0;
    if (SUCCEEDED(result)) result = ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(result)) return result;
    const uint64_t row_bytes = static_cast<uint64_t>(width) * 4;
    if (!width || !height || width > 16'384 || height > 16'384 ||
        row_bytes * height > 256ull * 1024 * 1024) return MF_E_INVALIDMEDIATYPE;
    UINT32 crop_left = 0, crop_top = 0;
    if (!ResolveExpectedAperture(type.Get(), width, height, visible_width, visible_height,
                                 crop_left, crop_top)) return MF_E_INVALIDMEDIATYPE;
    platform::BgraImage candidate;
    try { candidate.Reset(static_cast<int>(visible_width), static_cast<int>(visible_height)); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    ComPtr<IMFMediaBuffer> buffer;
    result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(result)) return result;
    ComPtr<IMF2DBuffer2> two_dimensional;
    BYTE* scanline = nullptr;
    BYTE* beginning = nullptr;
    DWORD length = 0;
    LONG stride = 0;
    const bool lock_2d = SUCCEEDED(buffer.As(&two_dimensional));
    if (lock_2d) {
        result = two_dimensional->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline,
                                            &stride, &beginning, &length);
    } else {
        UINT32 encoded_stride = 0;
        result = type->GetUINT32(MF_MT_DEFAULT_STRIDE, &encoded_stride);
        if (SUCCEEDED(result)) stride = static_cast<LONG>(encoded_stride);
        else result = ::MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride);
        if (SUCCEEDED(result)) result = buffer->Lock(&beginning, nullptr, &length);
        if (SUCCEEDED(result)) {
            const uint64_t offset = stride < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(stride)) * (height - 1) : 0;
            if (offset >= length) { buffer->Unlock(); return MF_E_INVALIDMEDIATYPE; }
            scanline = beginning + offset;
        }
    }
    if (FAILED(result)) return result;
    const int64_t first = scanline - beginning;
    const int64_t last = first + static_cast<int64_t>(stride) * (height - 1);
    const bool valid = (std::min)(first, last) >= 0 &&
        (std::max)(first, last) + static_cast<int64_t>(row_bytes) <= length &&
        (stride < 0 ? -static_cast<int64_t>(stride) : stride) >= static_cast<int64_t>(row_bytes);
    if (valid) {
        for (UINT32 row = 0; row < visible_height; ++row) {
            std::memcpy(candidate.row(static_cast<int>(row)),
                        scanline + static_cast<int64_t>(row + crop_top) * stride + crop_left * 4ull,
                        static_cast<size_t>(visible_width) * 4);
        }
    }
    if (lock_2d) two_dimensional->Unlock2D(); else buffer->Unlock();
    if (!valid) return MF_E_INVALIDMEDIATYPE;
    candidate.MakeOpaque();
    image = std::move(candidate);
    return S_OK;
}

class MfPlayPreview final : public IMediaPreview {
public:
    MfPlayPreview() = default;
    ~MfPlayPreview() override { Close(); }

    bool Open(const std::wstring& source_path, HWND video_window,
              HWND notify_window, UINT notify_message) override {
        Close();
        if (source_path.empty() || video_window == nullptr ||
            notify_window == nullptr || notify_message < WM_APP) {
            SetFailure(E_INVALIDARG, MediaPreviewOperation::Open);
            return false;
        }
        const DWORD attributes = ::GetFileAttributesW(source_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            SetFailure(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
                       MediaPreviewOperation::Open);
            return false;
        }

        HRESULT result = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(result)) {
            SetFailure(result, MediaPreviewOperation::Open);
            platform::LogHresult(L"Starting Media Foundation preview", result);
            return false;
        }
        mf_started_ = true;
        owner_thread_ = ::GetCurrentThreadId();
        source_path_ = source_path;
        video_window_ = video_window;
        notify_window_ = notify_window;
        notify_message_ = notify_message;
        {
            const std::scoped_lock lock(state_mutex_);
            state_ = MediaPreviewState::Opening;
            duration_ = MediaTime::zero();
            error_ = S_OK;
            failed_operation_ = MediaPreviewOperation::None;
            transport_.Reset();
            dispatch_call_active_ = false;
            ++player_generation_;
            liveness_.Reset(player_generation_);
            opening_started_ms_ = ::GetTickCount64();
        }

        if (!CreateOwnerWindow()) {
            SetFailure(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            ClosePlayerResources();
            return false;
        }
        callback_ = new (std::nothrow) Callback(this, player_generation_);
        if (callback_ == nullptr) {
            SetFailure(E_OUTOFMEMORY, MediaPreviewOperation::Open);
            ClosePlayerResources();
            return false;
        }

        if (!CreateNativeVideoWindow()) {
            SetFailure(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            ClosePlayerResources();
            return false;
        }
        result = ::MFPCreateMediaPlayer(
            source_path.c_str(), FALSE, 0,
            callback_, native_video_window_, &player_);
        if (FAILED(result)) {
            SetFailure(result, MediaPreviewOperation::Open);
            platform::LogHresult(L"Opening the MP4 preview", result);
            ClosePlayerResources();
            return false;
        }
        static_cast<void>(player_->SetBorderColor(RGB(0, 0, 0)));
        static_cast<void>(player_->SetAspectRatioMode(MFVideoARMode_PreservePicture));
        return true;
    }

    void Close() noexcept override {
        if (callback_ != nullptr) {
            callback_->Detach();
        }
        ClosePlayerResources();
        const std::scoped_lock lock(state_mutex_);
        state_ = MediaPreviewState::Closed;
        duration_ = MediaTime::zero();
        error_ = S_OK;
        failed_operation_ = MediaPreviewOperation::None;
        transport_.Reset();
        first_frame_pending_ = false;
        dispatch_call_active_ = false;
        recovery_snapshot_.reset();
        source_path_.clear();
        video_window_ = nullptr;
        video_endpoint_.reset();
    }

    bool Play() override {
        {
            const std::scoped_lock lock(state_mutex_);
            if (!CanAcceptTransportCommandLocked()) {
                return false;
            }
            if (transport_.ended() || transport_.position_override() == duration_) {
                pending_exclusive_end_ = false;
                transport_.RequestPlayFrom(MediaTime::zero());
            } else {
                transport_.RequestPlay();
            }
            ++intent_sequence_;
            LogTransportLocked(L"intent-play", 0, S_OK, 0);
        }
        return DriveTransport();
    }

    bool Pause() override {
        {
            const std::scoped_lock lock(state_mutex_);
            if (!CanAcceptTransportCommandLocked()) {
                return false;
            }
            transport_.RequestPause();
            ++intent_sequence_;
            LogTransportLocked(L"intent-pause", 0, S_OK, 0);
        }
        return DriveTransport();
    }

    bool Seek(MediaTime position) override {
        return QueuePosition(position, PositionIntent::PreservePlayback);
    }

    bool PlayFrom(MediaTime position) override {
        return QueuePosition(position, PositionIntent::Play);
    }

    bool PauseAt(MediaTime position, bool exclusive_end = true) override {
        return QueuePosition(position, PositionIntent::Pause, exclusive_end);
    }

    void UpdateVideo() noexcept override {
        RECT bounds{};
        ::GetClientRect(video_window_, &bounds);
        for (HWND child : {native_video_window_, still_frame_window_}) {
            if (!child) continue;
            RECT current{};
            ::GetClientRect(child, &current);
            if (current.right != bounds.right || current.bottom != bounds.bottom) {
                ::SetWindowPos(child, nullptr, 0, 0, bounds.right, bounds.bottom,
                               SWP_NOACTIVATE | SWP_NOZORDER);
                ::InvalidateRect(child, nullptr, FALSE);
            }
        }
        // Parent paints must not invalidate a composited child again. The
        // native child alone calls MFPlay UpdateVideo, inside its BeginPaint /
        // EndPaint pair; the parent DC does not belong to that video surface.
    }

    [[nodiscard]] MediaTime position() const noexcept override {
        return snapshot().position;
    }

    [[nodiscard]] MediaPreviewSnapshot snapshot() const noexcept override {
        const std::scoped_lock lock(state_mutex_);
        if (recovery_snapshot_) return *recovery_snapshot_;
        PROPVARIANT value{};
        const HRESULT result = player_ != nullptr
            ? player_->GetPosition(MFP_POSITIONTYPE_100NS, &value) : E_UNEXPECTED;
        const auto native = SUCCEEDED(result) ? MediaTimeFromVariant(value)
                                              : std::nullopt;
        ::PropVariantClear(&value);
        MFP_MEDIAPLAYER_STATE native_state = MFP_MEDIAPLAYER_STATE_EMPTY;
        if (player_ != nullptr) static_cast<void>(player_->GetState(&native_state));
        const bool starting = first_frame_pending_;
        return {starting ? MediaPreviewState::Opening : state_, duration_, error_, failed_operation_,
                !starting && transport_.play_requested(), starting || transport_.command_pending(),
                starting ? MediaTime::zero() : transport_.ResolvePosition(native.value_or(MediaTime::zero())),
                starting || transport_.position_pending(), native.value_or(MediaTime::zero()),
                native.has_value(), native_state == MFP_MEDIAPLAYER_STATE_PLAYING,
                player_generation_, command_sequence_,
                !first_frame_pending_ && transport_.in_flight() == PreviewTransportCommandType::None ? 0
                    : ::GetTickCount64() - command_started_ms_, intent_sequence_};
    }

    [[nodiscard]] std::wstring diagnostics() const override {
        const std::scoped_lock lock(state_mutex_);
        std::wstring result;
        const uint64_t first = trace_sequence_ > trace_.size()
            ? trace_sequence_ - trace_.size() : 0;
        for (uint64_t index = first; index < trace_sequence_; ++index) {
            result.append(trace_[index % trace_.size()].data());
        }
        std::array<wchar_t, 1024> current{};
        FormatTransportLocked(current.data(), current.size(), L"snapshot", 0,
                              S_OK, 0, trace_sequence_);
        result.append(current.data());
        return result;
    }

private:
    enum class PositionIntent { PreservePlayback, Play, Pause };
    class Callback final : public IMFPMediaPlayerCallback {
    public:
        Callback(MfPlayPreview* owner, uint64_t generation)
            : owner_(owner), generation_(generation) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                                 void** object) override {
            if (object == nullptr) {
                return E_POINTER;
            }
            if (iid == IID_IUnknown || iid == __uuidof(IMFPMediaPlayerCallback)) {
                *object = static_cast<IMFPMediaPlayerCallback*>(this);
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
            const ULONG remaining =
                references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0) {
                delete this;
            }
            return remaining;
        }

        void STDMETHODCALLTYPE OnMediaPlayerEvent(
            MFP_EVENT_HEADER* event) override {
            const std::scoped_lock lock(owner_mutex_);
            if (owner_ != nullptr) {
                owner_->OnPlayerEvent(event, generation_);
            }
        }

        void Detach() noexcept {
            const std::scoped_lock lock(owner_mutex_);
            owner_ = nullptr;
        }

    private:
        std::atomic<ULONG> references_{1};
        std::mutex owner_mutex_;
        MfPlayPreview* owner_{nullptr};
        const uint64_t generation_;
    };

    static LRESULT CALLBACK NativeVideoWindowProc(HWND window, UINT message,
                                                  WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = reinterpret_cast<MfPlayPreview*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<MfPlayPreview*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_SIZE) { ::InvalidateRect(window, nullptr, FALSE); return 0; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            ::BeginPaint(window, &paint);
            if (self && self->player_ && !self->show_still_ && !self->recovery_snapshot_) {
                const HRESULT result = self->player_->UpdateVideo();
                if (FAILED(result)) platform::LogHresult(L"Updating the MP4 preview surface", result);
            }
            ::EndPaint(window, &paint);
            return 0;
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    bool CreateNativeVideoWindow() noexcept {
        constexpr wchar_t name[] = L"EasnapPreviewNativeVideo";
        WNDCLASSW cls{};
        cls.hInstance = ::GetModuleHandleW(nullptr);
        cls.lpfnWndProc = NativeVideoWindowProc;
        cls.lpszClassName = name;
        if (!::RegisterClassW(&cls) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        RECT bounds{};
        ::GetClientRect(video_window_, &bounds);
        native_video_window_ = ::CreateWindowExW(0, name, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, bounds.right, bounds.bottom,
            video_window_, nullptr, cls.hInstance, this);
        return native_video_window_ != nullptr;
    }

    static LRESULT CALLBACK StillFrameWindowProc(HWND window, UINT message,
                                                 WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = reinterpret_cast<MfPlayPreview*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<MfPlayPreview*>(create->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_SIZE) {
            if (self) self->PresentStillFrame(window);
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(window, &paint);
            if (self) self->PaintStillFrame(window, dc);
            ::EndPaint(window, &paint);
            return 0;
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    bool PaintStillFrame(HWND window, HDC dc) noexcept {
        if (!dc || !show_still_) return false;
        RECT client{};
        ::GetClientRect(window, &client);
        // A minimized/empty child has nothing to present. Keep the selected
        // still's visibility state; its next WM_SIZE publishes the bitmap.
        if (client.right <= 0 || client.bottom <= 0) return true;
        const auto& frame = showing_endpoint_ ? endpoint_frame_ : still_frame_;
        if (frame.empty()) {
            SetFailure(MF_E_INVALIDMEDIATYPE, MediaPreviewOperation::Playback);
            Notify();
            return false;
        }
        ui::PaintBuffer paint(dc, client);
        dc = paint.dc();
        ::FillRect(dc, &client, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        const double scale = (std::min)(static_cast<double>(client.right) / frame.width,
                                       static_cast<double>(client.bottom) / frame.height);
        const int width = (std::max)(1, static_cast<int>(frame.width * scale));
        const int height = (std::max)(1, static_cast<int>(frame.height * scale));
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = frame.width;
        bitmap.bmiHeader.biHeight = -frame.height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        const int saved = ::SaveDC(dc);
        ::SetStretchBltMode(dc, HALFTONE);
        ::SetBrushOrgEx(dc, 0, 0, nullptr);
        ::StretchDIBits(dc, (client.right - width) / 2, (client.bottom - height) / 2, width, height,
                        0, 0, frame.width, frame.height, frame.pixels.data(),
                        &bitmap, DIB_RGB_COLORS, SRCCOPY);
        if (saved) ::RestoreDC(dc, saved);
        if (!paint.PresentLayered(window)) {
            const HRESULT result = HRESULT_FROM_WIN32(::GetLastError());
            SetFailure(FAILED(result) ? result : E_FAIL, MediaPreviewOperation::Playback);
            Notify();
            return false;
        }
        return true;
    }

    bool PresentStillFrame(HWND window) noexcept {
        if (!show_still_) return false;
        // UpdateLayeredWindow owns this child's complete bitmap. It does not
        // bootstrap a WM_PAINT while hidden, so submit on handoff and resize.
        // Both this HWND and its DC belong to the helper's owning thread.
        const HDC dc = ::GetDC(window);
        if (!dc) {
            const HRESULT result = HRESULT_FROM_WIN32(::GetLastError());
            SetFailure(FAILED(result) ? result : E_FAIL, MediaPreviewOperation::Playback);
            Notify();
            return false;
        }
        const bool presented = PaintStillFrame(window, dc);
        ::ReleaseDC(window, dc);
        return presented;
    }

    bool CreateStillFrameWindow() noexcept {
        constexpr wchar_t class_name[] = L"EasnapPreviewStillFrame";
        WNDCLASSW window_class{};
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.lpfnWndProc = StillFrameWindowProc;
        window_class.lpszClassName = class_name;
        if (!::RegisterClassW(&window_class) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        RECT bounds{};
        ::GetClientRect(video_window_, &bounds);
        // A separate opaque surface prevents the native/GDI handoff from
        // inheriting stale pixels from the composited trim parent's backing.
        still_frame_window_ = ::CreateWindowExW(WS_EX_LAYERED, class_name, nullptr,
            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, bounds.right, bounds.bottom,
            video_window_, nullptr, window_class.hInstance, this);
        return still_frame_window_ != nullptr;
    }

    void DestroyStillFrameWindow() noexcept {
        if (still_frame_window_) {
            const HWND window = still_frame_window_;
            still_frame_window_ = nullptr;
            ::DestroyWindow(window);
        }
        still_frame_ = {};
        endpoint_frame_ = {};
        cached_still_position_.reset();
        show_still_ = false;
        showing_endpoint_ = false;
    }

    void ShowStillFrame(bool endpoint) noexcept {
        if (show_still_ && showing_endpoint_ && endpoint) return;
        showing_endpoint_ = endpoint;
        show_still_ = true;
        UpdateVideo();
        // Publish the complete independent surface before changing visibility.
        // Redraw suppression alone cannot make a parent-backed GDI paint atomic
        // with EVR's separately composed video surface.
        if (!PresentStillFrame(still_frame_window_)) return;
        ::SetWindowPos(native_video_window_, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER |
                       SWP_HIDEWINDOW | SWP_NOREDRAW);
        ::SetWindowPos(still_frame_window_, HWND_TOP, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW);
        LogTransport(L"present-still", endpoint ? 1u : 0u, S_OK, 0);
    }

    void ScheduleEndpointPresentation() noexcept {
        ::SetThreadpoolWait(presentation_wait_, nullptr, nullptr);
        ::CancelWaitableTimer(presentation_timer_);
        if (!video_endpoint_ || first_frame_pending_ || recovery_snapshot_) return;
        const auto current = snapshot();
        if (!current.play_requested || !current.native_playing || current.command_pending ||
            current.position_pending || !current.native_position_valid) return;
        const auto remaining = video_endpoint_->last_frame_position - current.native_position;
        if (remaining <= MediaTime::zero()) {
            // The final frame already owns this part of the native clock.
            // Cover EVR before it clears its surface at EOS; the transport and
            // any trailing audio continue until the real ENDED callback.
            ShowStillFrame(true);
        } else {
            MediaTime next = remaining;
            if (show_still_ && !showing_endpoint_) {
                const auto first_remaining = cached_still_end_ - current.native_position;
                if (first_remaining <= MediaTime::zero()) {
                    show_still_ = false;
                    ::ShowWindow(still_frame_window_, SW_HIDE);
                    ::UpdateWindow(native_video_window_);
                } else {
                    // Keep the decoded In frame for its actual presentation
                    // interval. A PLAY callback does not mean EVR presented it.
                    next = (std::min)(next, first_remaining);
                }
            }
            LARGE_INTEGER due{};
            due.QuadPart = -next.count();
            // A low-priority WM_TIMER with a 10 ms minimum can miss an entire
            // 60 fps final sample. Wake the owner at the media boundary without
            // changing system timer resolution or touching COM on the waiter.
            ::SetThreadpoolWait(presentation_wait_, presentation_timer_, nullptr);
            if (!::SetWaitableTimerEx(presentation_timer_, &due, 0, nullptr, nullptr, nullptr, 0))
                SetFailure(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Playback);
        }
    }

    HRESULT PrepareStillFrame(MediaTime position, bool exclusive_end,
                              MediaTime& native_target) noexcept {
        if (position > video_endpoint_->last_frame_position ||
            (!exclusive_end && position == video_endpoint_->last_frame_position)) {
            native_target = video_endpoint_->last_frame_position;
            ShowStillFrame(true);
            return S_OK;
        }
        if (cached_still_position_ != position || cached_still_exclusive_ != exclusive_end) {
            const HRESULT result = DecodePreviewFrame(source_path_, position, exclusive_end,
                                                       still_frame_, cached_still_pts_, &cached_still_end_);
            if (FAILED(result)) return result;
            cached_still_position_ = position;
            cached_still_exclusive_ = exclusive_end;
        }
        if (exclusive_end) native_target = cached_still_pts_;
        ShowStillFrame(false);
        return S_OK;
    }

    [[nodiscard]] bool CanAcceptTransportCommandLocked() const noexcept {
        return ::GetCurrentThreadId() == owner_thread_ &&
               player_ != nullptr && duration_ > MediaTime::zero() &&
               state_ != MediaPreviewState::Closed &&
               state_ != MediaPreviewState::Opening &&
               state_ != MediaPreviewState::Failed;
    }

    bool QueuePosition(MediaTime position, PositionIntent intent, bool exclusive_end = false) {
        {
            const std::scoped_lock lock(state_mutex_);
            if (!CanAcceptTransportCommandLocked()) {
                return false;
            }
            const MediaTime bounded =
                (std::clamp)(position, MediaTime::zero(), duration_);
            pending_exclusive_end_ = exclusive_end || bounded == duration_;
            if (bounded == duration_) {
                // The exclusive endpoint has no media to play. Its last video
                // frame is positioned by a real, safely bounded native Seek.
                transport_.RequestPauseAt(bounded);
            } else switch (intent) {
                case PositionIntent::PreservePlayback:
                    transport_.RequestSeek(bounded);
                    break;
                case PositionIntent::Play:
                    transport_.RequestPlayFrom(bounded);
                    break;
                case PositionIntent::Pause:
                    transport_.RequestPauseAt(bounded);
                    break;
            }
            ++intent_sequence_;
            LogTransportLocked(L"intent-position", static_cast<unsigned>(intent), S_OK, 0);
        }
        return DriveTransport();
    }

    bool DriveTransport() noexcept {
        PreviewTransportCommand command;
        {
            const std::scoped_lock lock(state_mutex_);
            if (!CanAcceptTransportCommandLocked()) {
                return false;
            }
            if (dispatch_call_active_) {
                LogTransportLocked(L"wait-dispatch-return", 0, S_OK, 0);
                return true;
            }
            command = transport_.TakeNext();
            if (transport_.ended()) state_ = MediaPreviewState::Ended;
            if (command.type != PreviewTransportCommandType::None) {
                dispatch_call_active_ = true;
                last_command_ = command;
                ++command_sequence_;
                command_started_ms_ = ::GetTickCount64();
                LogTransportLocked(L"dispatch", static_cast<unsigned>(command.type), S_OK, 0);
            } else {
                LogTransportLocked(transport_.command_pending() ? L"wait-owned-callback"
                                   : L"converged", 0, S_OK, 0);
            }
        }
        if (command.type == PreviewTransportCommandType::None) {
            return true;
        }

        HRESULT result = E_UNEXPECTED;
        switch (command.type) {
            case PreviewTransportCommandType::Play:
                ::ShowWindow(native_video_window_, SW_SHOWNOACTIVATE);
                result = player_->Play();
                break;
            case PreviewTransportCommandType::Pause:
                result = player_->Pause();
                break;
            case PreviewTransportCommandType::Seek: {
                PROPVARIANT value;
                ::PropVariantInit(&value);
                value.vt = VT_I8;
                // MFPlay can accept a seek past the final video sample but
                // never complete it, even if AAC still extends the duration.
                // Render the measured final frame; logical target ownership is
                // unchanged and still requires the genuine POSITION_SET event.
                MediaTime native_target = (std::min)(command.position,
                    video_endpoint_->last_frame_position);
                if (pending_exclusive_end_ || show_still_ ||
                    command.position >= video_endpoint_->last_frame_position) {
                    result = PrepareStillFrame(command.position, pending_exclusive_end_, native_target);
                    if (FAILED(result)) break;
                }
                last_native_target_ = native_target;
                value.hVal.QuadPart = native_target.count();
                result = player_->SetPosition(MFP_POSITIONTYPE_100NS, &value);
                break;
            }
            default:
                break;
        }
        LogTransportCommand(command.type, result);
        if (FAILED(result)) {
            if (command.type == PreviewTransportCommandType::Pause &&
                result == MF_E_SESSION_PAUSEWHILESTOPPED) {
                bool reconciled = false;
                {
                    const std::scoped_lock lock(state_mutex_);
                    reconciled = transport_.ReconcilePauseWhileStopped();
                    if (reconciled) {
                        state_ = MediaPreviewState::Paused;
                    }
                    dispatch_call_active_ = false;
                }
                if (reconciled) {
                    platform::LogMessage(
                        L"[Preview] Reconciled Pause because MFPlay was already stopped; pending position intent was retained");
                    return DriveTransport();
                }
            }
            const MediaPreviewOperation operation = state_ == MediaPreviewState::Opening
                ? MediaPreviewOperation::Open : OperationForCommand(command.type);
            SetFailure(result, operation);
            {
                const std::scoped_lock lock(state_mutex_);
                dispatch_call_active_ = false;
            }
            switch (operation) {
                case MediaPreviewOperation::Play:
                    platform::LogHresult(L"Starting MP4 preview playback",
                                         result);
                    break;
                case MediaPreviewOperation::Pause:
                    platform::LogHresult(L"Pausing MP4 preview playback",
                                         result);
                    break;
                case MediaPreviewOperation::Seek:
                    platform::LogHresult(L"Seeking the MP4 preview", result);
                    break;
                default:
                    platform::LogHresult(L"Controlling MP4 preview playback",
                                         result);
                    break;
            }
            Notify();
            return false;
        }
        // A free-threaded completion callback may have run before the async COM
        // method returned. Drive again only after the dispatch call unwinds.
        {
            const std::scoped_lock lock(state_mutex_);
            dispatch_call_active_ = false;
        }
        return DriveTransport();
    }

    [[nodiscard]] MediaPreviewOperation OperationForEventFailure(
        MFP_EVENT_TYPE event_type) const noexcept {
        const PreviewTransportCommandType in_flight = transport_.in_flight();
        if (in_flight != PreviewTransportCommandType::None) {
            return OperationForCommand(in_flight);
        }
        switch (event_type) {
            case MFP_EVENT_TYPE_MEDIAITEM_CREATED:
            case MFP_EVENT_TYPE_MEDIAITEM_SET:
                return MediaPreviewOperation::Open;
            case MFP_EVENT_TYPE_PLAY:
                return MediaPreviewOperation::Play;
            case MFP_EVENT_TYPE_PAUSE:
                return MediaPreviewOperation::Pause;
            case MFP_EVENT_TYPE_POSITION_SET:
                return MediaPreviewOperation::Seek;
            default:
                return MediaPreviewOperation::Playback;
        }
    }

    void OnPlayerEvent(MFP_EVENT_HEADER* event, uint64_t generation) noexcept {
        if (event == nullptr) {
            return;
        }
        struct CallbackScope final {
            explicit CallbackScope(unsigned& depth) : depth_(depth) { ++depth_; }
            ~CallbackScope() { --depth_; }
            unsigned& depth_;
        } callback_scope(callback_depth_);
        {
            const std::scoped_lock lock(state_mutex_);
            if (generation != player_generation_) {
                LogTransportLocked(L"stale-generation", event->eEventType,
                                   event->hrEvent, event->eState);
                return;
            }
            if (state_ == MediaPreviewState::Failed ||
                state_ == MediaPreviewState::Closed) return;
        }
        LogTransportEvent(event->eEventType, event->hrEvent, event->eState);
        if (FAILED(event->hrEvent)) {
            if (event->eEventType == MFP_EVENT_TYPE_PAUSE &&
                event->hrEvent == MF_E_SESSION_PAUSEWHILESTOPPED) {
                bool reconciled = false;
                {
                    const std::scoped_lock lock(state_mutex_);
                    reconciled = transport_.ReconcilePauseWhileStopped();
                    if (reconciled) {
                        state_ = MediaPreviewState::Paused;
                    }
                }
                if (reconciled) {
                    platform::LogMessage(
                        L"[Preview] Reconciled failed Pause event because MFPlay was already stopped");
                    static_cast<void>(DriveTransport());
                    Notify();
                    return;
                }
            }
            MediaPreviewOperation operation;
            {
                const std::scoped_lock lock(state_mutex_);
                operation = state_ == MediaPreviewState::Opening
                    ? MediaPreviewOperation::Open : OperationForEventFailure(event->eEventType);
            }
            SetFailure(event->hrEvent, operation);
            platform::LogMessage(
                L"[Preview] MFPlay event %u failed during operation %u",
                static_cast<unsigned>(event->eEventType),
                static_cast<unsigned>(operation));
            platform::LogHresult(L"Processing an MP4 preview event",
                                 event->hrEvent);
            Notify();
            return;
        }

        // FrameStep owns opening until its completion. An EOS notification can
        // precede that completion for a one-frame source; it is not a user
        // transport completion and must not expose Ended/Ready prematurely.
        if (first_frame_pending_ && event->eEventType != MFP_EVENT_TYPE_FRAME_STEP &&
            event->eEventType != MFP_EVENT_TYPE_ERROR) {
            Notify();
            return;
        }
        bool hold_natural_end = false;
        switch (event->eEventType) {
            case MFP_EVENT_TYPE_MEDIAITEM_SET: {
                const auto* media_event = MFP_GET_MEDIAITEM_SET_EVENT(event);
                IMFPMediaItem* media_item =
                    media_event != nullptr ? media_event->pMediaItem : nullptr;
                PROPVARIANT value;
                ::PropVariantInit(&value);
                BOOL has_video = FALSE;
                BOOL video_selected = FALSE;
                HRESULT result =
                    media_item != nullptr
                        ? media_item->HasVideo(&has_video, &video_selected)
                        : E_UNEXPECTED;
                if (SUCCEEDED(result) && (!has_video || !video_selected)) {
                    result = MF_E_INVALIDMEDIATYPE;
                }
                if (SUCCEEDED(result)) {
                    result = media_item->GetDuration(MFP_POSITIONTYPE_100NS,
                                                     &value);
                }
                const auto duration = SUCCEEDED(result)
                                          ? MediaTimeFromVariant(value)
                                          : std::nullopt;
                ::PropVariantClear(&value);
                if (!duration.has_value() ||
                    *duration <= MediaTime::zero()) {
                    SetFailure(FAILED(result) ? result : MF_E_INVALIDMEDIATYPE,
                               MediaPreviewOperation::Open);
                } else {
                    if (!video_endpoint_) {
                        PreviewVideoEndpoint endpoint;
                        result = ReadPreviewVideoEndpoint(source_path_, *duration, endpoint);
                        if (FAILED(result)) {
                            SetFailure(result, MediaPreviewOperation::Open);
                            break;
                        }
                        video_endpoint_ = endpoint;
                    }
                    {
                        MediaTime decoded_position;
                        result = DecodePreviewFrame(source_path_, video_endpoint_->last_frame_position,
                                                     false, endpoint_frame_, decoded_position);
                        if (SUCCEEDED(result) && decoded_position != video_endpoint_->last_frame_position)
                            result = MF_E_INVALID_FILE_FORMAT;
                        if (SUCCEEDED(result) && !CreateStillFrameWindow()) {
                            result = HRESULT_FROM_WIN32(::GetLastError());
                            if (SUCCEEDED(result)) result = E_FAIL;
                        }
                        if (FAILED(result)) {
                            SetFailure(result, MediaPreviewOperation::Open);
                            break;
                        }
                        if (video_endpoint_->last_frame_position == MediaTime::zero()) ShowStillFrame(true);
                    }
                    {
                        const std::scoped_lock lock(state_mutex_);
                        duration_ = *duration;
                        state_ = MediaPreviewState::Opening;
                        error_ = S_OK;
                        failed_operation_ = MediaPreviewOperation::None;
                        transport_.Reset();
                        first_frame_pending_ = true;
                        ++command_sequence_;
                        command_started_ms_ = ::GetTickCount64();
                        LogTransportLocked(L"first-frame-step", event->eEventType, S_OK, event->eState);
                    }
                    // MFPlay's native frame-step command decodes one frame and
                    // settles paused; opening never starts timed playback.
                    // https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/mediafoundation/MFPlayer2
                    result = player_->FrameStep();
                    if (FAILED(result)) SetFailure(result, MediaPreviewOperation::Open);
                }
                break;
            }
            case MFP_EVENT_TYPE_FRAME_STEP: {
                if (!first_frame_pending_) break;
                MFP_MEDIAPLAYER_STATE native = MFP_MEDIAPLAYER_STATE_EMPTY;
                HRESULT result = player_->GetState(&native);
                PROPVARIANT value{};
                if (SUCCEEDED(result)) result = player_->GetPosition(MFP_POSITIONTYPE_100NS, &value);
                const auto position = SUCCEEDED(result) ? MediaTimeFromVariant(value) : std::nullopt;
                ::PropVariantClear(&value);
                if (FAILED(result) || native != MFP_MEDIAPLAYER_STATE_PAUSED ||
                    position != MediaTime::zero()) {
                    SetFailure(FAILED(result) ? result : MF_E_INVALIDREQUEST, MediaPreviewOperation::Open);
                    break;
                }
                const std::scoped_lock lock(state_mutex_);
                first_frame_pending_ = false;
                transport_.Observe(PreviewNativeState::Paused);
                state_ = MediaPreviewState::Ready;
                LogTransportLocked(L"first-frame-ready", event->eEventType, S_OK, native);
                break;
            }
            case MFP_EVENT_TYPE_PLAY:
            case MFP_EVENT_TYPE_PAUSE:
            case MFP_EVENT_TYPE_POSITION_SET:
            case MFP_EVENT_TYPE_STOP:
            case MFP_EVENT_TYPE_PLAYBACK_ENDED: {
                const std::scoped_lock lock(state_mutex_);
                if (state_ == MediaPreviewState::Failed) break;
                // Event headers are historical. Read current native state under
                // the same lock that protects intent and command ownership.
                MFP_MEDIAPLAYER_STATE native = MFP_MEDIAPLAYER_STATE_EMPTY;
                const HRESULT result = event->pMediaPlayer->GetState(&native);
                if (FAILED(result) ||
                    (native != MFP_MEDIAPLAYER_STATE_PLAYING &&
                     native != MFP_MEDIAPLAYER_STATE_PAUSED &&
                     native != MFP_MEDIAPLAYER_STATE_STOPPED)) {
                    transport_.Failed();
                    state_ = MediaPreviewState::Failed;
                    error_ = FAILED(result) ? result
                        : native == MFP_MEDIAPLAYER_STATE_SHUTDOWN
                            ? MF_E_SHUTDOWN : MF_E_INVALIDREQUEST;
                    failed_operation_ = MediaPreviewOperation::Playback;
                    break;
                }
                if (event->eEventType == MFP_EVENT_TYPE_STOP ||
                    event->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
                    transport_.Ended(NativeState(native),
                        event->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED
                            ? std::optional<MediaTime>{duration_} : std::nullopt);
                    hold_natural_end = event->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED &&
                        transport_.ended() && !transport_.command_pending() && !transport_.play_requested();
                } else {
                    const auto type = event->eEventType == MFP_EVENT_TYPE_PLAY
                        ? PreviewTransportCommandType::Play
                        : event->eEventType == MFP_EVENT_TYPE_PAUSE
                            ? PreviewTransportCommandType::Pause
                            : PreviewTransportCommandType::Seek;
                    const bool accepted = transport_.Complete(type, NativeState(native));
                    LogTransportLocked(accepted ? L"accepted" : L"unmatched", event->eEventType,
                                       event->hrEvent, event->eState);
                }
                state_ = transport_.playing() ? MediaPreviewState::Playing
                    : transport_.ended() ? MediaPreviewState::Ended
                                         : MediaPreviewState::Paused;
                break;
            }

            case MFP_EVENT_TYPE_ERROR:
                SetFailure(event->hrEvent == S_OK ? E_FAIL : event->hrEvent,
                           first_frame_pending_ ? MediaPreviewOperation::Open : MediaPreviewOperation::Playback);
                break;
            default:
                break;
        }
        if (hold_natural_end) ShowStillFrame(true);
        ScheduleEndpointPresentation();
        if (event->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET ||
            event->eEventType == MFP_EVENT_TYPE_PLAY ||
            event->eEventType == MFP_EVENT_TYPE_PAUSE ||
            event->eEventType == MFP_EVENT_TYPE_POSITION_SET ||
            event->eEventType == MFP_EVENT_TYPE_STOP ||
            event->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
            static_cast<void>(DriveTransport());
        }
        Notify();
    }

    void LogTransport(const wchar_t* kind, unsigned action, HRESULT result,
                      unsigned header_state) noexcept {
        const std::scoped_lock lock(state_mutex_);
        LogTransportLocked(kind, action, result, header_state);
    }

    void LogTransportLocked(const wchar_t* kind, unsigned action, HRESULT result,
                            unsigned header_state) noexcept {
        auto& line = trace_[trace_sequence_ % trace_.size()];
        ++trace_sequence_;
        FormatTransportLocked(line.data(), line.size(), kind, action, result,
                              header_state, trace_sequence_);
        ::OutputDebugStringW(line.data());
    }

    void FormatTransportLocked(wchar_t* buffer, size_t capacity, const wchar_t* kind,
                               unsigned action, HRESULT result, unsigned header_state,
                               uint64_t sequence) const noexcept {
        if (recovery_snapshot_) {
            const auto& captured = *recovery_snapshot_;
            static_cast<void>(::swprintf_s(buffer, capacity,
                L"[Preview] seq=%llu ms=%llu tid=%lu gen=%llu intent=%llu %s=%u hr=%08X recovery-requested=1 operation=%u cmd=%llu age=%llu pending=%u position-pending=%u logical=%lld desired=%u native-playing=%u native-position=%lld native-valid=%u\n",
                sequence, ::GetTickCount64(), ::GetCurrentThreadId(),
                captured.player_generation, captured.intent_sequence, kind,
                action, static_cast<unsigned>(captured.error),
                static_cast<unsigned>(captured.failed_operation),
                captured.command_sequence, captured.command_age_ms,
                captured.command_pending ? 1u : 0u,
                captured.position_pending ? 1u : 0u, captured.position.count(),
                captured.play_requested ? 1u : 0u,
                captured.native_playing ? 1u : 0u,
                captured.native_position.count(), captured.native_position_valid ? 1u : 0u));
            return;
        }
        MFP_MEDIAPLAYER_STATE native = MFP_MEDIAPLAYER_STATE_EMPTY;
        const HRESULT state_result =
            player_ != nullptr ? player_->GetState(&native) : E_UNEXPECTED;
        PROPVARIANT position{};
        const HRESULT position_result = player_ != nullptr
            ? player_->GetPosition(MFP_POSITIONTYPE_100NS, &position) : E_UNEXPECTED;
        const auto native_position = SUCCEEDED(position_result)
            ? MediaTimeFromVariant(position) : std::nullopt;
        ::PropVariantClear(&position);
        const auto logical = transport_.ResolvePosition(native_position.value_or(MediaTime{-1}));
        static_cast<void>(::swprintf_s(buffer, capacity,
            L"[Preview] seq=%llu ms=%llu tid=%lu gen=%llu intent=%llu %s=%u hr=%08X cmd=%llu/%s target=%lld desired=%u actual=%u flight=%s age=%llu dispatch=%u pending=%lld logical=%lld position-pending=%u preview=%u header=%u native=%u/%08X pos=%lld/%08X override=%lld native-target=%lld\n",
            sequence, ::GetTickCount64(), ::GetCurrentThreadId(), player_generation_,
            intent_sequence_, kind, action, static_cast<unsigned>(result),
            command_sequence_, TransportCommandName(last_command_.type),
            last_command_.position.count(), transport_.play_requested() ? 1u : 0u,
            transport_.playing() ? 1u : 0u, TransportCommandName(transport_.in_flight()),
            transport_.in_flight() == PreviewTransportCommandType::None ? 0
                : ::GetTickCount64() - command_started_ms_, dispatch_call_active_ ? 1u : 0u,
            transport_.desired_position().value_or(MediaTime{-1}).count(),
            logical.count(), transport_.position_pending() ? 1u : 0u,
            static_cast<unsigned>(state_), header_state,
            static_cast<unsigned>(native), static_cast<unsigned>(state_result),
            native_position.value_or(MediaTime{-1}).count(),
            static_cast<unsigned>(position_result),
            transport_.position_override().value_or(MediaTime{-1}).count(),
            last_native_target_.count()));
    }

    void LogTransportCommand(PreviewTransportCommandType command,
                             HRESULT result) noexcept {
        LogTransport(L"command", static_cast<unsigned>(command), result, 0);
    }

    void LogTransportEvent(MFP_EVENT_TYPE event_type, HRESULT result,
                           MFP_MEDIAPLAYER_STATE header_state) noexcept {
        if (event_type == MFP_EVENT_TYPE_MF && SUCCEEDED(result)) return;
        LogTransport(L"event", static_cast<unsigned>(event_type), result,
                      static_cast<unsigned>(header_state));
    }

    void SetFailure(HRESULT error, MediaPreviewOperation operation) noexcept {
        DestroyStillFrameWindow();
        const std::scoped_lock lock(state_mutex_);
        transport_.Failed();
        first_frame_pending_ = false;
        state_ = MediaPreviewState::Failed;
        error_ = error;
        failed_operation_ = operation;
    }

    void Notify() const noexcept {
        if (notify_window_ != nullptr && notify_message_ >= WM_APP) {
            static_cast<void>(
                ::PostMessageW(notify_window_, notify_message_, 0, 0));
        }
    }

    void ClosePlayerResources() noexcept {
        if (presentation_wait_) {
            ::SetThreadpoolWait(presentation_wait_, nullptr, nullptr);
            ::WaitForThreadpoolWaitCallbacks(presentation_wait_, TRUE);
            ::CloseThreadpoolWait(presentation_wait_);
            presentation_wait_ = nullptr;
        }
        if (presentation_timer_) {
            ::CancelWaitableTimer(presentation_timer_);
            ::CloseHandle(presentation_timer_);
            presentation_timer_ = nullptr;
        }
        DestroyStillFrameWindow();
        if (owner_window_ != nullptr) {
            ::KillTimer(owner_window_, 1);
            ::DestroyWindow(owner_window_);
            owner_window_ = nullptr;
        }
        RetirePlayer();
        if (native_video_window_) {
            ::DestroyWindow(native_video_window_);
            native_video_window_ = nullptr;
        }
        if (mf_started_) {
            static_cast<void>(::MFShutdown());
            mf_started_ = false;
        }
        notify_window_ = nullptr;
        notify_message_ = 0;
    }

    void RetirePlayer() noexcept {
        if (callback_ != nullptr) callback_->Detach();
        if (player_ != nullptr) {
            static_cast<void>(player_->Shutdown());
            player_.Reset();
        }
        if (callback_ != nullptr) {
            callback_->Release();
            callback_ = nullptr;
        }
    }

    static LRESULT CALLBACK OwnerWindowProc(HWND window, UINT message,
                                            WPARAM wparam, LPARAM lparam) noexcept {
        auto* owner = reinterpret_cast<MfPlayPreview*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            owner = static_cast<MfPlayPreview*>(
                reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
        }
        if (owner && message == kPresentationMessage) {
            owner->ScheduleEndpointPresentation();
            return 0;
        }
        if (message == WM_TIMER && owner != nullptr) {
            owner->CheckLiveness();
            return 0;
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    bool CreateOwnerWindow() noexcept {
        static const bool registered = [] {
            WNDCLASSW descriptor{};
            descriptor.lpfnWndProc = OwnerWindowProc;
            descriptor.hInstance = ::GetModuleHandleW(nullptr);
            descriptor.lpszClassName = L"Easnap.PreviewTransportOwner";
            return ::RegisterClassW(&descriptor) != 0 ||
                   ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }();
        if (!registered) return false;
        owner_window_ = ::CreateWindowExW(0, L"Easnap.PreviewTransportOwner", L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), this);
        if (!owner_window_) return false;
        presentation_timer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        // Preserve the existing Windows 10 baseline when the newer timer flag
        // is unavailable. Both paths deliver an owner message, not WM_TIMER.
        if (!presentation_timer_ && ::GetLastError() == ERROR_INVALID_PARAMETER)
            presentation_timer_ = ::CreateWaitableTimerExW(nullptr, nullptr, 0,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!presentation_timer_) return false;
        presentation_wait_ = ::CreateThreadpoolWait(
            [](PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) noexcept {
                ::PostMessageW(static_cast<HWND>(context), kPresentationMessage, 0, 0);
            }, owner_window_, nullptr);
        if (!presentation_wait_) return false;
        return ::SetTimer(owner_window_, 1, 100, nullptr) != 0;
    }

    void CheckLiveness() noexcept {
        // Native callbacks and this timer share the creating thread. The parent
        // process alone owns replacement and its retry budget; this helper only
        // detects loss of progress and publishes the evidence without retiring
        // native resources on a path that could itself block in Shutdown.
        if (dispatch_call_active_ || callback_depth_ != 0 ||
            state_ == MediaPreviewState::Failed || state_ == MediaPreviewState::Closed) return;
        const uint64_t now = ::GetTickCount64();
        if (state_ == MediaPreviewState::Opening) {
            if (now - opening_started_ms_ > 5'000) {
                MediaPreviewSnapshot current{};
                {
                    const std::scoped_lock lock(state_mutex_);
                    current.state = MediaPreviewState::Opening;
                    current.duration = duration_;
                    current.play_requested = transport_.play_requested();
                    current.command_pending = transport_.command_pending();
                    current.position = transport_.ResolvePosition(MediaTime::zero());
                    current.position_pending = transport_.position_pending();
                    current.player_generation = player_generation_;
                    current.command_sequence = command_sequence_;
                    current.command_age_ms =
                        transport_.in_flight() == PreviewTransportCommandType::None
                            ? 0 : now - command_started_ms_;
                    current.intent_sequence = intent_sequence_;
                }
                RequestRecovery(current, MediaPreviewOperation::Open,
                                PreviewLivenessFailure::CommandTimeout);
            }
            return;
        }
        const auto current = snapshot();
        const auto failure = liveness_.Observe(now, player_generation_,
            command_sequence_, transport_.in_flight(), current.play_requested,
            current.native_playing, current.native_position_valid
                ? std::optional<MediaTime>{current.native_position} : std::nullopt,
            current.position_pending);
        if (failure != PreviewLivenessFailure::None) {
            RequestRecovery(current, MediaPreviewOperation::Playback, failure);
        }
    }

    void RequestRecovery(MediaPreviewSnapshot current, MediaPreviewOperation operation,
                         PreviewLivenessFailure failure) noexcept {
        {
            const std::scoped_lock lock(state_mutex_);
            if (recovery_snapshot_) return;
            // Keep pending ownership, latest accepted intent and native evidence
            // intact. Failed prevents further commands; the generation change
            // rejects every old callback before it can change that evidence.
            current.state = MediaPreviewState::Failed;
            current.error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            current.failed_operation = operation;
            current.recovery_requested = true;
            recovery_snapshot_ = current;
            ++player_generation_;
            state_ = current.state;
            error_ = current.error;
            failed_operation_ = current.failed_operation;
        }
        if (callback_ != nullptr) callback_->Detach();
        // Once captured, both diagnostics and snapshot avoid all native calls.
        // Close owns eventual release inside the supervised helper process.
        LogTransport(L"liveness-timeout", static_cast<unsigned>(failure),
                     current.error, 0);
        const uint64_t first = trace_sequence_ > trace_.size()
            ? trace_sequence_ - trace_.size() : 0;
        for (uint64_t index = first; index < trace_sequence_; ++index) {
            ::OutputDebugStringW(trace_[index % trace_.size()].data());
        }
        Notify();
    }

    

    mutable std::mutex state_mutex_;
    ComPtr<IMFPMediaPlayer> player_;
    Callback* callback_{nullptr};
    HWND notify_window_{nullptr};
    HWND video_window_{nullptr};
    HWND owner_window_{nullptr};
    DWORD owner_thread_{0};
    std::wstring source_path_;
    UINT notify_message_{0};
    MediaPreviewState state_{MediaPreviewState::Closed};
    MediaTime duration_{0};
    HRESULT error_{S_OK};
    MediaPreviewOperation failed_operation_{MediaPreviewOperation::None};
    PreviewTransport transport_;
    bool first_frame_pending_{false};
    static constexpr UINT kPresentationMessage = WM_APP + 17;
    HANDLE presentation_timer_{nullptr};
    PTP_WAIT presentation_wait_{nullptr};
    HWND native_video_window_{nullptr};
    HWND still_frame_window_{nullptr};
    platform::BgraImage still_frame_;
    platform::BgraImage endpoint_frame_;
    std::optional<MediaTime> cached_still_position_;
    MediaTime cached_still_pts_{0};
    MediaTime cached_still_end_{0};
    bool cached_still_exclusive_{false};
    bool pending_exclusive_end_{false};
    bool show_still_{false};
    bool showing_endpoint_{false};
    PreviewLiveness liveness_;
    std::optional<PreviewVideoEndpoint> video_endpoint_;
    MediaTime last_native_target_{0};
    PreviewTransportCommand last_command_;
    uint64_t command_sequence_{0};
    uint64_t trace_sequence_{0};
    uint64_t player_generation_{0};
    uint64_t intent_sequence_{0};
    uint64_t command_started_ms_{0};
    std::array<std::array<wchar_t, 1024>, 128> trace_{};
    bool dispatch_call_active_{false};
    bool mf_started_{false};
    std::optional<MediaPreviewSnapshot> recovery_snapshot_;
    uint64_t opening_started_ms_{0};
    unsigned callback_depth_{0};
};

}  // namespace

std::unique_ptr<IMediaPreview> CreateInProcessMfPlayPreview() {
    try {
        return std::make_unique<MfPlayPreview>();
    } catch (...) {
        return nullptr;
    }
}

}  // namespace easnap::media
