// Easnap - reusable D3D11/DXGI device-manager capability for Media Foundation.
#pragma once

#include <d3d11.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace easnap::media {

class MfD3dDevice final {
public:
    static std::unique_ptr<MfD3dDevice> Create();

    [[nodiscard]] IMFDXGIDeviceManager* manager() const noexcept {
        return manager_.Get();
    }
    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] const std::wstring& adapter_name() const noexcept {
        return adapter_name_;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    UINT reset_token_{0};
    std::wstring adapter_name_;
};

}  // namespace easnap::media
