// Easnap - synchronous image output implementation.
#include "platform/image_output.h"

#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "platform/files.h"
#include "platform/graphics_device.h"
#include "platform/platform_utils.h"

namespace easnap::platform {

namespace {

/// Rows written per call: a cancelled export stops between two bands.
constexpr int kEncodeBandRows = 256;

bool Cancelled(const std::atomic<bool>* cancel) noexcept {
    return cancel != nullptr && cancel->load(std::memory_order_acquire);
}

bool WriteEncodedImage(IWICImagingFactory* wic, const BgraImage& image, ImageFormat format, IStream* stream,
                       const std::atomic<bool>* cancel) {
    if (wic == nullptr || stream == nullptr || image.empty()) {
        return false;
    }

    const GUID container = (format == ImageFormat::Jpeg) ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
    ComPtr<IWICBitmapEncoder> encoder;
    HRESULT hr = wic->CreateEncoder(container, nullptr, &encoder);
    if (FAILED(hr)) {
        LogHresult(L"WIC CreateEncoder", hr);
        return false;
    }
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        LogHresult(L"WIC encoder Initialize", hr);
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    hr = encoder->CreateNewFrame(&frame, &options);
    if (FAILED(hr)) {
        LogHresult(L"WIC CreateNewFrame", hr);
        return false;
    }
    hr = frame->Initialize(options.Get());
    if (FAILED(hr)) {
        return false;
    }
    const UINT width = static_cast<UINT>(image.width);
    hr = frame->SetSize(width, static_cast<UINT>(image.height));
    if (FAILED(hr)) {
        return false;
    }

    // JPEG has no alpha channel; BGR is the closest native match.
    WICPixelFormatGUID pixel_format =
        (format == ImageFormat::Jpeg) ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&pixel_format);
    if (FAILED(hr)) {
        return false;
    }

    // Successive writes add rows to the frame, so the image goes in bands: a
    // cancelled export stops after the band being encoded, and a converted
    // format never needs a second copy of the whole image.
    const bool direct = ::IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA) != FALSE;
    const UINT stride = static_cast<UINT>(image.stride_bytes());
    for (int top = 0; top < image.height && SUCCEEDED(hr); top += kEncodeBandRows) {
        if (Cancelled(cancel)) {
            return false;
        }
        const UINT rows = static_cast<UINT>((std::min)(kEncodeBandRows, image.height - top));
        auto* band = reinterpret_cast<BYTE*>(const_cast<uint32_t*>(image.row(top)));
        if (direct) {
            hr = frame->WritePixels(rows, stride, stride * rows, band);
            continue;
        }
        // Convert to the format the encoder negotiated.
        ComPtr<IWICBitmap> source;
        hr = wic->CreateBitmapFromMemory(width, rows, GUID_WICPixelFormat32bppBGRA, stride, stride * rows, band,
                                         &source);
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(hr)) { hr = wic->CreateFormatConverter(&converter); }
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(source.Get(), pixel_format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        }
        WICRect rect{0, 0, static_cast<INT>(width), static_cast<INT>(rows)};
        if (SUCCEEDED(hr)) { hr = frame->WriteSource(converter.Get(), &rect); }
    }

    if (FAILED(hr)) {
        LogHresult(L"WIC WritePixels", hr);
        return false;
    }
    if (Cancelled(cancel)) {
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        LogHresult(L"WIC encoder Commit", hr);
        return false;
    }
    return true;
}

/// The image as clipboard memory in CF_DIB, or null. The caller owns it until
/// SetClipboardData accepts it.
HGLOBAL MakeClipboardDib(const BgraImage& image, const std::atomic<bool>* cancel) {
    // 32bpp BI_RGB DIB: the format virtually every Windows application accepts.
    const size_t pixel_bytes = image.pixels.size() * sizeof(uint32_t);
    const size_t total = sizeof(BITMAPINFOHEADER) + pixel_bytes;
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, total);
    if (memory == nullptr) {
        return nullptr;
    }
    auto* header = static_cast<BITMAPINFOHEADER*>(::GlobalLock(memory));
    if (header == nullptr) {
        ::GlobalFree(memory);
        return nullptr;
    }
    *header = BITMAPINFOHEADER{};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = image.width;
    // Bottom-up. A top-down DIB is a valid bitmap, but not every reader of the
    // clipboard accepts one: .NET's Clipboard.GetImage returns nothing for it.
    header->biHeight = image.height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixel_bytes);
    auto* const rows = reinterpret_cast<uint32_t*>(header + 1);
    const size_t row_bytes = static_cast<size_t>(image.width) * sizeof(uint32_t);
    for (int y = 0; y < image.height; ++y) {
        if (y % kEncodeBandRows == 0 && Cancelled(cancel)) {
            ::GlobalUnlock(memory);
            ::GlobalFree(memory);
            return nullptr;
        }
        std::memcpy(rows + static_cast<size_t>(image.height - 1 - y) * image.width, image.row(y), row_bytes);
    }
    ::GlobalUnlock(memory);
    return memory;
}

/// The image encoded as PNG in clipboard memory, or null. The encoder writes
/// straight into the memory the clipboard receives.
HGLOBAL MakeClipboardPng(IWICImagingFactory* factory, const BgraImage& image, const std::atomic<bool>* cancel) {
    ComPtr<IStream> stream;
    const HRESULT hr = ::CreateStreamOnHGlobal(nullptr, FALSE, &stream);
    if (FAILED(hr)) {
        LogHresult(L"CreateStreamOnHGlobal", hr);
        return nullptr;
    }
    bool encoded = WriteEncodedImage(factory, image, ImageFormat::Png, stream.Get(), cancel);
    if (encoded) {
        // The memory can be larger than what was written, and readers of the
        // clipboard take its size for the size of the PNG.
        STATSTG stat{};
        encoded = SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart != 0 &&
                  SUCCEEDED(stream->SetSize(stat.cbSize));
    }
    // The stream does not free its memory; it is handed out or freed here.
    HGLOBAL memory = nullptr;
    if (FAILED(::GetHGlobalFromStream(stream.Get(), &memory))) {
        memory = nullptr;
    }
    stream.Reset();
    if (!encoded && memory != nullptr) {
        ::GlobalFree(memory);
        memory = nullptr;
    }
    return memory;
}

/// Hands `memory` to the open clipboard, or frees it when the clipboard
/// refuses it.
bool PutClipboardMemory(UINT format, HGLOBAL memory) {
    if (memory == nullptr) {
        return false;
    }
    if (format == 0 || ::SetClipboardData(format, memory) == nullptr) {
        ::GlobalFree(memory);
        return false;
    }
    return true;  // Ownership transferred to the clipboard.
}

}  // namespace

// ---------------------------------------------------------------------------
// Export helpers
// ---------------------------------------------------------------------------

ImageFormat FormatFromPath(const std::wstring& path) {
    const wchar_t* extension = ::PathFindExtensionW(path.c_str());
    if (extension != nullptr &&
        (::StrCmpIW(extension, L".jpg") == 0 || ::StrCmpIW(extension, L".jpeg") == 0)) {
        return ImageFormat::Jpeg;
    }
    return ImageFormat::Png;
}

bool EncodeImage(const BgraImage& image, ImageFormat format, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    if (!GraphicsEnvironment::Get().EnsureInitialized()) {
        return false;
    }
    ComPtr<IStream> stream;
    HRESULT hr = ::CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) {
        LogHresult(L"CreateStreamOnHGlobal", hr);
        return false;
    }
    if (!WriteEncodedImage(GraphicsEnvironment::Get().wic_factory(), image, format, stream.Get(), nullptr)) {
        return false;
    }

    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.HighPart != 0 || stat.cbSize.LowPart == 0) {
        return false;
    }
    out_bytes.resize(stat.cbSize.LowPart);
    LARGE_INTEGER zero{};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) {
        return false;
    }
    ULONG read = 0;
    hr = stream->Read(out_bytes.data(), static_cast<ULONG>(out_bytes.size()), &read);
    if (FAILED(hr) || read != out_bytes.size()) {
        out_bytes.clear();
        return false;
    }
    return true;
}

bool SaveImage(const BgraImage& image, const std::wstring& path) {
    if (!GraphicsEnvironment::Get().EnsureInitialized()) {
        return false;
    }
    return SaveImage(GraphicsEnvironment::Get().wic_factory(), image, path, nullptr);
}

bool SaveImage(IWICImagingFactory* factory, const BgraImage& image, const std::wstring& path,
               const std::atomic<bool>* cancel) {
    if (factory == nullptr || path.empty()) {
        return false;
    }
    // The destination always comes from the system picker, so the path is
    // already canonicalised by the shell; it is only opened, never composed.
    // Encode beside the destination and swap the finished file in. A failed
    // export then leaves an existing file at this path exactly as it was.
    wchar_t suffix[48]{};
    ::swprintf_s(suffix, L".easnap-%08lx%016llx.tmp", ::GetCurrentProcessId(), ::GetTickCount64());
    const std::wstring staged = path + suffix;
    ComPtr<IStream> stream;
    const HRESULT hr = ::SHCreateStreamOnFileEx(staged.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                                FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
    if (FAILED(hr)) {
        LogHresult(L"SHCreateStreamOnFileEx", hr);
        return false;
    }
    const bool encoded = WriteEncodedImage(factory, image, FormatFromPath(path), stream.Get(), cancel) &&
                         SUCCEEDED(stream->Commit(STGC_DEFAULT));
    stream.Reset();
    if (!encoded || Cancelled(cancel) ||
        ::MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        if (encoded && !Cancelled(cancel)) {
            LogMessage(L"SaveImage could not replace the destination: %lu", ::GetLastError());
        }
        ::DeleteFileW(staged.c_str());  // Never leave a partial export behind.
        return false;
    }
    return true;
}

ClipboardImage::~ClipboardImage() { Reset(); }

ClipboardImage::ClipboardImage(ClipboardImage&& other) noexcept : dib_(other.dib_), png_(other.png_) {
    other.dib_ = nullptr;
    other.png_ = nullptr;
}

ClipboardImage& ClipboardImage::operator=(ClipboardImage&& other) noexcept {
    if (this != &other) {
        Reset();
        dib_ = other.dib_;
        png_ = other.png_;
        other.dib_ = nullptr;
        other.png_ = nullptr;
    }
    return *this;
}

void ClipboardImage::Reset() noexcept {
    if (dib_ != nullptr) { ::GlobalFree(dib_); }
    if (png_ != nullptr) { ::GlobalFree(png_); }
    dib_ = nullptr;
    png_ = nullptr;
}

ClipboardImage PrepareClipboardImage(IWICImagingFactory* factory, const BgraImage& image,
                                     const std::atomic<bool>* cancel) {
    ClipboardImage prepared;
    if (image.empty()) {
        return prepared;
    }
    // Both formats are ready before the clipboard opens: other applications
    // cannot use the clipboard while it is open, and a tall PNG takes seconds.
    prepared.dib_ = MakeClipboardDib(image, cancel);
    if (prepared.dib_ == nullptr) {
        if (!Cancelled(cancel)) { LogMessage(L"Clipboard image memory could not be allocated"); }
        return prepared;
    }
    prepared.png_ = MakeClipboardPng(factory, image, cancel);
    if (Cancelled(cancel)) {
        prepared.Reset();
    }
    return prepared;
}

bool PublishClipboardImage(HWND owner, ClipboardImage& image) {
    if (image.empty()) {
        return false;
    }
    if (::OpenClipboard(owner) == FALSE) {
        LogMessage(L"OpenClipboard failed (error %lu)", ::GetLastError());
        return false;
    }
    bool ok = false;
    if (::EmptyClipboard() != FALSE) {
        static const UINT png_format = ::RegisterClipboardFormatW(L"PNG");
        HGLOBAL dib = image.dib_;
        HGLOBAL png = image.png_;
        image.dib_ = nullptr;
        image.png_ = nullptr;
        ok = PutClipboardMemory(CF_DIB, dib);
        if (png != nullptr) { static_cast<void>(PutClipboardMemory(png_format, png)); }
    }
    ::CloseClipboard();
    return ok;
}

std::optional<std::wstring> ShowSaveImageDialog(HWND owner, const std::wstring& suggested_file_name,
                                                const std::wstring& initial_directory,
                                                const SaveImageDialogText& text) {
    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = ::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) {
        LogHresult(L"CoCreateInstance(FileSaveDialog)", hr);
        return std::nullopt;
    }

    // Built for each dialog: the type names follow the active language. The
    // dialog copies the filters.
    const COMDLG_FILTERSPEC filters[] = {
        {text.png_type, L"*.png"},
        {text.jpeg_type, L"*.jpg;*.jpeg"},
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"png");
    dialog->SetTitle(text.title);
    dialog->SetFileName(suggested_file_name.c_str());

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM |
                           FOS_NOREADONLYRETURN);
    }

    if (!initial_directory.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(initial_directory.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            // SetFolder, not SetDefaultFolder: the dialog would otherwise prefer
            // the folder it was last closed in over the one asked for.
            dialog->SetFolder(folder.Get());
        }
    } else if (auto pictures = KnownFolder(FOLDERID_Pictures)) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(pictures->c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetDefaultFolder(folder.Get());
        }
    }

    {
        const ModalLoopScope modal;
        hr = dialog->Show(owner);
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::nullopt;
    }
    if (FAILED(hr)) {
        LogHresult(L"IFileSaveDialog::Show", hr);
        return std::nullopt;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return std::nullopt;
    }
    PWSTR raw_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || raw_path == nullptr) {
        return std::nullopt;
    }
    std::wstring result(raw_path);
    ::CoTaskMemFree(raw_path);
    return result;
}

}  // namespace easnap::platform
