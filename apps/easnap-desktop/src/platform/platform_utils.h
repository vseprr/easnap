// Easnap - platform utilities.
//
// This header isolates every Win32/COM/Direct2D detail the rest of the
// application needs: DPI awareness, monitor enumeration, the shared graphics
// environment, image containers, clipboard and file export helpers.
//
// Feature code must never call raw Win32 APIs that are wrapped here, so the
// Windows 11 / future-platform work stays confined to this layer.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <d2d1_1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace easnap::platform {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// ---------------------------------------------------------------------------
// COM / lifetime helpers
// ---------------------------------------------------------------------------

/// RAII wrapper around CoInitializeEx for a single thread.
class ComRuntime {
public:
    ComRuntime();
    ~ComRuntime();

    ComRuntime(const ComRuntime&) = delete;
    ComRuntime& operator=(const ComRuntime&) = delete;

    [[nodiscard]] bool ok() const noexcept { return initialized_; }

private:
    bool initialized_{false};
};

/// Deleter that lets std::unique_ptr own GDI/USER handles.
struct WindowDeleter {
    void operator()(HWND hwnd) const noexcept {
        if (hwnd != nullptr) {
            ::DestroyWindow(hwnd);
        }
    }
};
using UniqueWindow = std::unique_ptr<std::remove_pointer_t<HWND>, WindowDeleter>;

/// Deleter for kernel objects returned as HANDLE (mutexes, files, events).
struct HandleDeleter {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

// ---------------------------------------------------------------------------
// Geometry helpers (all coordinates are virtual-desktop pixels unless noted)
// ---------------------------------------------------------------------------

struct MonitorInfo {
    HMONITOR handle{nullptr};
    RECT bounds{};     ///< Full monitor rectangle in virtual-screen pixels.
    RECT work_area{};  ///< Monitor rectangle without taskbar/app bars.
    UINT dpi{96};
    bool primary{false};

    [[nodiscard]] float scale() const noexcept { return static_cast<float>(dpi) / 96.0f; }
};

/// Call first in every process entry point, before any library is loaded.
void RestrictDllSearchPath();
void EnableDpiAwareness();
[[nodiscard]] std::vector<MonitorInfo> EnumerateMonitors();
[[nodiscard]] MonitorInfo MonitorForPoint(POINT pt);
[[nodiscard]] MonitorInfo MonitorForRect(const RECT& rect);
[[nodiscard]] RECT VirtualScreenBounds();

[[nodiscard]] RECT NormalizedRect(POINT a, POINT b);
[[nodiscard]] RECT IntersectRects(const RECT& a, const RECT& b);
[[nodiscard]] bool IsEmptyRect(const RECT& r) noexcept;
[[nodiscard]] inline LONG RectWidth(const RECT& r) noexcept { return r.right - r.left; }
[[nodiscard]] inline LONG RectHeight(const RECT& r) noexcept { return r.bottom - r.top; }
[[nodiscard]] RECT ClampRectToBounds(RECT rect, const RECT& bounds);

// ---------------------------------------------------------------------------
// CPU-side image container (32bpp BGRA, top-down, no padding)
// ---------------------------------------------------------------------------

struct BgraImage {
    int width{0};
    int height{0};
    std::vector<uint32_t> pixels;

    void Reset(int w, int h);
    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0 || pixels.empty(); }
    [[nodiscard]] size_t stride_bytes() const noexcept { return static_cast<size_t>(width) * 4u; }
    [[nodiscard]] uint32_t* row(int y) noexcept { return pixels.data() + static_cast<size_t>(y) * width; }
    [[nodiscard]] const uint32_t* row(int y) const noexcept {
        return pixels.data() + static_cast<size_t>(y) * width;
    }
    /// Forces every pixel to be fully opaque (screenshots are never transparent).
    void MakeOpaque() noexcept;
};

/// Temporarily wraps a BgraImage in a top-down 32bpp DIB section so GDI can draw
/// into it (used for cursor compositing). Pixels are copied back on destruction.
class GdiImageSurface {
public:
    explicit GdiImageSurface(BgraImage& image);
    ~GdiImageSurface();

    GdiImageSurface(const GdiImageSurface&) = delete;
    GdiImageSurface& operator=(const GdiImageSurface&) = delete;

    [[nodiscard]] HDC dc() const noexcept { return dc_; }
    [[nodiscard]] bool valid() const noexcept { return dc_ != nullptr && bits_ != nullptr; }

private:
    BgraImage& image_;
    HDC dc_{nullptr};
    HBITMAP bitmap_{nullptr};
    HBITMAP previous_{nullptr};
    void* bits_{nullptr};
};

/// Averages `block` x `block` pixel blocks inside `area` (mosaic / privacy blur).
void ApplyMosaic(BgraImage& image, const RECT& area, int block_size);

/// Mosaic restricted to the pixels whose `coverage` value is >= `min_level`.
///
/// `coverage` is a width*height map produced by the caller (one entry per pixel,
/// counting how many brush strokes painted it). Running this once per level with
/// a growing `block_size` is what makes repeated painting progressively coarser
/// without touching the pixels a user only painted over once.
void ApplyMosaicMasked(BgraImage& image, const std::vector<uint8_t>& coverage, uint8_t min_level,
                       const RECT& area, int block_size);

/// Copies `source_region` (image coordinates) out of `source` into `out`.
bool CropImage(const BgraImage& source, const RECT& source_region, BgraImage& out);

// ---------------------------------------------------------------------------
// Shared Direct3D/Direct2D/DirectWrite/WIC environment
// ---------------------------------------------------------------------------

/// Process-wide graphics singleton. Created lazily on first capture so the idle
/// notification-area process keeps its promise of not initialising any capture,
/// encoder or renderer resources while in the Idle state.
class GraphicsEnvironment {
public:
    static GraphicsEnvironment& Get();

    bool EnsureInitialized();
    void Shutdown();

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] ID2D1Factory1* d2d_factory() const noexcept { return d2d_factory_.Get(); }
    [[nodiscard]] ID2D1Device* d2d_device() const noexcept { return d2d_device_.Get(); }
    [[nodiscard]] IDWriteFactory* dwrite_factory() const noexcept { return dwrite_factory_.Get(); }
    [[nodiscard]] IWICImagingFactory* wic_factory() const noexcept { return wic_factory_.Get(); }
    [[nodiscard]] ID3D11Device* d3d_device() const noexcept { return d3d_device_.Get(); }
    [[nodiscard]] IDXGIFactory2* dxgi_factory() const noexcept { return dxgi_factory_.Get(); }

    /// Drops every device-dependent object; the next EnsureInitialized() recreates
    /// them (used after DXGI_ERROR_DEVICE_REMOVED).
    void HandleDeviceLost();

    ComPtr<IDWriteTextFormat> CreateTextFormat(float size_dip,
                                               DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                               const wchar_t* family = L"Segoe UI");

private:
    GraphicsEnvironment() = default;

    bool initialized_{false};
    ComPtr<ID3D11Device> d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;
    ComPtr<IDXGIDevice1> dxgi_device_;
    ComPtr<IDXGIFactory2> dxgi_factory_;
    ComPtr<ID2D1Factory1> d2d_factory_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<IDWriteFactory> dwrite_factory_;
    ComPtr<IWICImagingFactory> wic_factory_;
};

/// Direct2D device context bound to an HWND flip-model swap chain.
/// Rendering happens in physical pixels (the target DPI stays at 96) and UI
/// metrics are scaled explicitly per monitor, which keeps selection maths exact.
class SwapChainTarget {
public:
    SwapChainTarget() = default;
    ~SwapChainTarget();

    SwapChainTarget(const SwapChainTarget&) = delete;
    SwapChainTarget& operator=(const SwapChainTarget&) = delete;

    bool Create(HWND hwnd, UINT width, UINT height);
    bool Resize(UINT width, UINT height);
    void Reset();
    /// Presents the back buffer; returns false when the device was lost.
    bool Present();

    [[nodiscard]] ID2D1DeviceContext* context() const noexcept { return context_.Get(); }
    [[nodiscard]] bool valid() const noexcept { return context_ != nullptr; }

private:
    bool BindBackBuffer();

    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID2D1DeviceContext> context_;
    UINT width_{0};
    UINT height_{0};
};

/// Uploads a CPU image into an immutable Direct2D bitmap.
ComPtr<ID2D1Bitmap1> CreateD2DBitmap(ID2D1DeviceContext* context, const BgraImage& image);
/// Same, for the plain render-target interface (WIC/offscreen targets).
ComPtr<ID2D1Bitmap> CreateD2DBitmapForTarget(ID2D1RenderTarget* target, const BgraImage& image);

// ---------------------------------------------------------------------------
// Export: encoding, clipboard, file destinations
// ---------------------------------------------------------------------------

enum class ImageFormat { Png, Jpeg };

[[nodiscard]] ImageFormat FormatFromPath(const std::wstring& path);
bool EncodeImage(const BgraImage& image, ImageFormat format, std::vector<uint8_t>& out_bytes);
bool SaveImage(const BgraImage& image, const std::wstring& path);

/// Publishes the image as CF_DIB (universally understood) plus the registered
/// "PNG" format for applications that prefer lossless data with metadata.
bool CopyImageToClipboard(HWND owner, const BgraImage& image);

/// System save dialog (IFileSaveDialog). Returns std::nullopt when cancelled.
/// Only the picker may decide the destination: no user-provided path fragment
/// ever reaches the file system directly (see docs/threat model notes).
std::optional<std::wstring> ShowSaveImageDialog(HWND owner,
                                                const std::wstring& suggested_file_name,
                                                const std::wstring& initial_directory);

std::optional<std::wstring> KnownFolder(REFKNOWNFOLDERID folder_id);
/// %LOCALAPPDATA%\Easnap, created on demand.
std::wstring AppDataDirectory();
std::wstring DirectoryOf(const std::wstring& path);
std::wstring TimestampedFileName(const wchar_t* prefix, const wchar_t* extension);
bool EnsureDirectory(const std::wstring& path);
/// Deletes a directory strictly below AppDataDirectory(); refuses reparse points.
/// Returns false on incomplete cleanup, leaving unremoved entries intact.
bool RemoveDirectoryTree(const std::wstring& path);

// ---------------------------------------------------------------------------
// Diagnostics (privacy preserving: never log pixels, paths or typed text)
// ---------------------------------------------------------------------------

void LogMessage(const wchar_t* format, ...);
void LogHresult(const wchar_t* what, HRESULT hr);
void ShowErrorMessage(HWND owner, const wchar_t* text);

HICON LoadAppIcon(int desired_size);

}  // namespace easnap::platform
