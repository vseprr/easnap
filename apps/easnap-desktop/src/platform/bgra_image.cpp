// Easnap - CPU-side image container implementation.
#include "platform/bgra_image.h"

#include <algorithm>
#include <cstring>

#include "platform/screen_geometry.h"

namespace easnap::platform {

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
// BgraImage
// ---------------------------------------------------------------------------

void BgraImage::Reset(int w, int h) {
    width = std::max(0, w);
    height = std::max(0, h);
    pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xFF000000u);
}

void BgraImage::ResizeForOverwrite(int w, int h) {
    width = std::max(0, w);
    height = std::max(0, h);
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (count > pixels.capacity()) {
        // Growing past the capacity would copy the old pixels into the new
        // storage, only for the caller to overwrite them.
        pixels.clear();
    }
    pixels.resize(count);
}

namespace {

void CopyRowOpaque(uint32_t* __restrict target, const uint32_t* __restrict source,
                   size_t count) noexcept {
    for (size_t x = 0; x < count; ++x) {
        target[x] = source[x] | 0xFF000000u;
    }
}

}  // namespace

void BgraImage::CopyOpaqueFrom(int w, int h, const void* source, size_t source_pitch) {
    ResizeForOverwrite(w, h);
    const auto* bytes = static_cast<const uint8_t*>(source);
    for (int y = 0; y < height; ++y) {
        // 32bpp rows from Direct3D and GDI start on four-byte boundaries.
        const void* source_row = bytes + static_cast<size_t>(y) * source_pitch;
        CopyRowOpaque(row(y), static_cast<const uint32_t*>(source_row), static_cast<size_t>(width));
    }
}

void BgraImage::MakeOpaque() noexcept {
    for (uint32_t& pixel : pixels) {
        pixel |= 0xFF000000u;
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

}  // namespace easnap::platform
