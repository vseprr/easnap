// Easnap - Desktop Duplication based region capture with a GDI fallback.
#include "capture/capture_provider.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <cstring>

namespace easnap::capture {
namespace {

using platform::ComPtr;
using platform::LogHresult;
using platform::LogMessage;
using platform::MonitorInfo;

// A completely static desktop never presents a new frame, so the retry budget is
// deliberately short: falling back to the GDI blit is far better than making the
// user wait. 8 x 30 ms is enough to catch the "first frame is still empty"
// window that Desktop Duplication opens right after DuplicateOutput().
constexpr UINT kAcquireTimeoutMs = 30;
constexpr int kAcquireAttempts = 8;

enum class FrameAcquireStatus { Acquired, NoNewFrame, Failed };

RegionCaptureFailureDiagnostic HresultFailure(
    RegionCaptureFailureStage stage, HRESULT result) noexcept {
    return {stage, RegionCaptureFailureCodeKind::HResult,
            static_cast<uint32_t>(result)};
}

RegionCaptureFailureDiagnostic Win32Failure(
    RegionCaptureFailureStage stage, DWORD error) noexcept {
    return {stage, RegionCaptureFailureCodeKind::Win32,
            static_cast<uint32_t>(error)};
}

uint64_t ElapsedMicroseconds(std::chrono::steady_clock::time_point start) noexcept {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto value = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    return value > 0 ? static_cast<uint64_t>(value) : 0;
}

/// Cheap sanity check used to detect the "duplication handed us an empty
/// surface" case that some drivers hit on the primary output. Sampling a grid
/// keeps the cost independent of the monitor resolution.
bool ImageLooksBlank(const BgraImage& image) {
    if (image.empty()) {
        return true;
    }
    constexpr int kSamplesPerAxis = 24;
    const int step_x = std::max(1, image.width / kSamplesPerAxis);
    const int step_y = std::max(1, image.height / kSamplesPerAxis);
    for (int y = 0; y < image.height; y += step_y) {
        const uint32_t* row = image.row(y);
        for (int x = 0; x < image.width; x += step_x) {
            if ((row[x] & 0x00FFFFFFu) != 0u) {
                return false;
            }
        }
    }
    return true;
}

/// Copies a GPU texture into a CPU image through a staging resource.
bool ReadTextureToImage(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                        BgraImage& out, RegionCaptureFailureDiagnostic* failure) {
    if (device == nullptr || context == nullptr || texture == nullptr) {
        if (failure != nullptr) {
            *failure = HresultFailure(RegionCaptureFailureStage::DuplicationInvalidTexture,
                                     E_INVALIDARG);
        }
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        if (failure != nullptr) {
            *failure = HresultFailure(RegionCaptureFailureStage::DuplicationInvalidTexture,
                                     E_INVALIDARG);
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr)) {
        LogHresult(L"CreateTexture2D(staging)", hr);
        if (failure != nullptr) {
            *failure = HresultFailure(
                RegionCaptureFailureStage::DuplicationStagingTexture, hr);
        }
        return false;
    }
    context->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LogHresult(L"ID3D11DeviceContext::Map", hr);
        if (failure != nullptr) {
            *failure = HresultFailure(RegionCaptureFailureStage::DuplicationMapTexture,
                                     hr);
        }
        return false;
    }

    out.Reset(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.row(y), source + static_cast<size_t>(y) * mapped.RowPitch, out.stride_bytes());
    }
    context->Unmap(staging.Get(), 0);
    out.MakeOpaque();  // The desktop image is always fully opaque.
    return true;
}

bool ReadTextureRegionToImage(ID3D11Device* device, ID3D11DeviceContext* context,
                              ID3D11Texture2D* texture, const RECT& source_region,
                              BgraImage& out,
                              RegionCaptureFailureDiagnostic* failure) {
    if (device == nullptr || context == nullptr || texture == nullptr ||
        platform::IsEmptyRect(source_region) || source_region.left < 0 || source_region.top < 0) {
        if (failure != nullptr) {
            *failure = HresultFailure(RegionCaptureFailureStage::DuplicationInvalidTexture,
                                     E_INVALIDARG);
        }
        return false;
    }
    D3D11_TEXTURE2D_DESC source_desc{};
    texture->GetDesc(&source_desc);
    if (source_region.right > static_cast<LONG>(source_desc.Width) ||
        source_region.bottom > static_cast<LONG>(source_desc.Height)) {
        if (failure != nullptr) {
            *failure = HresultFailure(RegionCaptureFailureStage::DuplicationInvalidTexture,
                                     E_INVALIDARG);
        }
        return false;
    }

    const UINT width = static_cast<UINT>(platform::RectWidth(source_region));
    const UINT height = static_cast<UINT>(platform::RectHeight(source_region));
    D3D11_TEXTURE2D_DESC staging_desc = source_desc;
    staging_desc.Width = width;
    staging_desc.Height = height;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr)) {
        LogHresult(L"CreateTexture2D(region staging)", hr);
        if (failure != nullptr) {
            *failure = HresultFailure(
                RegionCaptureFailureStage::DuplicationStagingTexture, hr);
        }
        return false;
    }
    const D3D11_BOX source_box{
        static_cast<UINT>(source_region.left), static_cast<UINT>(source_region.top), 0,
        static_cast<UINT>(source_region.right), static_cast<UINT>(source_region.bottom), 1};
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, &source_box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LogHresult(L"ID3D11DeviceContext::Map(region)", hr);
        if (failure != nullptr) {
            *failure = HresultFailure(RegionCaptureFailureStage::DuplicationMapTexture,
                                     hr);
        }
        return false;
    }

    out.Reset(static_cast<int>(width), static_cast<int>(height));
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.row(y), source + static_cast<size_t>(y) * mapped.RowPitch,
                    out.stride_bytes());
    }
    context->Unmap(staging.Get(), 0);
    out.MakeOpaque();
    return true;
}

/// Screen-DC blit used when Desktop Duplication is unavailable (RDP sessions,
/// some virtual machines, secure-desktop transitions).
bool CaptureMonitorWithGdi(const RECT& bounds, BgraImage& out,
                           RegionCaptureFailureDiagnostic* failure = nullptr) {
    const int width = static_cast<int>(platform::RectWidth(bounds));
    const int height = static_cast<int>(platform::RectHeight(bounds));
    if (width <= 0 || height <= 0) {
        if (failure != nullptr) {
            *failure = Win32Failure(RegionCaptureFailureStage::GdiInvalidRegion,
                                    ERROR_INVALID_PARAMETER);
        }
        return false;
    }

    ::SetLastError(ERROR_SUCCESS);
    HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) {
        if (failure != nullptr) {
            *failure = Win32Failure(RegionCaptureFailureStage::GdiGetScreenDc,
                                    ::GetLastError());
        }
        return false;
    }
    ::SetLastError(ERROR_SUCCESS);
    HDC memory_dc = ::CreateCompatibleDC(screen);
    if (memory_dc == nullptr) {
        const DWORD error = ::GetLastError();
        ::ReleaseDC(nullptr, screen);
        if (failure != nullptr) {
            *failure = Win32Failure(RegionCaptureFailureStage::GdiCreateMemoryDc,
                                    error);
        }
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    ::SetLastError(ERROR_SUCCESS);
    HBITMAP bitmap = ::CreateDIBSection(memory_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap == nullptr || bits == nullptr) {
        if (failure != nullptr) {
            *failure = Win32Failure(RegionCaptureFailureStage::GdiCreateDibSection,
                                    ::GetLastError());
        }
    } else {
        ::SetLastError(ERROR_SUCCESS);
        HBITMAP previous = static_cast<HBITMAP>(::SelectObject(memory_dc, bitmap));
        if (previous == nullptr || previous == HGDI_ERROR) {
            if (failure != nullptr) {
                *failure = Win32Failure(RegionCaptureFailureStage::GdiSelectBitmap,
                                        ::GetLastError());
            }
        } else {
            ::SetLastError(ERROR_SUCCESS);
            ok = ::BitBlt(memory_dc, 0, 0, width, height, screen, bounds.left, bounds.top,
                          SRCCOPY) != FALSE;
            const DWORD blit_error = ok ? ERROR_SUCCESS : ::GetLastError();
            ::GdiFlush();
            if (ok) {
                out.Reset(width, height);
                std::memcpy(out.pixels.data(), bits, out.pixels.size() * sizeof(uint32_t));
                out.MakeOpaque();
            } else if (failure != nullptr) {
                *failure = Win32Failure(RegionCaptureFailureStage::GdiBitBlt,
                                        blit_error);
            }
            ::SelectObject(memory_dc, previous);
        }
    }
    if (bitmap != nullptr) {
        ::DeleteObject(bitmap);
    }
    ::DeleteDC(memory_dc);
    ::ReleaseDC(nullptr, screen);
    return ok;
}

/// Per-output duplication state. Recreated whenever the desktop changes mode,
/// the session is locked, or a fullscreen application takes ownership.
struct OutputDuplication {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    HMONITOR monitor{nullptr};  ///< Authoritative identity: rectangles can collide.
    RECT desktop_rect{};
    DXGI_MODE_ROTATION rotation{DXGI_MODE_ROTATION_IDENTITY};
    bool frame_acquired{false};
    bool unusable{false};  ///< Set once this output proved it only returns black.
    RegionCaptureFailureDiagnostic unusable_failure;

    void ReleaseFrame() {
        if (frame_acquired && duplication != nullptr) {
            duplication->ReleaseFrame();
            frame_acquired = false;
        }
    }
};

class DesktopDuplicationProvider final : public IRegionCaptureProvider {
public:
    ~DesktopDuplicationProvider() override { Release(); }

    bool CaptureDesktop(bool include_cursor, DesktopSnapshot& out) override {
        out.Clear();
        out.virtual_bounds = platform::VirtualScreenBounds();
        out.timestamp = std::chrono::steady_clock::now();

        if (needs_rebuild_) {
            // A previous capture lost duplication access (mode change, session
            // lock, fullscreen hand-off). Rebuilding here is safe: no frame is
            // in flight and nothing holds a reference into `outputs_`.
            needs_rebuild_ = false;
            Release();
        }

        const std::vector<MonitorInfo> monitors = platform::EnumerateMonitors();
        for (const MonitorInfo& monitor : monitors) {
            MonitorFrame frame;
            frame.desktop_rect = monitor.bounds;
            if (!CaptureMonitor(monitor, frame.image)) {
                LogMessage(L"Monitor capture failed; the region will be filled with black");
                frame.image.Reset(static_cast<int>(platform::RectWidth(monitor.bounds)),
                                  static_cast<int>(platform::RectHeight(monitor.bounds)));
                frame.image.MakeOpaque();
            }
            out.monitors.push_back(std::move(frame));
        }

        if (include_cursor) {
            for (MonitorFrame& frame : out.monitors) {
                CompositeCursor(frame.image, frame.desktop_rect);
            }
        }
        return !out.monitors.empty();
    }

    bool CaptureRegion(const RECT& region, bool include_cursor, BgraImage& out) override {
        DesktopSnapshot snapshot;
        if (!CaptureDesktop(include_cursor, snapshot)) {
            return false;
        }
        return snapshot.CropRegion(region, out);
    }

    RegionCaptureResult CaptureRegionFrame(const RECT& region, bool include_cursor,
                                            media::MediaTime timestamp,
                                            media::MediaTime duration,
                                            media::VideoFrame& out) override {
        BgraImage image;
        RegionCaptureResult result = CaptureRecordingRegion(region, include_cursor, image);
        if (result.status != RegionCaptureStatus::FrameAcquired) {
            return result;
        }
        out = media::VideoFrame{
            std::make_shared<const BgraImage>(std::move(image)), timestamp, duration};
        return result;
    }

    void Release() noexcept override {
        for (OutputDuplication& output : outputs_) {
            output.ReleaseFrame();
        }
        outputs_.clear();
        initialized_ = false;
        // Capability probing runs again on the next capture: an output that was
        // unavailable (locked session, protected playback) may work next time.
        using_fallback_ = false;
        needs_rebuild_ = false;
        recording_frame_available_ = false;
        duplication_initialization_failure_ = {};
    }

    [[nodiscard]] const wchar_t* name() const override {
        return using_fallback_ ? L"GDI screen blit (fallback)" : L"DXGI Desktop Duplication";
    }

private:
    /// Duplication first, GDI second - decided per monitor, never globally.
    ///
    /// Real hardware regularly serves one output correctly while another one
    /// hands back an empty surface (mixed refresh rates, DisplayPort links,
    /// hybrid GPUs). Falling back for that single output keeps the other
    /// monitors on the fast path instead of degrading the whole capture.
    RegionCaptureResult CaptureRecordingRegion(const RECT& region, bool include_cursor,
                                                BgraImage& out) {
        RegionCaptureResult result;
        const RECT normalized = platform::IntersectRects(region, platform::VirtualScreenBounds());
        if (platform::IsEmptyRect(normalized) || !::EqualRect(&normalized, &region)) {
            result.primary_failure = Win32Failure(
                RegionCaptureFailureStage::InvalidRegion, ERROR_INVALID_PARAMETER);
            return result;
        }
        if (needs_rebuild_) {
            needs_rebuild_ = false;
            Release();
        }

        const std::vector<MonitorInfo> monitors = platform::EnumerateMonitors();
        for (const MonitorInfo& monitor : monitors) {
            const RECT overlap = platform::IntersectRects(region, monitor.bounds);
            if (!platform::IsEmptyRect(overlap) && ::EqualRect(&overlap, &region)) {
                result = CaptureMonitorRegion(monitor, region, out);
                if (result.status != RegionCaptureStatus::FrameAcquired) {
                    return result;
                }
                if (include_cursor) {
                    CompositeCursor(out, region);
                }
                return result;
            }
        }

        // A cross-output recording must sample every output consistently. GDI
        // avoids waiting for unrelated Desktop Duplication present streams.
        out.Reset(static_cast<int>(platform::RectWidth(region)),
                  static_cast<int>(platform::RectHeight(region)));
        out.MakeOpaque();
        bool overlaps_monitor = false;
        for (const MonitorInfo& monitor : monitors) {
            const RECT overlap = platform::IntersectRects(region, monitor.bounds);
            if (platform::IsEmptyRect(overlap)) {
                continue;
            }
            overlaps_monitor = true;
            BgraImage portion;
            const auto copy_start = std::chrono::steady_clock::now();
            if (!CaptureMonitorWithGdi(overlap, portion)) {
                LogMessage(L"Monitor region capture failed; that portion remains black");
                continue;
            }
            result.metrics.region_copy_microseconds += ElapsedMicroseconds(copy_start);
            const int target_x = static_cast<int>(overlap.left - region.left);
            const int target_y = static_cast<int>(overlap.top - region.top);
            for (int y = 0; y < portion.height; ++y) {
                std::memcpy(out.row(target_y + y) + target_x, portion.row(y),
                            portion.stride_bytes());
            }
        }
        if (include_cursor) {
            CompositeCursor(out, region);
        }
        result.status = overlaps_monitor ? RegionCaptureStatus::FrameAcquired
                                         : RegionCaptureStatus::Failed;
        if (overlaps_monitor) {
            recording_frame_available_ = true;
        } else {
            result.primary_failure = Win32Failure(
                RegionCaptureFailureStage::NoOverlappingMonitor,
                ERROR_NOT_FOUND);
        }
        return result;
    }

    RegionCaptureResult CaptureMonitorRegion(const MonitorInfo& monitor, const RECT& region,
                                              BgraImage& out) {
        RegionCaptureResult result;
        if (!using_fallback_ && EnsureDuplication()) {
            const int index = FindOutputIndex(monitor);
            if (index >= 0) {
                OutputDuplication& output = outputs_[static_cast<size_t>(index)];
                const bool rotated = output.rotation != DXGI_MODE_ROTATION_IDENTITY &&
                                     output.rotation != DXGI_MODE_ROTATION_UNSPECIFIED;
                const RECT source_region{
                    region.left - monitor.bounds.left, region.top - monitor.bounds.top,
                    region.right - monitor.bounds.left, region.bottom - monitor.bounds.top};
                if (!rotated && !output.unusable) {
                    const FrameAcquireStatus acquired = AcquireFrame(
                        output, out, &source_region, 0, 1, &result.metrics,
                        &result.primary_failure);
                    if (acquired == FrameAcquireStatus::Acquired && !ImageLooksBlank(out)) {
                        recording_frame_available_ = true;
                        result.status = RegionCaptureStatus::FrameAcquired;
                        return result;
                    }
                    if (acquired == FrameAcquireStatus::NoNewFrame &&
                        recording_frame_available_) {
                        result.status = RegionCaptureStatus::NoNewFrame;
                        return result;
                    }
                    if (acquired == FrameAcquireStatus::Acquired) {
                        LogMessage(L"Duplication returned a blank recording region; using the GDI blit");
                        output.unusable = true;
                        output.unusable_failure = {
                            RegionCaptureFailureStage::DuplicationBlankFrame,
                            RegionCaptureFailureCodeKind::None, 0};
                        result.primary_failure = output.unusable_failure;
                    }
                } else if (rotated) {
                    result.primary_failure = {
                        RegionCaptureFailureStage::DuplicationRotationUnsupported,
                        RegionCaptureFailureCodeKind::None, 0};
                } else {
                    result.primary_failure = output.unusable_failure;
                }
            } else {
                result.primary_failure = {
                    RegionCaptureFailureStage::DuplicationOutputNotFound,
                    RegionCaptureFailureCodeKind::None, 0};
            }
        } else {
            result.primary_failure = duplication_initialization_failure_;
        }
        const auto copy_start = std::chrono::steady_clock::now();
        if (CaptureMonitorWithGdi(region, out, &result.fallback_failure)) {
            result.metrics.region_copy_microseconds += ElapsedMicroseconds(copy_start);
            recording_frame_available_ = true;
            result.status = RegionCaptureStatus::FrameAcquired;
            return result;
        }
        LogMessage(L"GDI blit failed for one recording region");
        result.status = RegionCaptureStatus::Failed;
        return result;
    }

    bool CaptureMonitor(const MonitorInfo& monitor, BgraImage& out) {
        if (!using_fallback_ && EnsureDuplication()) {
            const int index = FindOutputIndex(monitor);
            if (index >= 0) {
                OutputDuplication& output = outputs_[static_cast<size_t>(index)];
                const bool rotated = output.rotation != DXGI_MODE_ROTATION_IDENTITY &&
                                     output.rotation != DXGI_MODE_ROTATION_UNSPECIFIED;
                // Rotated outputs need a transform pass; the GDI path already
                // returns upright pixels, so use it until Phase 2 adds one.
                if (!rotated && !output.unusable &&
                    AcquireFrame(output, out, nullptr, kAcquireTimeoutMs, kAcquireAttempts,
                                 nullptr, nullptr) == FrameAcquireStatus::Acquired) {
                    if (!ImageLooksBlank(out)) {
                        return true;
                    }
                    // The output is attached and duplication reported success,
                    // yet every sampled pixel is black: trust GDI from now on.
                    LogMessage(L"Duplication returned a blank surface for one output; using the GDI blit");
                    output.unusable = true;
                    output.unusable_failure = {
                        RegionCaptureFailureStage::DuplicationBlankFrame,
                        RegionCaptureFailureCodeKind::None, 0};
                }
            }
        }
        if (CaptureMonitorWithGdi(monitor.bounds, out)) {
            return true;
        }
        LogMessage(L"GDI blit failed for one monitor");
        return false;
    }

    /// Matches an output to a monitor by HMONITOR first; duplicated desktop
    /// rectangles (cloned displays) would make a rectangle comparison ambiguous.
    [[nodiscard]] int FindOutputIndex(const MonitorInfo& monitor) const {
        for (size_t i = 0; i < outputs_.size(); ++i) {
            if (monitor.handle != nullptr && outputs_[i].monitor == monitor.handle) {
                return static_cast<int>(i);
            }
        }
        for (size_t i = 0; i < outputs_.size(); ++i) {
            if (::EqualRect(&outputs_[i].desktop_rect, &monitor.bounds)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool EnsureDuplication() {
        if (initialized_) {
            return !outputs_.empty();
        }
        initialized_ = true;
        duplication_initialization_failure_ = {};

        platform::GraphicsEnvironment& graphics = platform::GraphicsEnvironment::Get();
        if (!graphics.EnsureInitialized()) {
            using_fallback_ = true;
            duplication_initialization_failure_ = {
                RegionCaptureFailureStage::GraphicsInitialization,
                RegionCaptureFailureCodeKind::None, 0};
            return false;
        }

        ComPtr<IDXGIFactory2> factory = graphics.dxgi_factory();
        for (UINT adapter_index = 0;; ++adapter_index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            if (!CreateDeviceForAdapter(adapter.Get(), device, context,
                                        &duplication_initialization_failure_)) {
                continue;
            }

            for (UINT output_index = 0;; ++output_index) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                DXGI_OUTPUT_DESC output_desc{};
                if (FAILED(output->GetDesc(&output_desc)) || output_desc.AttachedToDesktop == FALSE) {
                    continue;
                }
                ComPtr<IDXGIOutput1> output1;
                if (FAILED(output.As(&output1))) {
                    continue;
                }
                OutputDuplication entry;
                entry.device = device;
                entry.context = context;
                entry.monitor = output_desc.Monitor;
                entry.desktop_rect = output_desc.DesktopCoordinates;
                entry.rotation = output_desc.Rotation;
                const HRESULT hr = output1->DuplicateOutput(device.Get(), &entry.duplication);
                if (FAILED(hr)) {
                    // E_ACCESSDENIED happens on the secure desktop and while a
                    // protected fullscreen app is running: fall back silently.
                    LogHresult(L"IDXGIOutput1::DuplicateOutput", hr);
                    duplication_initialization_failure_ = HresultFailure(
                        RegionCaptureFailureStage::DuplicateOutput, hr);
                    continue;
                }
                outputs_.push_back(std::move(entry));
            }
        }

        if (outputs_.empty()) {
            using_fallback_ = true;
            if (duplication_initialization_failure_.stage ==
                RegionCaptureFailureStage::None) {
                duplication_initialization_failure_ = {
                    RegionCaptureFailureStage::DuplicationOutputNotFound,
                    RegionCaptureFailureCodeKind::None, 0};
            }
            LogMessage(L"Desktop Duplication unavailable; using the GDI fallback path");
            return false;
        }
        return true;
    }

    static bool CreateDeviceForAdapter(IDXGIAdapter1* adapter, ComPtr<ID3D11Device>& device,
                                       ComPtr<ID3D11DeviceContext>& context,
                                       RegionCaptureFailureDiagnostic* failure) {
        static constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL obtained{};
        const HRESULT hr =
            ::D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION, &device, &obtained, &context);
        if (FAILED(hr)) {
            LogHresult(L"D3D11CreateDevice(adapter)", hr);
            if (failure != nullptr) {
                *failure = HresultFailure(
                    RegionCaptureFailureStage::D3dDeviceCreation, hr);
            }
            return false;
        }
        return true;
    }

    /// Pulls one usable desktop image out of `output`.
    ///
    /// Desktop Duplication may hand back a frame that carries no desktop update
    /// at all (`LastPresentTime == 0`), which is exactly the case that produced
    /// an all-black primary monitor: the surface exists but was never filled.
    /// Such frames are kept only as a last resort while the loop keeps asking
    /// for a real presentation.
    FrameAcquireStatus AcquireFrame(OutputDuplication& output, BgraImage& out,
                                    const RECT* source_region, UINT timeout_ms,
                                    int attempts, RegionCaptureMetrics* metrics,
                                    RegionCaptureFailureDiagnostic* failure) {
        output.ReleaseFrame();

        BgraImage provisional;
        bool have_provisional = false;

        for (int attempt = 0; attempt < attempts; ++attempt) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            const auto acquisition_start = std::chrono::steady_clock::now();
            const HRESULT hr = output.duplication->AcquireNextFrame(timeout_ms, &info, &resource);
            if (metrics != nullptr) {
                metrics->acquisition_microseconds += ElapsedMicroseconds(acquisition_start);
            }
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                if (failure != nullptr) {
                    *failure = HresultFailure(
                        RegionCaptureFailureStage::DuplicationAcquire, hr);
                }
                continue;
            }
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
                LogMessage(L"Duplication access lost; the next capture rebuilds the providers");
                output.ReleaseFrame();
                needs_rebuild_ = true;
                if (failure != nullptr) {
                    *failure = HresultFailure(
                        RegionCaptureFailureStage::DuplicationAcquire, hr);
                }
                return FrameAcquireStatus::Failed;
            }
            if (FAILED(hr)) {
                LogHresult(L"AcquireNextFrame", hr);
                output.ReleaseFrame();
                if (failure != nullptr) {
                    *failure = HresultFailure(
                        RegionCaptureFailureStage::DuplicationAcquire, hr);
                }
                return FrameAcquireStatus::Failed;
            }

            output.frame_acquired = true;
            ComPtr<ID3D11Texture2D> texture;
            BgraImage candidate;
            const auto copy_start = std::chrono::steady_clock::now();
            const HRESULT texture_result = resource.As(&texture);
            if (FAILED(texture_result) && failure != nullptr) {
                *failure = HresultFailure(
                    RegionCaptureFailureStage::DuplicationFrameTexture,
                    texture_result);
            }
            const bool read = SUCCEEDED(texture_result) &&
                (source_region == nullptr
                     ? ReadTextureToImage(output.device.Get(), output.context.Get(),
                                          texture.Get(), candidate, failure)
                     : ReadTextureRegionToImage(output.device.Get(), output.context.Get(),
                                                texture.Get(), *source_region,
                                                candidate, failure));
            if (metrics != nullptr) {
                metrics->region_copy_microseconds += ElapsedMicroseconds(copy_start);
            }
            output.ReleaseFrame();
            if (!read) {
                return FrameAcquireStatus::Failed;
            }

            const bool carries_desktop_update = info.LastPresentTime.QuadPart != 0 || info.AccumulatedFrames != 0;
            if (carries_desktop_update && !ImageLooksBlank(candidate)) {
                out = std::move(candidate);
                return FrameAcquireStatus::Acquired;
            }
            provisional = std::move(candidate);
            have_provisional = true;
            if (!ImageLooksBlank(provisional)) {
                // Content is there even though the frame carried no update flag.
                out = std::move(provisional);
                return FrameAcquireStatus::Acquired;
            }
        }

        if (have_provisional) {
            out = std::move(provisional);
            return FrameAcquireStatus::Acquired;  // Blank; caller decides whether to fall back.
        }
        if (timeout_ms != 0 || attempts > 1) {
            LogMessage(L"Duplication produced no frame within the timeout budget");
        }
        return FrameAcquireStatus::NoNewFrame;
    }

    std::vector<OutputDuplication> outputs_;
    bool initialized_{false};
    bool using_fallback_{false};
    bool recording_frame_available_{false};
    RegionCaptureFailureDiagnostic duplication_initialization_failure_;
    /// Set from inside a capture; the interfaces are only torn down between
    /// captures so no live reference is invalidated mid-loop.
    bool needs_rebuild_{false};
};

}  // namespace

const wchar_t* RegionCaptureFailureStageName(
    RegionCaptureFailureStage stage) noexcept {
    switch (stage) {
        case RegionCaptureFailureStage::None:
            return L"none";
        case RegionCaptureFailureStage::InvalidRegion:
            return L"invalid-region";
        case RegionCaptureFailureStage::NoOverlappingMonitor:
            return L"no-overlapping-monitor";
        case RegionCaptureFailureStage::GraphicsInitialization:
            return L"graphics-initialization";
        case RegionCaptureFailureStage::D3dDeviceCreation:
            return L"d3d-device-creation";
        case RegionCaptureFailureStage::DuplicateOutput:
            return L"duplicate-output";
        case RegionCaptureFailureStage::DuplicationOutputNotFound:
            return L"duplication-output-not-found";
        case RegionCaptureFailureStage::DuplicationRotationUnsupported:
            return L"duplication-rotation-unsupported";
        case RegionCaptureFailureStage::DuplicationAcquire:
            return L"duplication-acquire";
        case RegionCaptureFailureStage::DuplicationFrameTexture:
            return L"duplication-frame-texture";
        case RegionCaptureFailureStage::DuplicationInvalidTexture:
            return L"duplication-invalid-texture";
        case RegionCaptureFailureStage::DuplicationStagingTexture:
            return L"duplication-staging-texture";
        case RegionCaptureFailureStage::DuplicationMapTexture:
            return L"duplication-map-texture";
        case RegionCaptureFailureStage::DuplicationBlankFrame:
            return L"duplication-blank-frame";
        case RegionCaptureFailureStage::GdiInvalidRegion:
            return L"gdi-invalid-region";
        case RegionCaptureFailureStage::GdiGetScreenDc:
            return L"gdi-get-screen-dc";
        case RegionCaptureFailureStage::GdiCreateMemoryDc:
            return L"gdi-create-memory-dc";
        case RegionCaptureFailureStage::GdiCreateDibSection:
            return L"gdi-create-dib-section";
        case RegionCaptureFailureStage::GdiSelectBitmap:
            return L"gdi-select-bitmap";
        case RegionCaptureFailureStage::GdiBitBlt:
            return L"gdi-bitblt";
    }
    return L"unknown";
}

// ---------------------------------------------------------------------------
// DesktopSnapshot
// ---------------------------------------------------------------------------

void DesktopSnapshot::Clear() {
    monitors.clear();
    virtual_bounds = RECT{0, 0, 0, 0};
}

bool DesktopSnapshot::CropRegion(const RECT& region, BgraImage& out) const {
    const RECT normalized = platform::IntersectRects(region, virtual_bounds);
    if (platform::IsEmptyRect(normalized) || monitors.empty()) {
        return false;
    }
    const int width = static_cast<int>(platform::RectWidth(normalized));
    const int height = static_cast<int>(platform::RectHeight(normalized));
    out.Reset(width, height);

    for (const MonitorFrame& frame : monitors) {
        const RECT overlap = platform::IntersectRects(normalized, frame.desktop_rect);
        if (platform::IsEmptyRect(overlap) || frame.image.empty()) {
            continue;
        }
        const int copy_width = static_cast<int>(platform::RectWidth(overlap));
        const int copy_height = static_cast<int>(platform::RectHeight(overlap));
        for (int y = 0; y < copy_height; ++y) {
            const int source_y = static_cast<int>(overlap.top - frame.desktop_rect.top) + y;
            const int target_y = static_cast<int>(overlap.top - normalized.top) + y;
            if (source_y < 0 || source_y >= frame.image.height || target_y < 0 || target_y >= out.height) {
                continue;
            }
            const int source_x = static_cast<int>(overlap.left - frame.desktop_rect.left);
            const int target_x = static_cast<int>(overlap.left - normalized.left);
            const int clipped_width = std::min(copy_width, frame.image.width - source_x);
            if (clipped_width <= 0) {
                continue;
            }
            std::memcpy(out.row(target_y) + target_x, frame.image.row(source_y) + source_x,
                        static_cast<size_t>(clipped_width) * sizeof(uint32_t));
        }
    }
    out.MakeOpaque();
    return true;
}

// ---------------------------------------------------------------------------
// Cursor compositing
// ---------------------------------------------------------------------------

bool CompositeCursor(BgraImage& image, const RECT& image_rect) {
    if (image.empty()) {
        return false;
    }
    CURSORINFO cursor_info{};
    cursor_info.cbSize = sizeof(cursor_info);
    if (::GetCursorInfo(&cursor_info) == FALSE || (cursor_info.flags & CURSOR_SHOWING) == 0 ||
        cursor_info.hCursor == nullptr) {
        return false;
    }

    ICONINFO icon_info{};
    if (::GetIconInfo(cursor_info.hCursor, &icon_info) == FALSE) {
        return false;
    }
    if (icon_info.hbmColor != nullptr) {
        ::DeleteObject(icon_info.hbmColor);
    }
    if (icon_info.hbmMask != nullptr) {
        ::DeleteObject(icon_info.hbmMask);
    }

    const int x = static_cast<int>(cursor_info.ptScreenPos.x - image_rect.left - static_cast<LONG>(icon_info.xHotspot));
    const int y = static_cast<int>(cursor_info.ptScreenPos.y - image_rect.top - static_cast<LONG>(icon_info.yHotspot));
    if (x > image.width || y > image.height) {
        return false;
    }

    platform::GdiImageSurface surface(image);
    if (!surface.valid()) {
        return false;
    }
    return ::DrawIconEx(surface.dc(), x, y, cursor_info.hCursor, 0, 0, 0, nullptr, DI_NORMAL) != FALSE;
}

std::unique_ptr<IRegionCaptureProvider> CreateRegionCaptureProvider() {
    return std::make_unique<DesktopDuplicationProvider>();
}

}  // namespace easnap::capture
