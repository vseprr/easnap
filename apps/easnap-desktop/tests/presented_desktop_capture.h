#pragma once
#include <cstring>
#include <memory>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include "platform/platform_utils.h"

namespace easnap::test {
// Independent observer of the compositor's monitor output, not a child HWND's
// backing DC. Used only by native presentation tests on a test-owned backdrop.
class PresentedDesktopCapture {
public:
    explicit PresentedDesktopCapture(const RECT& region) {
        using namespace winrt::Windows::Graphics;
        const HMONITOR monitor=::MonitorFromRect(&region,MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};winrt::check_bool(::GetMonitorInfoW(monitor,&info));
        monitor_=info.rcMonitor;
        region_=region;
        winrt::check_bool(region.left>=monitor_.left && region.top>=monitor_.top &&
            region.right<=monitor_.right && region.bottom<=monitor_.bottom &&
            region.right>region.left && region.bottom>region.top);
        wchar_t capture_mode[32]{};
        ::GetEnvironmentVariableW(L"EASNAP_TEST_CAPTURE",capture_mode,32);
        if(::lstrcmpW(capture_mode,L"duplication")==0) {
            winrt::com_ptr<IDXGIFactory1> factory;
            winrt::check_hresult(::CreateDXGIFactory1(__uuidof(IDXGIFactory1),factory.put_void()));
            for(UINT adapter_index=0;;++adapter_index) {
                winrt::com_ptr<IDXGIAdapter1> adapter;
                const HRESULT next_adapter=factory->EnumAdapters1(adapter_index,adapter.put());
                if(next_adapter==DXGI_ERROR_NOT_FOUND) break;
                winrt::check_hresult(next_adapter);
                for(UINT output_index=0;;++output_index) {
                    winrt::com_ptr<IDXGIOutput> output;
                    const HRESULT next_output=adapter->EnumOutputs(output_index,output.put());
                    if(next_output==DXGI_ERROR_NOT_FOUND) break;
                    winrt::check_hresult(next_output);
                    DXGI_OUTPUT_DESC desc{};winrt::check_hresult(output->GetDesc(&desc));
                    if(desc.Monitor!=monitor) continue;
                    winrt::check_hresult(::D3D11CreateDevice(adapter.get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device_.put(),nullptr,context_.put()));
                    winrt::check_hresult(output.as<IDXGIOutput1>()->DuplicateOutput(device_.get(),duplication_.put()));
                    return;
                }
            }
            winrt::throw_hresult(DXGI_ERROR_NOT_FOUND);
        }
        winrt::check_hresult(::D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device_.put(),nullptr,context_.put()));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(device_.as<IDXGIDevice>().get(),inspectable.put()));
        auto runtime_device=inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        auto factory=winrt::get_activation_factory<Capture::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        Capture::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(factory->CreateForMonitor(monitor,winrt::guid_of<Capture::GraphicsCaptureItem>(),winrt::put_abi(item)));
        pool_=Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(runtime_device,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,item.Size());
        session_=pool_.CreateCaptureSession(item);
        session_.IsCursorCaptureEnabled(false);
        session_.StartCapture();
    }
    std::shared_ptr<const platform::BgraImage> CaptureNext() {
        if(duplication_) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            winrt::com_ptr<IDXGIResource> resource;
            const HRESULT result=duplication_->AcquireNextFrame(0,&info,resource.put());
            if(result==DXGI_ERROR_WAIT_TIMEOUT) return {};
            winrt::check_hresult(result);
            std::shared_ptr<const platform::BgraImage> image;
            try {
                // A pointer-only update is not a newly presented desktop image.
                image=info.LastPresentTime.QuadPart ? CopyTexture(resource.as<ID3D11Texture2D>().get()) : nullptr;
            } catch(...) { duplication_->ReleaseFrame();throw; }
            winrt::check_hresult(duplication_->ReleaseFrame());
            return image;
        }
        const auto frame=pool_.TryGetNextFrame();
        if(!frame) { return {}; }
        auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),source.put_void()));
        const auto image=CopyTexture(source.get());
        frame.Close();
        return image;
    }
    ~PresentedDesktopCapture() {
        // Destruction must also be safe after capture/device failure.
        try { if(session_) session_.Close(); } catch(const winrt::hresult_error&) {}
        try { if(pool_) pool_.Close(); } catch(const winrt::hresult_error&) {}
    }
private:
    std::shared_ptr<const platform::BgraImage> CopyTexture(ID3D11Texture2D* source) {
        const UINT width=static_cast<UINT>(region_.right-region_.left),height=static_cast<UINT>(region_.bottom-region_.top);
        if(!staging_) {
            D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
            desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            winrt::check_hresult(device_->CreateTexture2D(&desc,nullptr,staging_.put()));
        }
        const D3D11_BOX box{static_cast<UINT>(region_.left-monitor_.left),static_cast<UINT>(region_.top-monitor_.top),0,
            static_cast<UINT>(region_.right-monitor_.left),static_cast<UINT>(region_.bottom-monitor_.top),1};
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        // A display mode change invalidates this test's physical monitor region.
        winrt::check_bool(box.right<=source_desc.Width && box.bottom<=source_desc.Height);
        context_->CopySubresourceRegion(staging_.get(),0,0,0,0,source,0,&box);
        auto image=std::make_shared<platform::BgraImage>();
        image->width=static_cast<int>(width);image->height=static_cast<int>(height);
        image->pixels.resize(static_cast<size_t>(width)*height);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context_->Map(staging_.get(),0,D3D11_MAP_READ,0,&mapped));
        for(UINT y=0;y<height;++y) std::memcpy(image->row(static_cast<int>(y)),
            static_cast<const uint8_t*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,static_cast<size_t>(width)*4);
        context_->Unmap(staging_.get(),0);
        return image;
    }
    RECT monitor_{},region_{};
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> staging_;
    winrt::com_ptr<IDXGIOutputDuplication> duplication_;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
};
}
