// Easnap - graphics environment implementation.
#include "platform/graphics_device.h"

#include <array>

namespace easnap::platform {

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
    // The text drawn with this format is what the user typed. Its language is
    // the user's, not the interface's, so locale-dependent glyph forms and
    // font fallback follow the Windows regional format.
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (::GetUserDefaultLocaleName(locale, ARRAYSIZE(locale)) == 0) {
        ::wcscpy_s(locale, L"en-us");
    }
    const HRESULT hr = dwrite_factory_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                         DWRITE_FONT_STRETCH_NORMAL, size_dip, locale, &format);
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

}  // namespace easnap::platform
