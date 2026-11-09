// Easnap - copying and saving a finished image away from the UI thread.
#include "platform/image_export.h"

#include <limits>
#include <new>
#include <utility>

namespace easnap::platform {
namespace {

std::atomic<int> g_pending_exports{0};

const wchar_t* TargetName(ImageExportTarget target) noexcept {
    return target == ImageExportTarget::File ? L"file" : L"clipboard";
}

/// The worker's whole task. It has a COM apartment and a WIC factory of its
/// own: the shared GraphicsEnvironment belongs to the UI thread, which
/// releases its factory when the graphics device is lost.
ImageExportResult Export(const ImageExportRequest& request, const std::atomic<bool>& cancel) noexcept {
    ImageExportResult result;
    result.target = request.target;
    const ULONGLONG started = ::GetTickCount64();
    const HRESULT apartment = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) {
        LogHresult(L"CoInitializeEx(image export)", apartment);
        return result;
    }
    bool done = false;
    {
        ComPtr<IWICImagingFactory> factory;
        const HRESULT hr =
            ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            LogHresult(L"CoCreateInstance(WICImagingFactory) for an image export", hr);
        } else {
            try {
                if (request.target == ImageExportTarget::File) {
                    done = SaveImage(factory.Get(), *request.image, request.path, &cancel);
                } else {
                    result.clipboard = PrepareClipboardImage(factory.Get(), *request.image, &cancel);
                    done = !result.clipboard.empty();
                }
            } catch (const std::bad_alloc&) {
                LogMessage(L"Not enough memory to export the image");
                done = false;
            }
        }
    }
    ::CoUninitialize();
    const bool cancelled = cancel.load(std::memory_order_acquire);
    if (request.target == ImageExportTarget::File && done) {
        result.status = ImageExportStatus::Succeeded;  // The file is there.
    } else if (cancelled) {
        result.status = ImageExportStatus::Cancelled;
        result.clipboard = {};
    } else {
        result.status = done ? ImageExportStatus::Succeeded : ImageExportStatus::Failed;
    }
    static const wchar_t* const kStatusNames[] = {L"succeeded", L"failed", L"was cancelled"};
    LogMessage(L"Image export to the %s %s after %llu ms (%d x %d)", TargetName(request.target),
               kStatusNames[static_cast<int>(result.status)], ::GetTickCount64() - started, request.image->width,
               request.image->height);
    return result;
}

}  // namespace

ImageExportJob::~ImageExportJob() { Discard(); }

bool ImageExportJob::Start(HWND notify, UINT message, ImageExportRequest request) {
    if (pending_ || ImageExportActive() || notify == nullptr || !request.image || request.image->empty() ||
        (request.target == ImageExportTarget::File && request.path.empty()) ||
        generation_ == (std::numeric_limits<Generation>::max)()) {
        return false;
    }
    Wait();
    const Generation generation = generation_ + 1;
    const ImageExportTarget target = request.target;
    completed_.store(0, std::memory_order_relaxed);
    cancel_.store(false, std::memory_order_relaxed);
    result_ = {};
    try {
        worker_ = std::thread([this, notify, message, generation, request = std::move(request)] {
            result_ = Export(request, cancel_);
            // Release publishes the result. TryTake joins before it reads it.
            completed_.store(generation, std::memory_order_release);
            if (::PostMessageW(notify, message, static_cast<WPARAM>(generation), 0) == FALSE) {
                LogMessage(L"An image export could not report that it finished (error %lu)", ::GetLastError());
            }
        });
    } catch (...) {
        LogMessage(L"Could not start an image export thread");
        return false;
    }
    generation_ = generation;
    target_ = target;
    pending_ = true;
    g_pending_exports.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::optional<ImageExportResult> ImageExportJob::TryTake(Generation generation) {
    if (!pending_ || generation == 0 || generation != generation_ ||
        completed_.load(std::memory_order_acquire) != generation) {
        return std::nullopt;
    }
    Wait();
    std::optional<ImageExportResult> taken(std::move(result_));
    result_ = {};
    Release();
    return taken;
}

void ImageExportJob::Cancel() noexcept { cancel_.store(true, std::memory_order_release); }

void ImageExportJob::Wait() noexcept {
    if (!worker_.joinable()) {
        return;
    }
    try {
        worker_.join();
    } catch (...) {
        LogMessage(L"Could not join an image export thread");
    }
}

void ImageExportJob::SettleForSessionEnd() noexcept {
    if (!pending_) {
        return;
    }
    if (target_ != ImageExportTarget::File) {
        Cancel();
    }
    Wait();
    if (target_ == ImageExportTarget::Clipboard) {
        // A copy that had already been prepared is dropped as well: its
        // completion must not put anything on the clipboard or close a window.
        result_.status = ImageExportStatus::Cancelled;
        result_.clipboard = {};
    }
}

void ImageExportJob::Discard() noexcept {
    Cancel();
    Wait();
    if (pending_) {
        result_ = {};
        Release();
    }
}

void ImageExportJob::Release() noexcept {
    pending_ = false;
    g_pending_exports.fetch_sub(1, std::memory_order_relaxed);
}

bool ImageExportActive() noexcept { return g_pending_exports.load(std::memory_order_relaxed) != 0; }

}  // namespace easnap::platform
