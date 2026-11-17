// Easnap - the screenshot editor's export: annotations composited onto the image.
#include "editor/editor_export.h"

#include <algorithm>
#include <cstring>

#include "editor/annotation_renderer.h"
#include "platform/graphics_device.h"

namespace easnap::editor {
namespace {

/// Rows of each composite band of a document taller than kWholeBitmapRows.
constexpr int kCompositeBandRows = 2048;

}  // namespace

platform::BgraImage CompositeAnnotations(const platform::BgraImage& base_image, const std::vector<Annotation>& annotations) {
    platform::BgraImage output;
    auto& graphics = platform::GraphicsEnvironment::Get();
    if (!graphics.EnsureInitialized() || base_image.empty()) {
        return output;
    }
    // Once the pixels exist the frame cannot move, so the export is always the
    // whole image: the captured region, or the stitched document.
    if (annotations.empty()) {
        return base_image;  // Nothing to composite.
    }

    // Always the captured resolution - never the window or preview size. A
    // screenshot is one pass; a tall document is composited in bands, each
    // with the same annotation list shifted to its rows.
    const int width = base_image.width;
    const int height = base_image.height;
    const int band_rows = height <= kWholeBitmapRows ? height : kCompositeBandRows;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    const D2D1_BITMAP_PROPERTIES base_properties =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    output.Reset(width, height);
    for (int top = 0; top < height; top += band_rows) {
        const int rows = std::min(band_rows, height - top);
        ComPtr<IWICBitmap> wic_bitmap;
        HRESULT hr = graphics.wic_factory()->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(rows),
                                                          GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wic_bitmap);
        if (FAILED(hr)) {
            platform::LogHresult(L"WIC CreateBitmap(composite)", hr);
            return {};
        }
        ComPtr<ID2D1RenderTarget> render_target;
        hr = graphics.d2d_factory()->CreateWicBitmapRenderTarget(wic_bitmap.Get(), properties, &render_target);
        if (FAILED(hr)) {
            platform::LogHresult(L"CreateWicBitmapRenderTarget", hr);
            return {};
        }

        ComPtr<ID2D1Bitmap> base;
        hr = render_target->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(rows)),
                                         base_image.row(top), static_cast<UINT32>(base_image.stride_bytes()),
                                         base_properties, &base);
        if (FAILED(hr)) {
            platform::LogHresult(L"ID2D1RenderTarget::CreateBitmap", hr);
        }
        ComPtr<ID2D1SolidColorBrush> brush;
        render_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        if (!base || !brush) { return {}; }

        render_target->BeginDraw();
        render_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        render_target->DrawBitmap(base.Get(),
                                  D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(rows)));
        render_target->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -static_cast<float>(top)));
        for (const Annotation& annotation : annotations) {
            DrawAnnotation(render_target.Get(), annotation, brush.Get(), false);
        }
        hr = render_target->EndDraw();
        if (FAILED(hr)) {
            platform::LogHresult(L"Composite EndDraw", hr);
            return {};
        }

        WICRect lock_rect{0, 0, static_cast<INT>(width), static_cast<INT>(rows)};
        ComPtr<IWICBitmapLock> lock;
        hr = wic_bitmap->Lock(&lock_rect, WICBitmapLockRead, &lock);
        if (FAILED(hr)) {
            platform::LogHresult(L"IWICBitmap::Lock", hr);
            return {};
        }
        UINT stride = 0;
        UINT buffer_size = 0;
        BYTE* data = nullptr;
        if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&buffer_size, &data)) || data == nullptr) {
            return {};
        }
        for (int y = 0; y < rows; ++y) {
            std::memcpy(output.row(top + y), data + static_cast<size_t>(y) * stride, output.stride_bytes());
        }
    }
    output.MakeOpaque();
    return output;
}

}  // namespace easnap::editor
