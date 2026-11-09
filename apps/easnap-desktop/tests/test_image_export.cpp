// Easnap unit tests - copying and saving a finished image on a worker.
//
// The production ImageExportJob reports to a message-only window. Files are
// written into a folder the test creates and removes; clipboard memory is
// prepared but never published, so the user's clipboard is not touched.

#include "platform/image_export.h"
#include "temp_folder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace easnap;
using platform::ImageExportJob;
using platform::ImageExportRequest;
using platform::ImageExportStatus;
using platform::ImageExportTarget;
using test::TempFolder;

int passed{}, failed{};
#define CHECK(...)                                                         \
    do {                                                                   \
        if (__VA_ARGS__) {                                                 \
            ++passed;                                                      \
        } else {                                                           \
            ++failed;                                                      \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); \
        }                                                                  \
    } while (false)

using Clock = std::chrono::steady_clock;

long long MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

/// Opaque pixels without a pattern the PNG compressor could exploit.
std::shared_ptr<const platform::BgraImage> Noise(int width, int height, uint32_t seed) {
    auto image = std::make_shared<platform::BgraImage>();
    image->Reset(width, height);
    uint32_t state = seed;
    for (uint32_t& pixel : image->pixels) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        pixel = 0xff000000u | (state & 0x00ffffffu);
    }
    return image;
}

/// A smooth picture: JPEG keeps it close to the original.
std::shared_ptr<const platform::BgraImage> Gradient(int width, int height) {
    auto image = std::make_shared<platform::BgraImage>();
    image->Reset(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image->row(y)[x] = 0xff000000u | (static_cast<uint32_t>(x * 255 / width) << 16) |
                               (static_cast<uint32_t>(y * 255 / height) << 8) | 0x60u;
        }
    }
    return image;
}

/// Pumps the thread's messages until `window` receives the completion, or
/// the deadline passes. Returns its WPARAM, or 0.
WPARAM WaitForCompletion(HWND window, DWORD timeout_ms) {
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    for (;;) {
        MSG message{};
        if (::PeekMessageW(&message, window, platform::kMsgImageExportCompleted, platform::kMsgImageExportCompleted,
                           PM_REMOVE) != FALSE) {
            return message.wParam;
        }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) { return 0; }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(deadline - now), QS_POSTMESSAGE);
    }
}

/// Waits, with a deadline, until `condition` holds; between checks the
/// thread waits for messages for a few milliseconds at most.
template <typename Condition>
bool WaitUntil(Condition condition, DWORD timeout_ms) {
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    while (!condition()) {
        if (::GetTickCount64() >= deadline) { return false; }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
    return true;
}

/// A file save reports its generation and decodes back to the same pixels,
/// in more than one band of rows. One export runs at a time.
void SaveRoundTrip(HWND window, const TempFolder& folder) {
    ImageExportJob job;
    CHECK(!job.running() && !platform::ImageExportActive());
    const auto image = Noise(300, 600, 0x1234567u);
    const std::wstring path = folder.File(L"round trip.png");
    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::File, path}));
    CHECK(job.running() && platform::ImageExportActive());
    // A second export, of this job or of another, waits for the first.
    ImageExportJob other;
    CHECK(!other.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::Clipboard, {}}));
    CHECK(!job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::File, path}));
    const WPARAM generation = WaitForCompletion(window, 20000);
    CHECK(generation != 0 && generation == job.generation());
    CHECK(!job.TryTake(generation + 1).has_value());
    auto result = job.TryTake(generation);
    CHECK(result.has_value() && result->status == ImageExportStatus::Succeeded &&
          result->target == ImageExportTarget::File);
    CHECK(!job.TryTake(generation).has_value());  // Exactly once.
    CHECK(!job.running() && !platform::ImageExportActive());
    platform::BgraImage decoded;
    CHECK(test::DecodeImageFile(path, decoded));
    CHECK(decoded.width == 300 && decoded.height == 600 && decoded.pixels == image->pixels);
    CHECK(folder.StagedFiles() == 0);

    // JPEG goes through the per-band format conversion.
    const auto smooth = Gradient(257, 700);
    const std::wstring jpeg = folder.File(L"round trip.jpg");
    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {smooth, ImageExportTarget::File, jpeg}));
    const WPARAM second = WaitForCompletion(window, 20000);
    CHECK(second == job.generation() && second != generation);
    result = job.TryTake(second);
    CHECK(result.has_value() && result->status == ImageExportStatus::Succeeded);
    CHECK(test::DecodeImageFile(jpeg, decoded) && decoded.width == 257 && decoded.height == 700);
    if (decoded.width == 257 && decoded.height == 700) {
        long long difference = 0;
        for (size_t i = 0; i < decoded.pixels.size(); ++i) {
            for (int shift : {0, 8, 16}) {
                difference += std::abs(static_cast<int>((decoded.pixels[i] >> shift) & 0xff) -
                                       static_cast<int>((smooth->pixels[i] >> shift) & 0xff));
            }
        }
        const double mean = static_cast<double>(difference) / static_cast<double>(decoded.pixels.size() * 3);
        CHECK(mean < 4.0);
    }
}

/// A file that cannot be written fails, and nothing staged is left behind.
void FailedSaveLeavesNothing(HWND window, const TempFolder& folder) {
    ImageExportJob job;
    const auto image = Noise(64, 300, 77u);
    // The destination is a folder: the staged file is written, the swap fails.
    const std::wstring blocked = folder.File(L"occupied.png");
    CHECK(::CreateDirectoryW(blocked.c_str(), nullptr) != FALSE);
    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::File, blocked}));
    auto result = job.TryTake(WaitForCompletion(window, 20000));
    CHECK(result.has_value() && result->status == ImageExportStatus::Failed);
    CHECK(folder.StagedFiles() == 0);
    // A folder that does not exist.
    const std::wstring missing = folder.File(L"missing\\image.png");
    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::File, missing}));
    result = job.TryTake(WaitForCompletion(window, 20000));
    CHECK(result.has_value() && result->status == ImageExportStatus::Failed);
    CHECK(folder.StagedFiles() == 0 && !platform::ImageExportActive());
    // Requests that cannot be carried out never start.
    CHECK(!job.Start(window, platform::kMsgImageExportCompleted, {nullptr, ImageExportTarget::Clipboard, {}}));
    CHECK(!job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::File, {}}));
    CHECK(!job.Start(nullptr, platform::kMsgImageExportCompleted, {image, ImageExportTarget::Clipboard, {}}));
    CHECK(!job.running() && !platform::ImageExportActive());
}

/// Cancelling a long encode returns within a deadline and leaves no file.
void CancelIsPrompt(HWND window, const TempFolder& folder) {
    ImageExportJob job;
    const auto image = Noise(1920, 16000, 0xC0FFEEu);
    const std::wstring path = folder.File(L"cancelled.png");
    const Clock::time_point started = Clock::now();
    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::File, path}));
    // Cancel once the encode is well under way: 8 MB of the staged file are written.
    CHECK(WaitUntil([&] { return folder.StagedBytes() >= 8ull * 1024 * 1024; }, 20000));
    const long long running_ms = MillisecondsSince(started);
    const Clock::time_point cancelled = Clock::now();
    job.Cancel();
    job.Wait();
    const long long cancel_ms = MillisecondsSince(cancelled);
    std::printf("MEASURE image_export cancel: %lld ms after %lld ms of encoding (1920 x 16000)\n", cancel_ms,
                running_ms);
    CHECK(cancel_ms < 1000);
    auto result = job.TryTake(WaitForCompletion(window, 5000));
    CHECK(result.has_value() && result->status == ImageExportStatus::Cancelled);
    CHECK(folder.StagedFiles() == 0 && ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
}

/// Clipboard memory is prepared on the worker; after Wait the result is
/// still there to be taken, and Discard drops one that nobody wants.
void ClipboardResultSurvivesWait(HWND window) {
    ImageExportJob job;
    const auto image = Noise(640, 480, 99u);
    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::Clipboard, {}}));
    job.Wait();
    auto result = job.TryTake(job.generation());
    CHECK(result.has_value() && result->status == ImageExportStatus::Succeeded &&
          result->target == ImageExportTarget::Clipboard);
    CHECK(result.has_value() && !result->clipboard.empty() && result->clipboard.has_png());
    // The wakeup still arrives and finds nothing more to take.
    const WPARAM late = WaitForCompletion(window, 5000);
    CHECK(late == job.generation() && !job.TryTake(late).has_value());

    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::Clipboard, {}}));
    job.Discard();
    CHECK(!job.running() && !platform::ImageExportActive());
    const WPARAM dropped = WaitForCompletion(window, 5000);
    CHECK(dropped == job.generation() && !job.TryTake(dropped).has_value());

    // Session end: a copy is cancelled, and its result can still be taken.
    CHECK(job.Start(window, platform::kMsgImageExportCompleted,
                    {Noise(1920, 8000, 5u), ImageExportTarget::Clipboard, {}}));
    job.SettleForSessionEnd();
    result = job.TryTake(job.generation());
    CHECK(result.has_value() && result->status == ImageExportStatus::Cancelled && result->clipboard.empty());
    static_cast<void>(WaitForCompletion(window, 5000));
    // Also a copy whose worker had finished before the session ended.
    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::Clipboard, {}}));
    job.Wait();
    job.SettleForSessionEnd();
    const WPARAM finished = WaitForCompletion(window, 5000);
    result = job.TryTake(finished);
    CHECK(result.has_value() && result->status == ImageExportStatus::Cancelled && result->clipboard.empty());
}

/// Session end: a save is waited for and lands complete.
void SaveSettlesForSessionEnd(HWND window, const TempFolder& folder) {
    ImageExportJob job;
    const auto image = Noise(1200, 3000, 4242u);
    const std::wstring path = folder.File(L"session end.png");
    CHECK(job.Start(window, platform::kMsgImageExportCompleted, {image, ImageExportTarget::File, path}));
    job.SettleForSessionEnd();
    auto result = job.TryTake(job.generation());
    CHECK(result.has_value() && result->status == ImageExportStatus::Succeeded);
    platform::BgraImage decoded;
    CHECK(test::DecodeImageFile(path, decoded) && decoded.pixels == image->pixels);
    CHECK(folder.StagedFiles() == 0);
    static_cast<void>(WaitForCompletion(window, 5000));
}

}  // namespace

int main() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND window = ::CreateWindowExW(0, L"STATIC", L"image export test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    CHECK(window != nullptr);
    {
        TempFolder folder(L"easnap-image-export");
        CHECK(folder.ok());
        if (window != nullptr && folder.ok()) {
            SaveRoundTrip(window, folder);
            FailedSaveLeavesNothing(window, folder);
            CancelIsPrompt(window, folder);
            ClipboardResultSurvivesWait(window);
            SaveSettlesForSessionEnd(window, folder);
        }
    }
    if (window != nullptr) { ::DestroyWindow(window); }
    ::CoUninitialize();
    std::printf("Image export: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
