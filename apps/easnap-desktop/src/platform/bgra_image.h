// Easnap - CPU-side image container (32bpp BGRA, top-down, no padding).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace easnap::platform {

/// A picture in memory: 32bpp BGRA, top-down rows, no padding.
struct BgraImage {
    int width{0};
    int height{0};
    std::vector<uint32_t> pixels;

    /// Gives the picture the size `w` x `h`, every pixel opaque black.
    void Reset(int w, int h);
    /// Gives the picture the size `w` x `h` without filling it, for a caller
    /// that writes every pixel next. Pixels already there keep their old
    /// values and added ones are zero, so the picture means nothing until the
    /// caller has written all of it. A picture reused at its size is neither
    /// filled nor reallocated (ADR-042).
    void ResizeForOverwrite(int w, int h);
    /// Gives the picture the size `w` x `h` and copies every pixel from
    /// `source`, 32bpp rows `source_pitch` bytes apart, made fully opaque in
    /// the same pass: the picture `Reset`, a row copy and `MakeOpaque` give,
    /// with one write per pixel instead of three.
    void CopyOpaqueFrom(int w, int h, const void* source, size_t source_pitch);
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

/// Copies `source_region` (image coordinates) out of `source` into `out`.
bool CropImage(const BgraImage& source, const RECT& source_region, BgraImage& out);

}  // namespace easnap::platform
