// Easnap tests - a folder of the test's own for files an export writes, and
// reading such a file back.
#pragma once

#include "platform/platform_utils.h"

#include <string>
#include <vector>

namespace easnap::test {

/// A new folder under %TEMP%, removed with the files in it. Tests never
/// write into the user's own folders.
class TempFolder {
public:
    explicit TempFolder(const wchar_t* prefix) {
        wchar_t base[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, base);
        wchar_t name[128]{};
        ::swprintf_s(name, L"%s-%lu-%llu", prefix, ::GetCurrentProcessId(), ::GetTickCount64());
        path_ = std::wstring(base) + name;
        ok_ = ::CreateDirectoryW(path_.c_str(), nullptr) != FALSE;
    }
    ~TempFolder() {
        for (const std::wstring& file : Files(L"*")) {
            const std::wstring full = path_ + L"\\" + file;
            if (::RemoveDirectoryW(full.c_str()) == FALSE) { ::DeleteFileW(full.c_str()); }
        }
        ::RemoveDirectoryW(path_.c_str());
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] const std::wstring& path() const { return path_; }
    [[nodiscard]] std::wstring File(const wchar_t* name) const { return path_ + L"\\" + name; }
    /// Names in the folder matching `pattern`, without "." and "..".
    [[nodiscard]] std::vector<std::wstring> Files(const wchar_t* pattern) const {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW((path_ + L"\\" + pattern).c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) { return names; }
        do {
            const std::wstring name = data.cFileName;
            if (name != L"." && name != L"..") { names.push_back(name); }
        } while (::FindNextFileW(find, &data) != FALSE);
        ::FindClose(find);
        return names;
    }
    /// Files a save stages beside its destination; none may stay.
    [[nodiscard]] size_t StagedFiles() const { return Files(L"*.easnap-*.tmp").size(); }
    /// What has been written into staged files so far.
    [[nodiscard]] unsigned long long StagedBytes() const {
        unsigned long long bytes = 0;
        for (const std::wstring& name : Files(L"*.easnap-*.tmp")) {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (::GetFileAttributesExW(File(name.c_str()).c_str(), GetFileExInfoStandard, &data) != FALSE) {
                bytes += (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            }
        }
        return bytes;
    }

private:
    std::wstring path_;
    bool ok_{false};
};

/// The picture in `path`, read back through WIC as 32bpp BGRA. COM must be
/// initialised on the calling thread.
inline bool DecodeImageFile(const std::wstring& path, platform::BgraImage& out) {
    platform::ComPtr<IWICImagingFactory> factory;
    platform::ComPtr<IWICBitmapDecoder> decoder;
    platform::ComPtr<IWICBitmapFrameDecode> frame;
    platform::ComPtr<IWICFormatConverter> converter;
    UINT width = 0, height = 0;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    out.Reset(static_cast<int>(width), static_cast<int>(height));
    return SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(out.stride_bytes()),
                                           static_cast<UINT>(out.pixels.size() * sizeof(uint32_t)),
                                           reinterpret_cast<BYTE*>(out.pixels.data())));
}

}  // namespace easnap::test
