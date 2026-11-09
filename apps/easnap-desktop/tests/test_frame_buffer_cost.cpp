// Easnap - what allocating a recorded frame's buffers costs on this machine,
// against reusing them (ADR-042).
//
// Times the production BgraImage::Reset on a new image every iteration (an
// allocation, the opaque fill and the release, as a recording does for every
// frame) and on one image reset to the same size (the fill alone), and a full
// frame copy into that image (what writing every pixel once costs). Then the
// writer's NV12 buffer calls (MFCreateMemoryBuffer, Lock, Unlock,
// SetCurrentLength) with a new buffer every iteration and with one buffer
// reused; both write every byte, as the conversion does, so the pages of a new
// buffer are touched. 1080p and 2160p, the second giving a 4K figure without a
// 4K display. Prints one MEASURE line per operation with the median time and
// the page faults per operation; checks only that sizes and calls are sound,
// never a time.

#include <windows.h>

#include <mfapi.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "platform/platform_utils.h"

namespace {

using Microsoft::WRL::ComPtr;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

constexpr int kIterations = 200;

struct FrameSize {
    int width;
    int height;
};

constexpr FrameSize kSizes[] = {{1920, 1080}, {3840, 2160}};

// Keeps the optimizer from dropping work whose result nothing else reads.
volatile uint32_t g_sink = 0;

uint64_t PageFaults() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) == FALSE) {
        return 0;
    }
    return counters.PageFaultCount;
}

// Runs `operation` kIterations times and prints its median time and the page
// faults per operation. `operation` returns false when a call failed; the
// measurement then stops and is not printed.
template <typename Operation>
void Measure(const char* name, const FrameSize& size, Operation&& operation) {
    using Clock = std::chrono::steady_clock;
    std::vector<double> microseconds;
    microseconds.reserve(kIterations);
    const uint64_t faults_before = PageFaults();
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        const auto start = Clock::now();
        const bool succeeded = operation();
        const auto elapsed = Clock::now() - start;
        CHECK(succeeded);
        if (!succeeded) {
            return;
        }
        microseconds.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
    }
    const uint64_t faults = PageFaults() - faults_before;
    std::sort(microseconds.begin(), microseconds.end());
    const double median =
        (microseconds[kIterations / 2 - 1] + microseconds[kIterations / 2]) / 2.0;
    std::printf("MEASURE %s size=%dx%d median_us=%.0f faults_per_op=%.1f iterations=%d\n", name,
                size.width, size.height, median,
                static_cast<double>(faults) / static_cast<double>(kIterations), kIterations);
}

bool ResetLooksRight(const easnap::platform::BgraImage& image, const FrameSize& size) {
    const std::size_t pixels = static_cast<std::size_t>(size.width) * size.height;
    return image.width == size.width && image.height == size.height &&
           image.pixels.size() == pixels && image.pixels.front() == 0xFF000000u &&
           image.pixels.back() == 0xFF000000u;
}

void MeasureBgraImages(const FrameSize& size) {
    const std::size_t pixels = static_cast<std::size_t>(size.width) * size.height;
    CHECK(pixels > 0);

    Measure("bgra_reset_fresh", size, [&size] {
        easnap::platform::BgraImage image;
        image.Reset(size.width, size.height);
        g_sink = g_sink + image.pixels[image.pixels.size() / 2];
        return ResetLooksRight(image, size);
    });

    easnap::platform::BgraImage reused;
    reused.Reset(size.width, size.height);
    CHECK(ResetLooksRight(reused, size));
    Measure("bgra_reset_reused", size, [&size, &reused] {
        reused.Reset(size.width, size.height);
        g_sink = g_sink + reused.pixels[reused.pixels.size() / 2];
        return ResetLooksRight(reused, size);
    });

    const std::vector<uint32_t> source(pixels, 0xFF336699u);
    Measure("bgra_full_frame_copy", size, [&source, &reused] {
        std::memcpy(reused.pixels.data(), source.data(), source.size() * sizeof(uint32_t));
        g_sink = g_sink + reused.pixels[reused.pixels.size() / 2];
        return reused.pixels.back() == 0xFF336699u;
    });
}

// Fills `buffer` as CoreWriteVideoFrame does: Lock, write every byte, Unlock,
// SetCurrentLength.
bool FillNv12Buffer(IMFMediaBuffer* buffer, DWORD nv12_bytes, BYTE value) {
    BYTE* bytes = nullptr;
    DWORD maximum_length = 0;
    if (FAILED(buffer->Lock(&bytes, &maximum_length, nullptr)) || bytes == nullptr) {
        return false;
    }
    const bool large_enough = maximum_length >= nv12_bytes;
    if (large_enough) {
        std::memset(bytes, value, nv12_bytes);
        g_sink = g_sink + bytes[nv12_bytes / 2];
    }
    const bool unlocked = SUCCEEDED(buffer->Unlock());
    return large_enough && unlocked && SUCCEEDED(buffer->SetCurrentLength(nv12_bytes));
}

void MeasureNv12Buffers(const FrameSize& size) {
    const uint64_t pixels = static_cast<uint64_t>(size.width) * static_cast<uint64_t>(size.height);
    const auto nv12_bytes = static_cast<DWORD>(pixels + pixels / 2);
    CHECK(nv12_bytes > 0);

    BYTE value = 0;
    Measure("nv12_buffer_fresh", size, [nv12_bytes, &value] {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(::MFCreateMemoryBuffer(nv12_bytes, &buffer))) {
            return false;
        }
        return FillNv12Buffer(buffer.Get(), nv12_bytes, ++value);
    });

    ComPtr<IMFMediaBuffer> reused;
    const HRESULT created = ::MFCreateMemoryBuffer(nv12_bytes, &reused);
    CHECK(SUCCEEDED(created));
    if (FAILED(created)) {
        return;
    }
    // Written once before, as the buffer of the previous frame would be.
    CHECK(FillNv12Buffer(reused.Get(), nv12_bytes, value));
    Measure("nv12_buffer_reused", size, [nv12_bytes, &value, &reused] {
        return FillNv12Buffer(reused.Get(), nv12_bytes, ++value);
    });
}

}  // namespace

int main() {
    for (const FrameSize& size : kSizes) {
        MeasureBgraImages(size);
    }

    // As in Easnap, Media Foundation is loaded on first use: Windows N and KN
    // editions have none until the Media Feature Pack is installed.
    const bool media_foundation = easnap::platform::MediaFoundationAvailable();
    if (media_foundation) {
        const HRESULT started = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
        CHECK(SUCCEEDED(started));
        if (SUCCEEDED(started)) {
            for (const FrameSize& size : kSizes) {
                MeasureNv12Buffers(size);
            }
            CHECK(SUCCEEDED(::MFShutdown()));
        }
    }

    if (g_fail != 0) {
        std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
        return 1;
    }
    if (!media_foundation) {
        // The images were measured, the NV12 buffers were not. 77 is
        // SKIP_RETURN_CODE: skipped, not passed.
        std::printf("SKIPPED  %d/%d checks passed, but Windows has no Media Foundation: the "
                    "NV12 buffers were not measured\n",
                    g_pass, g_pass + g_fail);
        return 77;
    }
    std::printf("PASSED  %d/%d checks\n", g_pass, g_pass + g_fail);
    return 0;
}
