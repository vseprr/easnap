#include "media/mf_d3d_device.h"

#include <d3d10.h>

#include <new>

#include "platform/platform_utils.h"

namespace easnap::media {

std::unique_ptr<MfD3dDevice> MfD3dDevice::Create() {
    auto capability = std::unique_ptr<MfD3dDevice>(new (std::nothrow) MfD3dDevice{});
    if (!capability) {
        return nullptr;
    }

    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,
                                         D3D_FEATURE_LEVEL_11_0,
                                         D3D_FEATURE_LEVEL_10_1,
                                         D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &capability->device_,
        &selected, &capability->context_);
    if (result == E_INVALIDARG) {
        result = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION,
            &capability->device_, &selected, &capability->context_);
    }
    if (FAILED(result)) {
        platform::LogHresult(L"[Trim] Creating the reusable D3D11 video device",
                             result);
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_description{};
    if (SUCCEEDED(capability->device_.As(&dxgi_device)) &&
        SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&adapter_description))) {
        capability->adapter_name_ = adapter_description.Description;
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(capability->device_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    result = ::MFCreateDXGIDeviceManager(&capability->reset_token_,
                                         &capability->manager_);
    if (SUCCEEDED(result)) {
        result = capability->manager_->ResetDevice(capability->device_.Get(),
                                                   capability->reset_token_);
    }
    if (FAILED(result)) {
        platform::LogHresult(
            L"[Trim] Creating the Media Foundation DXGI device manager", result);
        return nullptr;
    }
    platform::LogMessage(L"[Trim] D3D11 media device ready; adapter=%s",
                         capability->adapter_name_.empty()
                             ? L"unreported"
                             : capability->adapter_name_.c_str());
    return capability;
}

}  // namespace easnap::media
