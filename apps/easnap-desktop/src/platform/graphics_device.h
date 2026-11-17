// Easnap - the shared Direct3D/Direct2D/DirectWrite/WIC environment and the
// swap-chain targets windows draw into.
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
#include <wincodec.h>

#include "platform/bgra_image.h"
#include "platform/platform_utils.h"

namespace easnap::platform {

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

}  // namespace easnap::platform
