// Easnap - synchronous image output: WIC encoding, saving to a file, the
// clipboard and the save dialog. platform/image_export runs the encoding and
// the clipboard preparation on a worker thread.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <wincodec.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "platform/bgra_image.h"

namespace easnap::platform {

/// File format an image is encoded in.
enum class ImageFormat { Png,
                         Jpeg };

/// Jpeg for a .jpg or .jpeg path, Png for any other.
[[nodiscard]] ImageFormat FormatFromPath(const std::wstring& path);
/// UI-thread forms: they use the shared GraphicsEnvironment.
bool EncodeImage(const BgraImage& image, ImageFormat format, std::vector<uint8_t>& out_bytes);
bool SaveImage(const BgraImage& image, const std::wstring& path);
/// Worker-thread form of SaveImage. `factory` belongs to the calling thread:
/// the shared GraphicsEnvironment is the UI thread's and may be released
/// under a worker. The image is written in bands of rows; once `cancel` is
/// set, the export stops at the next band and leaves no file behind.
bool SaveImage(IWICImagingFactory* factory, const BgraImage& image, const std::wstring& path,
               const std::atomic<bool>* cancel);

/// An image ready for the clipboard: CF_DIB, which virtually every Windows
/// application understands, and, when it could be encoded, the registered
/// "PNG" format for applications that prefer lossless data. Prepared away
/// from the UI thread; the memory is freed unless the clipboard takes it.
class ClipboardImage final {
public:
    ClipboardImage() noexcept = default;
    ~ClipboardImage();
    ClipboardImage(ClipboardImage&& other) noexcept;
    ClipboardImage& operator=(ClipboardImage&& other) noexcept;
    ClipboardImage(const ClipboardImage&) = delete;
    ClipboardImage& operator=(const ClipboardImage&) = delete;

    [[nodiscard]] bool empty() const noexcept { return dib_ == nullptr; }
    [[nodiscard]] bool has_png() const noexcept { return png_ != nullptr; }

private:
    friend ClipboardImage PrepareClipboardImage(IWICImagingFactory* factory, const BgraImage& image,
                                                const std::atomic<bool>* cancel);
    friend bool PublishClipboardImage(HWND owner, ClipboardImage& image);
    void Reset() noexcept;

    HGLOBAL dib_{nullptr};
    HGLOBAL png_{nullptr};
};

/// Builds the clipboard memory on the calling thread, with that thread's
/// `factory`. Empty when the CF_DIB could not be made or `cancel` was set;
/// the PNG is optional, as a very tall image can fail to encode.
[[nodiscard]] ClipboardImage PrepareClipboardImage(IWICImagingFactory* factory, const BgraImage& image,
                                                   const std::atomic<bool>* cancel);
/// Puts a prepared image on the clipboard, on the UI thread, in one attempt:
/// the clipboard is open only for the hand-over. The memory goes to the
/// clipboard or is freed with `image`. True when the CF_DIB was taken.
bool PublishClipboardImage(HWND owner, ClipboardImage& image);

/// System save dialog (IFileSaveDialog). Returns std::nullopt when cancelled.
/// Only the picker may decide the destination: no user-provided path fragment
/// ever reaches the file system directly (see SECURITY.md).
/// The dialog's text comes from the caller: this layer is also built into the
/// preview helper and into tests that carry no string tables.
struct SaveImageDialogText {
    const wchar_t* title{L"Save screenshot"};
    const wchar_t* png_type{L"PNG image"};
    const wchar_t* jpeg_type{L"JPEG image"};
};
std::optional<std::wstring> ShowSaveImageDialog(HWND owner,
                                                const std::wstring& suggested_file_name,
                                                const std::wstring& initial_directory,
                                                const SaveImageDialogText& text = {});

}  // namespace easnap::platform
