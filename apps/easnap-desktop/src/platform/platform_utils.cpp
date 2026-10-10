// Easnap - platform utilities implementation.
#include "platform/platform_utils.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "resource.h"

namespace easnap::platform {
namespace {

constexpr UINT kDefaultDpi = 96;

/// Dynamically resolved APIs. Easnap probes for capabilities instead of
/// branching on Windows version numbers (see docs/architecture.md).
struct DynamicApis {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);

    SetProcessDpiAwarenessContextFn set_process_dpi_awareness_context{nullptr};
    SetProcessDpiAwarenessFn set_process_dpi_awareness{nullptr};
    GetDpiForMonitorFn get_dpi_for_monitor{nullptr};

    DynamicApis() {
        if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
            set_process_dpi_awareness_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                reinterpret_cast<void*>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        }
        if (HMODULE shcore = ::LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            get_dpi_for_monitor = reinterpret_cast<GetDpiForMonitorFn>(
                reinterpret_cast<void*>(::GetProcAddress(shcore, "GetDpiForMonitor")));
            set_process_dpi_awareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
                reinterpret_cast<void*>(::GetProcAddress(shcore, "SetProcessDpiAwareness")));
        }
    }
};

const DynamicApis& Apis() {
    static const DynamicApis apis;
    return apis;
}

UINT DpiForMonitor(HMONITOR monitor) {
    UINT dpi_x = kDefaultDpi;
    UINT dpi_y = kDefaultDpi;
    if (Apis().get_dpi_for_monitor != nullptr &&
        SUCCEEDED(Apis().get_dpi_for_monitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
        return dpi_x;
    }
    return kDefaultDpi;
}

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == FALSE) {
        return TRUE;
    }
    MonitorInfo entry{};
    entry.handle = monitor;
    entry.bounds = info.rcMonitor;
    entry.work_area = info.rcWork;
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    entry.dpi = DpiForMonitor(monitor);
    monitors->push_back(entry);
    return TRUE;
}

bool WriteEncodedImage(const BgraImage& image, ImageFormat format, IStream* stream) {
    auto* wic = GraphicsEnvironment::Get().wic_factory();
    if (wic == nullptr || stream == nullptr || image.empty()) {
        return false;
    }

    const GUID container = (format == ImageFormat::Jpeg) ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
    ComPtr<IWICBitmapEncoder> encoder;
    HRESULT hr = wic->CreateEncoder(container, nullptr, &encoder);
    if (FAILED(hr)) {
        LogHresult(L"WIC CreateEncoder", hr);
        return false;
    }
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        LogHresult(L"WIC encoder Initialize", hr);
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    hr = encoder->CreateNewFrame(&frame, &options);
    if (FAILED(hr)) {
        LogHresult(L"WIC CreateNewFrame", hr);
        return false;
    }
    hr = frame->Initialize(options.Get());
    if (FAILED(hr)) {
        return false;
    }
    hr = frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height));
    if (FAILED(hr)) {
        return false;
    }

    // JPEG has no alpha channel; BGR is the closest native match.
    WICPixelFormatGUID pixel_format =
        (format == ImageFormat::Jpeg) ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&pixel_format);
    if (FAILED(hr)) {
        return false;
    }

    if (::IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA)) {
        const UINT stride = static_cast<UINT>(image.stride_bytes());
        const UINT size = stride * static_cast<UINT>(image.height);
        hr = frame->WritePixels(static_cast<UINT>(image.height), stride, size,
                                reinterpret_cast<BYTE*>(const_cast<uint32_t*>(image.pixels.data())));
    } else {
        // Convert to the format the encoder negotiated.
        ComPtr<IWICBitmap> source;
        hr = wic->CreateBitmapFromMemory(static_cast<UINT>(image.width), static_cast<UINT>(image.height),
                                         GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(image.stride_bytes()),
                                         static_cast<UINT>(image.pixels.size() * sizeof(uint32_t)),
                                         reinterpret_cast<BYTE*>(const_cast<uint32_t*>(image.pixels.data())),
                                         &source);
        if (FAILED(hr)) {
            return false;
        }
        ComPtr<IWICFormatConverter> converter;
        hr = wic->CreateFormatConverter(&converter);
        if (FAILED(hr)) {
            return false;
        }
        hr = converter->Initialize(source.Get(), pixel_format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            return false;
        }
        hr = frame->WriteSource(converter.Get(), nullptr);
    }

    if (FAILED(hr)) {
        LogHresult(L"WIC WritePixels", hr);
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        LogHresult(L"WIC encoder Commit", hr);
        return false;
    }
    return true;
}

bool PutClipboardDib(const BgraImage& image) {
    // 32bpp BI_RGB DIB: the format virtually every Windows application accepts.
    const size_t pixel_bytes = image.pixels.size() * sizeof(uint32_t);
    const size_t total = sizeof(BITMAPINFOHEADER) + pixel_bytes;
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, total);
    if (memory == nullptr) {
        return false;
    }
    auto* header = static_cast<BITMAPINFOHEADER*>(::GlobalLock(memory));
    if (header == nullptr) {
        ::GlobalFree(memory);
        return false;
    }
    *header = BITMAPINFOHEADER{};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = image.width;
    header->biHeight = -image.height;  // top-down
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixel_bytes);
    std::memcpy(header + 1, image.pixels.data(), pixel_bytes);
    ::GlobalUnlock(memory);

    if (::SetClipboardData(CF_DIB, memory) == nullptr) {
        ::GlobalFree(memory);
        return false;
    }
    return true;  // Ownership transferred to the clipboard.
}

bool PutClipboardPng(const BgraImage& image) {
    static const UINT png_format = ::RegisterClipboardFormatW(L"PNG");
    if (png_format == 0) {
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!EncodeImage(image, ImageFormat::Png, bytes) || bytes.empty()) {
        return false;
    }
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (memory == nullptr) {
        return false;
    }
    void* target = ::GlobalLock(memory);
    if (target == nullptr) {
        ::GlobalFree(memory);
        return false;
    }
    std::memcpy(target, bytes.data(), bytes.size());
    ::GlobalUnlock(memory);
    if (::SetClipboardData(png_format, memory) == nullptr) {
        ::GlobalFree(memory);
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// GdiImageSurface
// ---------------------------------------------------------------------------

GdiImageSurface::GdiImageSurface(BgraImage& image) : image_(image) {
    if (image.empty()) {
        return;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = image.width;
    bmi.bmiHeader.biHeight = -image.height;  // top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    dc_ = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (dc_ == nullptr) {
        return;
    }
    bitmap_ = ::CreateDIBSection(dc_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (bitmap_ == nullptr) {
        ::DeleteDC(dc_);
        dc_ = nullptr;
        return;
    }
    previous_ = static_cast<HBITMAP>(::SelectObject(dc_, bitmap_));
    std::memcpy(bits_, image.pixels.data(), image.pixels.size() * sizeof(uint32_t));
}

GdiImageSurface::~GdiImageSurface() {
    if (dc_ == nullptr) {
        return;
    }
    ::GdiFlush();
    if (bits_ != nullptr) {
        std::memcpy(image_.pixels.data(), bits_, image_.pixels.size() * sizeof(uint32_t));
        image_.MakeOpaque();
    }
    ::SelectObject(dc_, previous_);
    ::DeleteObject(bitmap_);
    ::DeleteDC(dc_);
}

// ---------------------------------------------------------------------------
// ComRuntime
// ---------------------------------------------------------------------------

ComRuntime::ComRuntime() {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    initialized_ = SUCCEEDED(hr);
    if (!initialized_) {
        LogHresult(L"CoInitializeEx", hr);
    }
}

ComRuntime::~ComRuntime() {
    if (initialized_) {
        ::CoUninitialize();
    }
}

// ---------------------------------------------------------------------------
// DPI and monitors
// ---------------------------------------------------------------------------

void RestrictDllSearchPath() {
    // Dynamic loads by bare name resolve from System32 (and directories added
    // explicitly), never from the directory a portable copy was unpacked into.
    // Static imports are covered by the /DEPENDENTLOADFLAG linker option.
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
}

void EnableDpiAwareness() {
    // The product executables declare per-monitor v2 in their manifests, which
    // makes the calls below fail harmlessly; they serve manifest-less tests.
    if (Apis().set_process_dpi_awareness_context != nullptr &&
        Apis().set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
        return;
    }
    if (Apis().set_process_dpi_awareness != nullptr &&
        SUCCEEDED(Apis().set_process_dpi_awareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
        return;
    }
    ::SetProcessDPIAware();
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    ::EnumDisplayMonitors(nullptr, nullptr, &MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        // Extremely defensive fallback: synthesize a primary monitor entry.
        MonitorInfo primary{};
        primary.bounds = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
        primary.work_area = primary.bounds;
        primary.primary = true;
        monitors.push_back(primary);
    }
    return monitors;
}

MonitorInfo MonitorForHandle(HMONITOR handle) {
    const std::vector<MonitorInfo> monitors = EnumerateMonitors();
    for (const MonitorInfo& monitor : monitors) {
        if (monitor.handle == handle) {
            return monitor;
        }
    }
    return monitors.front();
}

MonitorInfo MonitorForPoint(POINT pt) {
    return MonitorForHandle(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

MonitorInfo MonitorForRect(const RECT& rect) {
    return MonitorForHandle(::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST));
}

RECT VirtualScreenBounds() {
    RECT bounds{};
    bounds.left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds.top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds.right = bounds.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds.bottom = bounds.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return bounds;
}

RECT NormalizedRect(POINT a, POINT b) {
    RECT r{};
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    return r;
}

RECT IntersectRects(const RECT& a, const RECT& b) {
    RECT out{};
    if (::IntersectRect(&out, &a, &b) == FALSE) {
        return RECT{0, 0, 0, 0};
    }
    return out;
}

bool IsEmptyRect(const RECT& r) noexcept { return r.right <= r.left || r.bottom <= r.top; }

RECT ClampRectToBounds(RECT rect, const RECT& bounds) {
    const LONG width = std::min(RectWidth(rect), RectWidth(bounds));
    const LONG height = std::min(RectHeight(rect), RectHeight(bounds));
    rect.left = std::clamp(rect.left, bounds.left, bounds.right - width);
    rect.top = std::clamp(rect.top, bounds.top, bounds.bottom - height);
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

// ---------------------------------------------------------------------------
// BgraImage
// ---------------------------------------------------------------------------

void BgraImage::Reset(int w, int h) {
    width = std::max(0, w);
    height = std::max(0, h);
    pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xFF000000u);
}

void BgraImage::MakeOpaque() noexcept {
    for (uint32_t& pixel : pixels) {
        pixel |= 0xFF000000u;
    }
}

void ApplyMosaic(BgraImage& image, const RECT& area, int block_size) {
    if (image.empty() || block_size <= 1) {
        return;
    }
    const RECT clipped = IntersectRects(area, RECT{0, 0, image.width, image.height});
    if (IsEmptyRect(clipped)) {
        return;
    }
    for (LONG y = clipped.top; y < clipped.bottom; y += block_size) {
        const LONG y_end = std::min<LONG>(y + block_size, clipped.bottom);
        for (LONG x = clipped.left; x < clipped.right; x += block_size) {
            const LONG x_end = std::min<LONG>(x + block_size, clipped.right);
            uint64_t b = 0, g = 0, r = 0;
            uint64_t count = 0;
            for (LONG yy = y; yy < y_end; ++yy) {
                const uint32_t* row = image.row(static_cast<int>(yy));
                for (LONG xx = x; xx < x_end; ++xx) {
                    const uint32_t pixel = row[xx];
                    b += pixel & 0xFFu;
                    g += (pixel >> 8) & 0xFFu;
                    r += (pixel >> 16) & 0xFFu;
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const uint32_t average = 0xFF000000u | (static_cast<uint32_t>(r / count) << 16) |
                                     (static_cast<uint32_t>(g / count) << 8) | static_cast<uint32_t>(b / count);
            for (LONG yy = y; yy < y_end; ++yy) {
                uint32_t* row = image.row(static_cast<int>(yy));
                for (LONG xx = x; xx < x_end; ++xx) {
                    row[xx] = average;
                }
            }
        }
    }
}

void ApplyMosaicMasked(BgraImage& image, const std::vector<uint8_t>& coverage, uint8_t min_level,
                       const RECT& area, int block_size) {
    if (image.empty() || block_size <= 1 || min_level == 0) {
        return;
    }
    if (coverage.size() != static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) {
        return;
    }
    const RECT clipped = IntersectRects(area, RECT{0, 0, image.width, image.height});
    if (IsEmptyRect(clipped)) {
        return;
    }

    const auto covered = [&](LONG x, LONG y) {
        return coverage[static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)] >=
               min_level;
    };

    for (LONG y = clipped.top; y < clipped.bottom; y += block_size) {
        const LONG y_end = std::min<LONG>(y + block_size, clipped.bottom);
        for (LONG x = clipped.left; x < clipped.right; x += block_size) {
            const LONG x_end = std::min<LONG>(x + block_size, clipped.right);
            uint64_t b = 0, g = 0, r = 0;
            uint64_t count = 0;
            for (LONG yy = y; yy < y_end; ++yy) {
                const uint32_t* row = image.row(static_cast<int>(yy));
                for (LONG xx = x; xx < x_end; ++xx) {
                    if (!covered(xx, yy)) {
                        continue;
                    }
                    const uint32_t pixel = row[xx];
                    b += pixel & 0xFFu;
                    g += (pixel >> 8) & 0xFFu;
                    r += (pixel >> 16) & 0xFFu;
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const uint32_t average = 0xFF000000u | (static_cast<uint32_t>(r / count) << 16) |
                                     (static_cast<uint32_t>(g / count) << 8) | static_cast<uint32_t>(b / count);
            for (LONG yy = y; yy < y_end; ++yy) {
                uint32_t* row = image.row(static_cast<int>(yy));
                for (LONG xx = x; xx < x_end; ++xx) {
                    if (covered(xx, yy)) {
                        row[xx] = average;
                    }
                }
            }
        }
    }
}

bool CropImage(const BgraImage& source, const RECT& source_region, BgraImage& out) {
    const RECT region = IntersectRects(source_region, RECT{0, 0, source.width, source.height});
    if (source.empty() || IsEmptyRect(region)) {
        return false;
    }
    const int width = static_cast<int>(RectWidth(region));
    const int height = static_cast<int>(RectHeight(region));
    out.Reset(width, height);
    for (int y = 0; y < height; ++y) {
        const uint32_t* src = source.row(static_cast<int>(region.top) + y) + region.left;
        std::memcpy(out.row(y), src, static_cast<size_t>(width) * sizeof(uint32_t));
    }
    return true;
}

// ---------------------------------------------------------------------------
// GraphicsEnvironment
// ---------------------------------------------------------------------------

GraphicsEnvironment& GraphicsEnvironment::Get() {
    static GraphicsEnvironment instance;
    return instance;
}

bool GraphicsEnvironment::EnsureInitialized() {
    if (initialized_) {
        return true;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG) && defined(EASNAP_D3D_DEBUG_LAYER)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    static constexpr std::array<D3D_FEATURE_LEVEL, 4> kFeatureLevels = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kFeatureLevels.data(),
                                     static_cast<UINT>(kFeatureLevels.size()), D3D11_SDK_VERSION, &d3d_device_,
                                     &obtained, &d3d_context_);
    if (FAILED(hr)) {
        // WARP keeps Easnap usable inside VMs and on drivers without D3D11.
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, kFeatureLevels.data(),
                                 static_cast<UINT>(kFeatureLevels.size()), D3D11_SDK_VERSION, &d3d_device_,
                                 &obtained, &d3d_context_);
    }
    if (FAILED(hr)) {
        LogHresult(L"D3D11CreateDevice", hr);
        return false;
    }

    hr = d3d_device_.As(&dxgi_device_);
    if (FAILED(hr)) {
        LogHresult(L"QueryInterface IDXGIDevice1", hr);
        return false;
    }
    dxgi_device_->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device_->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) {
        hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory_));
    }
    if (FAILED(hr)) {
        LogHresult(L"IDXGIFactory2 lookup", hr);
        return false;
    }

    D2D1_FACTORY_OPTIONS factory_options{};
#if defined(_DEBUG) && defined(EASNAP_D2D_DEBUG_LAYER)
    factory_options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &factory_options,
                             reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        LogHresult(L"D2D1CreateFactory", hr);
        return false;
    }
    hr = d2d_factory_->CreateDevice(dxgi_device_.Get(), &d2d_device_);
    if (FAILED(hr)) {
        LogHresult(L"ID2D1Factory1::CreateDevice", hr);
        return false;
    }

    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        LogHresult(L"DWriteCreateFactory", hr);
        return false;
    }

    hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory_));
    if (FAILED(hr)) {
        LogHresult(L"CoCreateInstance(WICImagingFactory)", hr);
        return false;
    }

    initialized_ = true;
    LogMessage(L"Graphics environment ready (feature level 0x%04X)", static_cast<unsigned>(obtained));
    return true;
}

void GraphicsEnvironment::Shutdown() {
    wic_factory_.Reset();
    dwrite_factory_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    dxgi_factory_.Reset();
    dxgi_device_.Reset();
    d3d_context_.Reset();
    d3d_device_.Reset();
    initialized_ = false;
}

void GraphicsEnvironment::HandleDeviceLost() {
    LogMessage(L"Graphics device lost; recreating on next use");
    Shutdown();
}

ComPtr<IDWriteTextFormat> GraphicsEnvironment::CreateTextFormat(float size_dip, DWRITE_FONT_WEIGHT weight,
                                                                const wchar_t* family) {
    ComPtr<IDWriteTextFormat> format;
    if (dwrite_factory_ == nullptr) {
        return format;
    }
    const HRESULT hr = dwrite_factory_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                         DWRITE_FONT_STRETCH_NORMAL, size_dip, L"en-us", &format);
    if (FAILED(hr)) {
        LogHresult(L"CreateTextFormat", hr);
    }
    return format;
}

// ---------------------------------------------------------------------------
// SwapChainTarget
// ---------------------------------------------------------------------------

SwapChainTarget::~SwapChainTarget() { Reset(); }

bool SwapChainTarget::Create(HWND hwnd, UINT width, UINT height) {
    GraphicsEnvironment& graphics = GraphicsEnvironment::Get();
    if (!graphics.EnsureInitialized() || hwnd == nullptr) {
        return false;
    }
    width_ = std::max<UINT>(width, 1);
    height_ = std::max<UINT>(height, 1);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Scaling = DXGI_SCALING_STRETCH;

    HRESULT hr = graphics.dxgi_factory()->CreateSwapChainForHwnd(graphics.d3d_device(), hwnd, &desc, nullptr,
                                                                 nullptr, &swap_chain_);
    if (FAILED(hr)) {
        LogHresult(L"CreateSwapChainForHwnd", hr);
        return false;
    }
    // Easnap manages its own Alt+Enter/window state; DXGI must not interfere.
    graphics.dxgi_factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    hr = graphics.d2d_device()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_);
    if (FAILED(hr)) {
        LogHresult(L"CreateDeviceContext", hr);
        Reset();
        return false;
    }
    context_->SetDpi(96.0f, 96.0f);  // 1 DIP == 1 physical pixel, see header note.
    context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (!BindBackBuffer()) {
        Reset();
        return false;
    }
    return true;
}

bool SwapChainTarget::BindBackBuffer() {
    if (swap_chain_ == nullptr || context_ == nullptr) {
        return false;
    }
    ComPtr<IDXGISurface> surface;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) {
        LogHresult(L"IDXGISwapChain1::GetBuffer", hr);
        return false;
    }
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> target;
    hr = context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target);
    if (FAILED(hr)) {
        LogHresult(L"CreateBitmapFromDxgiSurface", hr);
        return false;
    }
    context_->SetTarget(target.Get());
    return true;
}

bool SwapChainTarget::Resize(UINT width, UINT height) {
    if (swap_chain_ == nullptr) {
        return false;
    }
    width = std::max<UINT>(width, 1);
    height = std::max<UINT>(height, 1);
    if (width == width_ && height == height_) {
        return true;
    }
    context_->SetTarget(nullptr);
    const HRESULT hr = swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LogHresult(L"ResizeBuffers", hr);
        return false;
    }
    width_ = width;
    height_ = height;
    return BindBackBuffer();
}

bool SwapChainTarget::Present() {
    if (swap_chain_ == nullptr) {
        return false;
    }
    const HRESULT hr = swap_chain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        GraphicsEnvironment::Get().HandleDeviceLost();
        return false;
    }
    return SUCCEEDED(hr);
}

void SwapChainTarget::Reset() {
    if (context_ != nullptr) {
        context_->SetTarget(nullptr);
    }
    context_.Reset();
    swap_chain_.Reset();
    width_ = 0;
    height_ = 0;
}

ComPtr<ID2D1Bitmap1> CreateD2DBitmap(ID2D1DeviceContext* context, const BgraImage& image) {
    ComPtr<ID2D1Bitmap1> bitmap;
    if (context == nullptr || image.empty()) {
        return bitmap;
    }
    const D2D1_BITMAP_PROPERTIES1 properties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(image.width), static_cast<UINT32>(image.height));
    const HRESULT hr = context->CreateBitmap(size, image.pixels.data(), static_cast<UINT32>(image.stride_bytes()),
                                             &properties, &bitmap);
    if (FAILED(hr)) {
        LogHresult(L"ID2D1DeviceContext::CreateBitmap", hr);
    }
    return bitmap;
}

ComPtr<ID2D1Bitmap> CreateD2DBitmapForTarget(ID2D1RenderTarget* target, const BgraImage& image) {
    ComPtr<ID2D1Bitmap> bitmap;
    if (target == nullptr || image.empty()) {
        return bitmap;
    }
    const D2D1_BITMAP_PROPERTIES properties =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(image.width), static_cast<UINT32>(image.height));
    const HRESULT hr =
        target->CreateBitmap(size, image.pixels.data(), static_cast<UINT32>(image.stride_bytes()), properties,
                             &bitmap);
    if (FAILED(hr)) {
        LogHresult(L"ID2D1RenderTarget::CreateBitmap", hr);
    }
    return bitmap;
}

// ---------------------------------------------------------------------------
// Export helpers
// ---------------------------------------------------------------------------

ImageFormat FormatFromPath(const std::wstring& path) {
    const wchar_t* extension = ::PathFindExtensionW(path.c_str());
    if (extension != nullptr &&
        (::StrCmpIW(extension, L".jpg") == 0 || ::StrCmpIW(extension, L".jpeg") == 0)) {
        return ImageFormat::Jpeg;
    }
    return ImageFormat::Png;
}

bool EncodeImage(const BgraImage& image, ImageFormat format, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    if (!GraphicsEnvironment::Get().EnsureInitialized()) {
        return false;
    }
    ComPtr<IStream> stream;
    HRESULT hr = ::CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) {
        LogHresult(L"CreateStreamOnHGlobal", hr);
        return false;
    }
    if (!WriteEncodedImage(image, format, stream.Get())) {
        return false;
    }

    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.HighPart != 0 || stat.cbSize.LowPart == 0) {
        return false;
    }
    out_bytes.resize(stat.cbSize.LowPart);
    LARGE_INTEGER zero{};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) {
        return false;
    }
    ULONG read = 0;
    hr = stream->Read(out_bytes.data(), static_cast<ULONG>(out_bytes.size()), &read);
    if (FAILED(hr) || read != out_bytes.size()) {
        out_bytes.clear();
        return false;
    }
    return true;
}

bool SaveImage(const BgraImage& image, const std::wstring& path) {
    if (!GraphicsEnvironment::Get().EnsureInitialized() || path.empty()) {
        return false;
    }
    // The destination always comes from the system picker, so the path is
    // already canonicalised by the shell; it is only opened, never composed.
    // Encode beside the destination and swap the finished file in. A failed
    // export then leaves an existing file at this path exactly as it was.
    wchar_t suffix[48]{};
    ::swprintf_s(suffix, L".easnap-%08lx%016llx.tmp", ::GetCurrentProcessId(), ::GetTickCount64());
    const std::wstring staged = path + suffix;
    ComPtr<IStream> stream;
    HRESULT hr = ::SHCreateStreamOnFileEx(staged.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                          FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
    if (FAILED(hr)) {
        LogHresult(L"SHCreateStreamOnFileEx", hr);
        return false;
    }
    const bool encoded = WriteEncodedImage(image, FormatFromPath(path), stream.Get()) &&
                         SUCCEEDED(stream->Commit(STGC_DEFAULT));
    stream.Reset();
    if (!encoded ||
        ::MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        if (encoded) {
            LogMessage(L"SaveImage could not replace the destination: %lu", ::GetLastError());
        }
        ::DeleteFileW(staged.c_str());  // Never leave a partial export behind.
        return false;
    }
    return true;
}

bool CopyImageToClipboard(HWND owner, const BgraImage& image) {
    if (image.empty()) {
        return false;
    }
    if (::OpenClipboard(owner) == FALSE) {
        LogMessage(L"OpenClipboard failed (error %lu)", ::GetLastError());
        return false;
    }
    bool ok = false;
    if (::EmptyClipboard() != FALSE) {
        // CF_DIB is the reliable baseline; PNG is offered for apps that want it.
        ok = PutClipboardDib(image);
        PutClipboardPng(image);
    }
    ::CloseClipboard();
    return ok;
}

std::optional<std::wstring> ShowSaveImageDialog(HWND owner, const std::wstring& suggested_file_name,
                                                const std::wstring& initial_directory) {
    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = ::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) {
        LogHresult(L"CoCreateInstance(FileSaveDialog)", hr);
        return std::nullopt;
    }

    static const COMDLG_FILTERSPEC kFilters[] = {
        {L"PNG image", L"*.png"},
        {L"JPEG image", L"*.jpg;*.jpeg"},
    };
    dialog->SetFileTypes(ARRAYSIZE(kFilters), kFilters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"png");
    dialog->SetTitle(L"Save screenshot");
    dialog->SetFileName(suggested_file_name.c_str());

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM |
                           FOS_NOREADONLYRETURN);
    }

    if (!initial_directory.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(initial_directory.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetDefaultFolder(folder.Get());
        }
    } else if (auto pictures = KnownFolder(FOLDERID_Pictures)) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(pictures->c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetDefaultFolder(folder.Get());
        }
    }

    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::nullopt;
    }
    if (FAILED(hr)) {
        LogHresult(L"IFileSaveDialog::Show", hr);
        return std::nullopt;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return std::nullopt;
    }
    PWSTR raw_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || raw_path == nullptr) {
        return std::nullopt;
    }
    std::wstring result(raw_path);
    ::CoTaskMemFree(raw_path);
    return result;
}

std::optional<std::wstring> KnownFolder(REFKNOWNFOLDERID folder_id) {
    PWSTR raw_path = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(folder_id, KF_FLAG_CREATE, nullptr, &raw_path);
    if (FAILED(hr) || raw_path == nullptr) {
        LogHresult(L"SHGetKnownFolderPath", hr);
        return std::nullopt;
    }
    std::wstring path(raw_path);
    ::CoTaskMemFree(raw_path);
    return path;
}

std::wstring AppDataDirectory() {
    auto local = KnownFolder(FOLDERID_LocalAppData);
    if (!local) {
        return {};
    }
    std::wstring directory = *local + L"\\Easnap";
    EnsureDirectory(directory);
    return directory;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

std::wstring TimestampedFileName(const wchar_t* prefix, const wchar_t* extension) {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t buffer[128]{};
    ::swprintf(buffer, ARRAYSIZE(buffer), L"%s_%04u-%02u-%02u_%02u-%02u-%02u.%s", prefix, now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond, extension);
    return buffer;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    if (::CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool RemoveDirectoryTree(const std::wstring& path) {
    const auto absolute_path = [](const std::wstring& input) -> std::wstring {
        if (input.empty() || input.find(L'\0') != std::wstring::npos) {
            return {};
        }
        const DWORD capacity = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
        if (capacity == 0) {
            return {};
        }
        std::wstring result(capacity, L'\0');
        const DWORD length = ::GetFullPathNameW(input.c_str(), capacity, result.data(), nullptr);
        if (length == 0 || length >= capacity) {
            return {};
        }
        result.resize(length);
        while (!result.empty() && result.back() == L'\\') {
            result.pop_back();
        }
        return result;
    };
    const std::wstring root = absolute_path(AppDataDirectory());
    const std::wstring target = absolute_path(path);
    // Require a complete path component: Easnap-other is not inside Easnap.
    if (root.empty() || target.size() <= root.size() || target[root.size()] != L'\\' ||
        ::CompareStringOrdinal(target.c_str(), static_cast<int>(root.size()),
                               root.c_str(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) {
        LogMessage(L"Refusing to delete a directory outside the Easnap data folder");
        return false;
    }

    // Pin every ancestor below the trusted storage root. Refuse reparse points
    // and deny write/delete sharing so a checked directory cannot be replaced
    // or converted to a junction while its descendants are being removed.
    // FILE_LIST_DIRECTORY makes the handle participate in sharing checks;
    // metadata-only FILE_READ_ATTRIBUTES handles do not enforce this exclusion.
    std::vector<UniqueHandle> ancestors;
    for (std::size_t end = root.size(); end != std::wstring::npos;
         end = target.find(L'\\', end + 1)) {
        UniqueHandle handle(::CreateFileW(target.substr(0, end).c_str(),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle.get() == INVALID_HANDLE_VALUE ||
            !::GetFileInformationByHandle(handle.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
        ancestors.push_back(std::move(handle));
    }

    // Children are enumerated and opened by path. A checked directory could be
    // turned into a junction between its check and that open (a metadata-only
    // open is exempt from the sharing denial above), so every opened handle
    // must still resolve to a direct child of the directory it was listed in.
    const auto final_path = [](HANDLE handle) -> std::wstring {
        std::wstring result(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = ::GetFinalPathNameByHandleW(
                handle, result.data(), static_cast<DWORD>(result.size()),
                FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
            if (length == 0) {
                return {};
            }
            if (length < result.size()) {
                result.resize(length);
                return result;
            }
            result.resize(static_cast<std::size_t>(length) + 1);
        }
    };
    const auto is_direct_child = [](const std::wstring& child, const std::wstring& parent) {
        return !parent.empty() && child.size() > parent.size() + 1 &&
               child[parent.size()] == L'\\' &&
               child.find(L'\\', parent.size() + 1) == std::wstring::npos &&
               ::CompareStringOrdinal(child.c_str(), static_cast<int>(parent.size()), parent.c_str(),
                                      static_cast<int>(parent.size()), TRUE) == CSTR_EQUAL;
    };

    const auto remove_entry = [&final_path, &is_direct_child](
                                  const auto& self, const std::wstring& entry_path,
                                  const std::wstring& parent_final, bool require_directory) -> bool {
        UniqueHandle handle(::CreateFileW(entry_path.c_str(),
            DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle.get() == INVALID_HANDLE_VALUE ||
            !::GetFileInformationByHandle(handle.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (require_directory && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)) {
            return false;
        }
        const std::wstring entry_final = final_path(handle.get());
        if (!is_direct_child(entry_final, parent_final)) {
            LogMessage(L"Refusing to delete an entry that resolved outside its listed directory");
            return false;
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            WIN32_FIND_DATAW entry{};
            const HANDLE find = ::FindFirstFileExW((entry_path + L"\\*").c_str(), FindExInfoBasic,
                &entry, FindExSearchNameMatch, nullptr, 0);
            if (find == INVALID_HANDLE_VALUE) {
                if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
                    return false;
                }
            } else {
                const auto close_find = [](void* value) { ::FindClose(value); };
                std::unique_ptr<void, decltype(close_find)> search(find, close_find);
                do {
                    const std::wstring name(entry.cFileName);
                    if (name != L"." && name != L".." && !self(self, entry_path + L"\\" + name, entry_final, false)) {
                        return false;
                    }
                } while (::FindNextFileW(find, &entry) != FALSE);
                if (::GetLastError() != ERROR_NO_MORE_FILES) {
                    return false;
                }
            }
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            FILE_BASIC_INFO basic{};
            basic.FileAttributes = info.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY;
            if (basic.FileAttributes == 0) {
                basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }
            if (!::SetFileInformationByHandle(handle.get(), FileBasicInfo, &basic, sizeof(basic))) {
                return false;
            }
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        return ::SetFileInformationByHandle(handle.get(), FileDispositionInfo,
                                            &disposition, sizeof(disposition)) != FALSE;
    };
    return remove_entry(remove_entry, target, final_path(ancestors.back().get()), true);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void LogMessage(const wchar_t* format, ...) {
    wchar_t buffer[512]{};
    va_list args;
    va_start(args, format);
    ::vswprintf(buffer, ARRAYSIZE(buffer), format, args);
    va_end(args);

    wchar_t line[600]{};
    ::swprintf(line, ARRAYSIZE(line), L"[easnap] %s\n", buffer);
    ::OutputDebugStringW(line);
}

void LogHresult(const wchar_t* what, HRESULT hr) { LogMessage(L"%s failed (hr=0x%08X)", what, static_cast<unsigned>(hr)); }

void ShowErrorMessage(HWND owner, const wchar_t* text) {
    ::MessageBoxW(owner, text, L"Easnap", MB_OK | MB_ICONWARNING);
}

HICON LoadAppIcon(int desired_size) {
    HMODULE module = ::GetModuleHandleW(nullptr);

    // Capability detection instead of an OS version check: LoadIconWithScaleDown
    // picks the best matching size out of the multi-resolution .ico and scales it
    // with a high quality filter. LoadImageW is the documented fallback.
    using LoadIconWithScaleDownFn = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);
    if (HMODULE comctl = ::LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        const auto load_scaled =
            reinterpret_cast<LoadIconWithScaleDownFn>(
                reinterpret_cast<void*>(::GetProcAddress(comctl, "LoadIconWithScaleDown")));
        if (load_scaled != nullptr) {
            HICON icon = nullptr;
            const HRESULT hr = load_scaled(module, MAKEINTRESOURCEW(IDI_EASNAP_APP), desired_size, desired_size, &icon);
            if (SUCCEEDED(hr) && icon != nullptr) {
                // comctl32 stays loaded for the process lifetime on purpose: the
                // returned icon must outlive this call.
                return icon;
            }
        }
        ::FreeLibrary(comctl);
    }

    return static_cast<HICON>(::LoadImageW(module, MAKEINTRESOURCEW(IDI_EASNAP_APP), IMAGE_ICON, desired_size,
                                           desired_size, LR_DEFAULTCOLOR));
}

}  // namespace easnap::platform
