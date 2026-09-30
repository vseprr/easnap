// Easnap deterministic GIF backend tests.
#include "media/gif_encoder.h"
#include "media/media_foundation_writer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using easnap::media::GifEncodeDiagnostics;
using easnap::media::GifEncodeOptions;
using easnap::media::GifFrameReadResult;
using easnap::media::GifQuantizationPolicy;
using easnap::media::IGifFrameSource;
using easnap::media::MediaTime;
using easnap::media::VideoFrame;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

constexpr int64_t kCentisecond = 100'000;

uint32_t Color(uint8_t red, uint8_t green, uint8_t blue) {
    return 0xFF000000u | (static_cast<uint32_t>(red) << 16) |
           (static_cast<uint32_t>(green) << 8) | blue;
}

class TestDirectory final {
public:
    bool Create() {
        wchar_t temp[MAX_PATH]{};
        const DWORD count = ::GetTempPathW(ARRAYSIZE(temp), temp);
        GUID id{};
        wchar_t text[40]{};
        if (count == 0 || count >= ARRAYSIZE(temp) || FAILED(::CoCreateGuid(&id)) ||
            ::StringFromGUID2(id, text, ARRAYSIZE(text)) == 0) {
            return false;
        }
        path_ = temp;
        path_ += L"EasnapGifTests-";
        path_ += text;
        return ::CreateDirectoryW(path_.c_str(), nullptr) != FALSE;
    }

    ~TestDirectory() {
        std::error_code ignored;
        if (!path_.empty()) {
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] std::wstring File(const wchar_t* name) const {
        return path_ + L"\\" + name;
    }

    [[nodiscard]] bool HasPartialFiles() const {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(path_, error)) {
            if (entry.path().filename().wstring().find(L".partial.gif") !=
                std::wstring::npos) {
                return true;
            }
        }
        return false;
    }

private:
    std::wstring path_;
};

class GeneratedSource final : public IGifFrameSource {
public:
    using DurationFn = std::function<MediaTime(std::size_t)>;
    using PixelFn =
        std::function<uint32_t(std::size_t, uint32_t, uint32_t)>;

    GeneratedSource(uint32_t width, uint32_t height, std::size_t frame_count,
                    DurationFn duration, PixelFn pixel)
        : width_(width), height_(height), frame_count_(frame_count),
          duration_fn_(std::move(duration)), pixel_fn_(std::move(pixel)) {
        for (std::size_t index = 0; index < frame_count_; ++index) {
            duration_ += duration_fn_(index);
        }
    }

    [[nodiscard]] uint32_t width() const noexcept override { return width_; }
    [[nodiscard]] uint32_t height() const noexcept override { return height_; }
    [[nodiscard]] MediaTime duration() const noexcept override { return duration_; }

    bool Reset() override {
        index_ = 0;
        timestamp_ = MediaTime::zero();
        ++reset_count_;
        return true;
    }

    GifFrameReadResult Read(VideoFrame& frame) override {
        if (index_ >= frame_count_) {
            return GifFrameReadResult::End;
        }
        try {
            auto image = std::make_shared<easnap::platform::BgraImage>();
            image->Reset(static_cast<int>(width_), static_cast<int>(height_));
            for (uint32_t y = 0; y < height_; ++y) {
                for (uint32_t x = 0; x < width_; ++x) {
                    image->row(static_cast<int>(y))[x] = pixel_fn_(index_, x, y);
                }
            }
            frame.image = std::move(image);
            frame.timestamp = timestamp_;
            frame.duration = duration_fn_(index_);
            timestamp_ += frame.duration;
            ++index_;
            ++frames_read_;
            return GifFrameReadResult::Frame;
        } catch (...) {
            error_ = L"Generated source allocation failed";
            return GifFrameReadResult::Error;
        }
    }

    [[nodiscard]] std::wstring last_error() const override { return error_; }
    [[nodiscard]] uint32_t reset_count() const noexcept { return reset_count_; }
    [[nodiscard]] uint64_t frames_read() const noexcept { return frames_read_; }

private:
    uint32_t width_{0};
    uint32_t height_{0};
    std::size_t frame_count_{0};
    DurationFn duration_fn_;
    PixelFn pixel_fn_;
    std::size_t index_{0};
    MediaTime timestamp_{0};
    MediaTime duration_{0};
    std::wstring error_;
    uint32_t reset_count_{0};
    uint64_t frames_read_{0};
};

struct DecodedFrame final {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t left{0};
    uint32_t top{0};
    uint16_t delay{0};
    uint8_t disposal{0};
    bool transparent{false};
    uint8_t transparent_index{0};
    std::vector<uint32_t> canvas;
};

struct DecodedGif final {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<DecodedFrame> frames;
};

uint32_t MetadataUnsigned(IWICMetadataQueryReader* reader, const wchar_t* name,
                          uint32_t fallback = 0) {
    if (reader == nullptr) {
        return fallback;
    }
    PROPVARIANT value;
    ::PropVariantInit(&value);
    const HRESULT result = reader->GetMetadataByName(name, &value);
    uint32_t output = fallback;
    if (SUCCEEDED(result)) {
        if (value.vt == VT_UI1) {
            output = value.bVal;
        } else if (value.vt == VT_UI2) {
            output = value.uiVal;
        } else if (value.vt == VT_UI4) {
            output = value.ulVal;
        } else if (value.vt == VT_BOOL) {
            output = value.boolVal == VARIANT_FALSE ? 0u : 1u;
        }
    }
    ::PropVariantClear(&value);
    return output;
}

bool DecodeWithWic(const std::wstring& path, DecodedGif& decoded) {
    decoded = {};
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
            &decoder);
    }
    UINT frame_count = 0;
    if (SUCCEEDED(result)) {
        result = decoder->GetFrameCount(&frame_count);
    }
    std::vector<uint32_t> canvas;
    for (UINT index = 0; SUCCEEDED(result) && index < frame_count; ++index) {
        ComPtr<IWICBitmapFrameDecode> frame;
        result = decoder->GetFrame(index, &frame);
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(result)) {
            result = frame->GetSize(&width, &height);
        }
        ComPtr<IWICMetadataQueryReader> metadata;
        if (SUCCEEDED(result)) {
            static_cast<void>(frame->GetMetadataQueryReader(&metadata));
        }
        const uint32_t left = MetadataUnsigned(metadata.Get(), L"/imgdesc/Left");
        const uint32_t top = MetadataUnsigned(metadata.Get(), L"/imgdesc/Top");
        if (index == 0) {
            decoded.width = width;
            decoded.height = height;
            canvas.assign(static_cast<std::size_t>(width) * height,
                          Color(0, 0, 0));
        }
        if (left + width > decoded.width || top + height > decoded.height) {
            return false;
        }
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(result)) {
            result = factory->CreateFormatConverter(&converter);
        }
        if (SUCCEEDED(result)) {
            result = converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }
        std::vector<uint32_t> local;
        if (SUCCEEDED(result)) {
            local.resize(static_cast<std::size_t>(width) * height);
            result = converter->CopyPixels(
                nullptr, width * sizeof(uint32_t),
                static_cast<UINT>(local.size() * sizeof(uint32_t)),
                reinterpret_cast<BYTE*>(local.data()));
        }
        if (FAILED(result)) {
            return false;
        }
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t pixel =
                    local[static_cast<std::size_t>(y) * width + x];
                if ((pixel & 0xFF000000u) != 0) {
                    canvas[static_cast<std::size_t>(top + y) * decoded.width +
                           left + x] = pixel;
                }
            }
        }
        DecodedFrame output;
        output.width = width;
        output.height = height;
        output.left = left;
        output.top = top;
        output.delay = static_cast<uint16_t>(
            MetadataUnsigned(metadata.Get(), L"/grctlext/Delay"));
        output.disposal = static_cast<uint8_t>(
            MetadataUnsigned(metadata.Get(), L"/grctlext/Disposal"));
        output.transparent =
            MetadataUnsigned(metadata.Get(), L"/grctlext/TransparencyFlag") != 0;
        output.transparent_index = static_cast<uint8_t>(MetadataUnsigned(
            metadata.Get(), L"/grctlext/TransparencyIndex"));
        output.canvas = canvas;
        decoded.frames.push_back(std::move(output));
    }
    return SUCCEEDED(result) && !decoded.frames.empty();
}

std::vector<uint8_t> ReadBytes(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
}

uint64_t Fingerprint(const std::wstring& path) {
    const std::vector<uint8_t> bytes = ReadBytes(path);
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool HasInfiniteLoopExtension(const std::vector<uint8_t>& bytes) {
    static constexpr std::array<uint8_t, 11> signature = {
        'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
    const auto found = std::search(bytes.begin(), bytes.end(), signature.begin(),
                                   signature.end());
    if (found == bytes.end()) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(found - bytes.begin()) +
                               signature.size();
    return offset + 4 < bytes.size() && bytes[offset] == 3 &&
           bytes[offset + 1] == 1 && bytes[offset + 2] == 0 &&
           bytes[offset + 3] == 0 && bytes[offset + 4] == 0;
}

uint16_t TotalDelay(const DecodedGif& gif) {
    uint32_t total = 0;
    for (const DecodedFrame& frame : gif.frames) {
        total += frame.delay;
    }
    return static_cast<uint16_t>((std::min)(total, uint32_t{65'535}));
}

uint64_t TotalDelay64(const DecodedGif& gif) {
    return std::accumulate(
        gif.frames.begin(), gif.frames.end(), uint64_t{0},
        [](uint64_t total, const DecodedFrame& frame) {
            return total + frame.delay;
        });
}

bool NearColor(uint32_t actual, uint32_t expected, int tolerance = 6) {
    for (uint32_t shift : {0u, 8u, 16u}) {
        const int a = static_cast<int>((actual >> shift) & 0xFFu);
        const int e = static_cast<int>((expected >> shift) & 0xFFu);
        if (std::abs(a - e) > tolerance) {
            return false;
        }
    }
    return true;
}

GifEncodeOptions SharpOptions(uint32_t fps = 100) {
    GifEncodeOptions options;
    options.maximum_frame_rate = fps;
    options.quantization = GifQuantizationPolicy::Sharp;
    return options;
}

GifEncodeOptions LegacyRgb332Options() {
    GifEncodeOptions options;
    options.maximum_frame_rate = 12;
    options.maximum_palette_colors = 256;
    options.quantization = GifQuantizationPolicy::Rgb332Ordered;
    options.palette = easnap::media::GifPalettePolicy::FixedRgb332;
    options.transparent_sparse_deltas = true;
    options.distributed_cadence = true;
    return options;
}
GifEncodeOptions LowSizeOptions() {
    auto options = LegacyRgb332Options();
    options.palette = easnap::media::GifPalettePolicy::FixedWebSafe;
    options.quantization = GifQuantizationPolicy::Balanced;
    options.temporal_tolerance = 8;
    return options;
}

bool Export(IGifFrameSource& source, const std::wstring& path,
            const GifEncodeOptions& options, GifEncodeDiagnostics& diagnostics,
            std::function<bool(float)> progress = {}) {
    std::wstring error;
    const bool result = easnap::media::ExportGif(
        source, path, options, std::move(progress), false, diagnostics, error);
    if (!result && !error.empty()) {
        std::fwprintf(stderr, L"GIF export error: %ls\n", error.c_str());
    }
    return result;
}

void TestSingleFrameAndLoopMetadata(const TestDirectory& directory) {
    GeneratedSource source(7, 5, 1, [](std::size_t) { return MediaTime{20 * kCentisecond}; },
                           [](std::size_t, uint32_t, uint32_t) {
                               return Color(25, 100, 220);
                           });
    const std::wstring path = directory.File(L"single.gif");
    GifEncodeDiagnostics diagnostics;
    std::vector<float> progress;
    CHECK(Export(source, path, SharpOptions(), diagnostics,
                 [&progress](float value) {
                     progress.push_back(value);
                     return true;
                 }));
    CHECK(!progress.empty() && progress.front() == 0.0f &&
          progress.back() == 1.0f);
    CHECK(std::is_sorted(progress.begin(), progress.end()));
    const auto bytes = ReadBytes(path);
    CHECK(bytes.size() > 14);
    CHECK(std::equal(bytes.begin(), bytes.begin() + 6, "GIF89a"));
    CHECK(bytes.back() == 0x3B);
    CHECK(HasInfiniteLoopExtension(bytes));
    const uint32_t table_entries = 1u << ((bytes[10] & 7u) + 1u);
    CHECK(table_entries <= 256);

    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.width == 7 && decoded.height == 5);
    CHECK(decoded.frames.size() == 1);
    CHECK(decoded.frames[0].delay == 20);
    CHECK(decoded.frames[0].disposal == 1);
    CHECK(NearColor(decoded.frames[0].canvas.front(), Color(25, 100, 220)));
}

void TestMultiFrameFirstFinalAndKnownColors(const TestDirectory& directory) {
    const std::array<uint32_t, 3> colors = {
        Color(255, 0, 0), Color(0, 255, 0), Color(0, 0, 255)};
    GeneratedSource source(
        8, 6, colors.size(), [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [colors](std::size_t frame, uint32_t, uint32_t) {
            return colors[frame];
        });
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"colors.gif");
    CHECK(Export(source, path, SharpOptions(), diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.frames.size() == 3);
    CHECK(NearColor(decoded.frames.front().canvas.front(), colors.front()));
    CHECK(NearColor(decoded.frames[1].canvas.front(), colors[1]));
    CHECK(NearColor(decoded.frames.back().canvas.front(), colors.back()));
    CHECK(TotalDelay64(decoded) == 30);
}

void TestScalingOddAndSmallDimensions(const TestDirectory& directory) {
    GeneratedSource source(9, 5, 2, [](std::size_t) { return MediaTime{5 * kCentisecond}; },
                           [](std::size_t frame, uint32_t x, uint32_t y) {
                               return Color(static_cast<uint8_t>(x * 20u),
                                            static_cast<uint8_t>(y * 40u),
                                            static_cast<uint8_t>(frame * 200u));
                           });
    GifEncodeOptions options = SharpOptions();
    options.output_width = 5;
    options.output_height = 0;
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"scaled-odd.gif");
    CHECK(Export(source, path, options, diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.width == 5 && decoded.height == 3);

    GeneratedSource tiny(1, 1, 1, [](std::size_t) { return MediaTime{kCentisecond}; },
                         [](std::size_t, uint32_t, uint32_t) {
                             return Color(9, 8, 7);
                         });
    CHECK(Export(tiny, directory.File(L"tiny.gif"), SharpOptions(), diagnostics));
    CHECK(DecodeWithWic(directory.File(L"tiny.gif"), decoded));
    CHECK(decoded.width == 1 && decoded.height == 1);
}

void TestGradientQuantization(const TestDirectory& directory) {
    GeneratedSource source(
        64, 32, 2, [](std::size_t) { return MediaTime{8 * kCentisecond}; },
        [](std::size_t frame, uint32_t x, uint32_t y) {
            return Color(static_cast<uint8_t>(x * 4u),
                         static_cast<uint8_t>(y * 8u),
                         static_cast<uint8_t>((x * 3u + y * 5u + frame * 17u) &
                                              0xFFu));
        });
    GifEncodeOptions options;
    options.maximum_frame_rate = 100;
    options.maximum_palette_colors = 128;
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"gradient.gif");
    CHECK(Export(source, path, options, diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.frames.size() == 2);
    const auto bytes = ReadBytes(path);
    CHECK((1u << ((bytes[10] & 7u) + 1u)) <= 128);
}

void TestLowSizePaletteSparseDeltasAndStability(
    const TestDirectory& directory) {
    GeneratedSource source(
        32, 16, 3, [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [](std::size_t frame, uint32_t x, uint32_t y) {
            uint32_t color = Color(36, 36, 85);
            if (frame >= 1 && x == 1 && y == 1) {
                color = Color(252, 0, 0);
            }
            if (frame >= 1 && x == 30 && y == 14) {
                color = Color(0, 252, 255);
            }
            if (frame >= 2 && x == 16 && y == 8) {
                color = Color(252, 252, 0);
            }
            return color;
        });
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"low-sparse.gif");
    CHECK(Export(source, path, LegacyRgb332Options(), diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.width == 32 && decoded.height == 16);
    CHECK(decoded.frames.size() == 3);
    CHECK(!decoded.frames[0].transparent);
    CHECK(decoded.frames[1].transparent);
    CHECK(decoded.frames[2].transparent);
    CHECK(decoded.frames[1].disposal == 1);
    CHECK(decoded.frames[1].canvas[8u * decoded.width + 16u] ==
          decoded.frames[0].canvas[8u * decoded.width + 16u]);
    CHECK(decoded.frames[1].canvas[1u * decoded.width + 1u] !=
          decoded.frames[0].canvas[1u * decoded.width + 1u]);
    CHECK(decoded.frames[2].canvas[1u * decoded.width + 1u] ==
          decoded.frames[1].canvas[1u * decoded.width + 1u]);
    CHECK(decoded.frames[2].canvas[8u * decoded.width + 16u] !=
          decoded.frames[1].canvas[8u * decoded.width + 16u]);
    CHECK(diagnostics.palette_entries == 256);
    CHECK(diagnostics.transparent_delta_frames == 2);
    CHECK(diagnostics.opaque_delta_fallback_frames == 0);

    const std::vector<uint8_t> bytes = ReadBytes(path);
    CHECK(bytes.size() >= 13u + 256u * 3u);
    CHECK((bytes[10] & 0x80u) != 0);
    CHECK((1u << ((bytes[10] & 7u) + 1u)) == 256u);
    for (uint32_t red = 0; red < 8; ++red) {
        for (uint32_t green = 0; green < 8; ++green) {
            for (uint32_t blue = 0; blue < 4; ++blue) {
                const std::size_t palette_index =
                    static_cast<std::size_t>(red * 32u + green * 4u + blue);
                const std::size_t offset = 13u + palette_index * 3u;
                CHECK(bytes[offset] == red * 36u);
                CHECK(bytes[offset + 1u] == green * 36u);
                CHECK(bytes[offset + 2u] == blue * 85u);
            }
        }
    }
}

void TestLowSizeOpaqueFallbackAndTransparentDuration(
    const TestDirectory& directory) {
    GeneratedSource all_colors(
        256, 16, 2, [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [](std::size_t frame, uint32_t x, uint32_t y) {
            const uint32_t value = (x + y * 16u + frame + 1u) & 0xFFu;
            const uint8_t red = static_cast<uint8_t>((value >> 5u) * 36u);
            const uint8_t green =
                static_cast<uint8_t>(((value >> 2u) & 7u) * 36u);
            const uint8_t blue = static_cast<uint8_t>((value & 3u) * 85u);
            return Color(red, green, blue);
        });
    GifEncodeDiagnostics diagnostics;
    const std::wstring fallback_path = directory.File(L"low-fallback.gif");
    CHECK(Export(all_colors, fallback_path, LegacyRgb332Options(), diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(fallback_path, decoded));
    CHECK(decoded.frames.size() == 2);
    CHECK(diagnostics.opaque_delta_fallback_frames == 1);
    CHECK(diagnostics.transparent_delta_frames == 0);
    CHECK(!decoded.frames[1].transparent);

    GeneratedSource long_hold(
        2, 2, 1,
        [](std::size_t) {
            return MediaTime{(static_cast<int64_t>(65'535) + 5) *
                             kCentisecond};
        },
        [](std::size_t, uint32_t, uint32_t) { return Color(36, 72, 85); });
    const std::wstring duration_path =
        directory.File(L"low-transparent-duration.gif");
    CHECK(Export(long_hold, duration_path, LegacyRgb332Options(), diagnostics));
    CHECK(DecodeWithWic(duration_path, decoded));
    CHECK(decoded.frames.size() == 2);
    CHECK(decoded.frames[1].width == 1 && decoded.frames[1].height == 1);
    CHECK(decoded.frames[1].transparent);
    CHECK(decoded.frames[1].canvas == decoded.frames[0].canvas);
    CHECK(TotalDelay64(decoded) == 65'540);
    CHECK(diagnostics.duration_only_frames == 1);
}

void TestLowSizeDesktopBenchmark(const TestDirectory& directory) {
    constexpr uint32_t kWidth = 192;
    constexpr uint32_t kHeight = 128;
    constexpr std::size_t kFrames = 90;
    GeneratedSource source(
        kWidth, kHeight, kFrames,
        [](std::size_t) { return MediaTime{333'333}; },
        [](std::size_t frame, uint32_t x, uint32_t y) {
            uint8_t red = static_cast<uint8_t>(14u + x * 24u / kWidth);
            uint8_t green = static_cast<uint8_t>(18u + y * 30u / kHeight);
            uint8_t blue = static_cast<uint8_t>(28u + (x + y) * 24u /
                                                        (kWidth + kHeight));
            if (x >= 12 && x < 180 && y >= 12 && y < 116) {
                red = static_cast<uint8_t>(28u + x * 30u / kWidth);
                green = static_cast<uint8_t>(32u + y * 28u / kHeight);
                blue = static_cast<uint8_t>(45u + x * 18u / kWidth);
            }
            if (y >= 20 && y < 24 && x >= 24 && x < 158) {
                red = 72;
                green = 108;
                blue = 170;
            }
            if (x >= 26 && x < 150 && y >= 38 && y < 92 &&
                ((y - 38u) % 11u == 0 ||
                 ((x - 26u) % 17u == 0 && (y - 38u) % 11u < 6u))) {
                red = 180;
                green = 180;
                blue = 170;
            }
            const uint32_t cursor_x =
                30u + static_cast<uint32_t>((frame * 3u) % 120u);
            const uint32_t cursor_y =
                96u + static_cast<uint32_t>((frame / 4u) % 12u);
            if ((x == cursor_x && y >= cursor_y && y < cursor_y + 9u) ||
                (y == cursor_y && x >= cursor_x && x < cursor_x + 9u)) {
                red = green = blue = 252;
            }
            if ((frame & 1u) != 0 &&
                ((x >= 16 && x < 21 && y >= 16 && y < 21) ||
                 (x >= 171 && x < 176 && y >= 107 && y < 112))) {
                red = 252;
                green = 72;
                blue = 0;
            }
            return Color(red, green, blue);
        });

    GifEncodeDiagnostics high_diagnostics;
    GifEncodeDiagnostics low_diagnostics;
    GifEncodeOptions high_options;
    const std::wstring high_path = directory.File(L"desktop-high.gif");
    const std::wstring low_path = directory.File(L"desktop-low.gif");
    CHECK(Export(source, high_path, high_options, high_diagnostics));
    CHECK(Export(source, low_path, LowSizeOptions(), low_diagnostics));
    DecodedGif high_decoded;
    DecodedGif low_decoded;
    CHECK(DecodeWithWic(high_path, high_decoded));
    CHECK(DecodeWithWic(low_path, low_decoded));
    CHECK(high_decoded.width == kWidth && high_decoded.height == kHeight);
    CHECK(low_decoded.width == kWidth && low_decoded.height == kHeight);
    CHECK(TotalDelay64(high_decoded) == 300);
    CHECK(TotalDelay64(low_decoded) == 300);
    CHECK(HasInfiniteLoopExtension(ReadBytes(high_path)));
    CHECK(HasInfiniteLoopExtension(ReadBytes(low_path)));
    const uint64_t high_bytes = std::filesystem::file_size(high_path);
    const uint64_t low_bytes = std::filesystem::file_size(low_path);
    CHECK(high_bytes == 120'066u);
    CHECK(low_bytes <= 11'172u);
    CHECK(low_bytes * 2u <= high_bytes);
    CHECK(high_diagnostics.transparent_delta_frames == 0);
    CHECK(low_diagnostics.transparent_delta_frames > 0);
    CHECK(low_diagnostics.palette_entries == 226);
    CHECK(high_diagnostics.compressed_image_bytes == 118'730u);
    const double low_ratio =
        high_bytes == 0 ? 0.0
                        : static_cast<double>(low_bytes) /
                              static_cast<double>(high_bytes);
    const double high_average_delta =
        high_diagnostics.delta_frames == 0
            ? 0.0
            : static_cast<double>(high_diagnostics.delta_rectangle_pixels) /
                  static_cast<double>(high_diagnostics.delta_frames * kWidth *
                                      kHeight);
    const double low_average_delta =
        low_diagnostics.delta_frames == 0
            ? 0.0
            : static_cast<double>(low_diagnostics.delta_rectangle_pixels) /
                  static_cast<double>(low_diagnostics.delta_frames * kWidth *
                                      kHeight);
    std::printf(
        "GIF_BENCHMARK high_bytes=%llu low_bytes=%llu ratio=%.4f high_frames=%zu low_frames=%zu high_palette=%llu low_palette=%llu high_compressed=%llu low_compressed=%llu high_avg_delta=%.4f low_avg_delta=%.4f low_transparent_deltas=%llu\n",
        static_cast<unsigned long long>(high_bytes),
        static_cast<unsigned long long>(low_bytes), low_ratio,
        high_decoded.frames.size(), low_decoded.frames.size(),
        static_cast<unsigned long long>(high_diagnostics.palette_entries),
        static_cast<unsigned long long>(low_diagnostics.palette_entries),
        static_cast<unsigned long long>(
            high_diagnostics.compressed_image_bytes),
        static_cast<unsigned long long>(
            low_diagnostics.compressed_image_bytes),
        high_average_delta, low_average_delta,
        static_cast<unsigned long long>(
            low_diagnostics.transparent_delta_frames));
}

void TestLowSizeTemporalStabilityAndMotion(const TestDirectory& directory) {
    // Compression-scale noise must not become animation. Real single-pixel
    // changes and slow accumulated changes must still reach the GIF canvas.
    GeneratedSource source(64, 32, 12,
        [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [](std::size_t frame, uint32_t x, uint32_t y) {
            if (y == 2 && x == frame + 2) return Color(255, 255, 255);
            if (x == 32 && y == 16) {
                const auto c = static_cast<uint8_t>(29 + frame * 3);
                return Color(c, c, c);
            }
            const int noise = frame == 0 ? 0 : static_cast<int>((x + y + frame) % 7) - 3;
            const auto c = static_cast<uint8_t>(29 + noise);
            return Color(c, c, c);
        });
    auto options = LowSizeOptions();
    options.maximum_frame_rate = 10;
    GifEncodeDiagnostics diagnostics;
    const auto path = directory.File(L"low-noise-and-motion.gif");
    CHECK(Export(source, path, options, diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.frames.size() == 12);
    CHECK(TotalDelay64(decoded) == 120);
    if (decoded.frames.size() != 12) return;
    for (std::size_t i = 0; i < decoded.frames.size(); ++i) {
        const auto& canvas = decoded.frames[i].canvas;
        CHECK(NearColor(canvas[2 * 64 + i + 2], Color(255, 255, 255), 10));
        if (i) CHECK(NearColor(canvas[2 * 64 + i + 1], Color(34, 34, 34), 18));
        for (uint32_t y = 20; y < 32; ++y) for (uint32_t x = 0; x < 64; ++x) {
            CHECK(canvas[y * 64 + x] == decoded.frames[0].canvas[y * 64 + x]);
            const auto c = canvas[y * 64 + x];
            CHECK(((c >> 16) & 255u) == ((c >> 8) & 255u) && ((c >> 8) & 255u) == (c & 255u));
        }
    }
    CHECK(decoded.frames.back().canvas[16 * 64 + 32] !=
          decoded.frames.front().canvas[16 * 64 + 32]);
    CHECK(diagnostics.transparent_delta_frames > 0);
    CHECK(source.reset_count() == 1);
}

void TestLzwCodeWidthTransitions(const TestDirectory& directory) {
    GeneratedSource boundary_source(
        255, 1, 1, [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [](std::size_t, uint32_t x, uint32_t) {
            return Color(static_cast<uint8_t>(((x >> 0u) & 31u) * 8u + 4u),
                         static_cast<uint8_t>(((x >> 5u) & 31u) * 8u + 4u),
                         static_cast<uint8_t>(((x >> 10u) & 31u) * 8u + 4u));
        });
    GifEncodeDiagnostics boundary_diagnostics;
    const std::wstring boundary_path =
        directory.File(L"lzw-final-width-boundary.gif");
    CHECK(Export(boundary_source, boundary_path, SharpOptions(),
                 boundary_diagnostics));
    DecodedGif boundary_decoded;
    CHECK(DecodeWithWic(boundary_path, boundary_decoded));
    CHECK(boundary_decoded.width == 255 && boundary_decoded.height == 1);
    CHECK(boundary_decoded.frames.size() == 1);

    GeneratedSource source(
        192, 128, 1, [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [](std::size_t, uint32_t x, uint32_t y) {
            uint32_t value = 1u + x + y * 192u;
            value = (value ^ (value >> 16u)) * 0x7FEB352Du;
            value = (value ^ (value >> 15u)) * 0x846CA68Bu;
            value ^= value >> 16u;
            return Color(static_cast<uint8_t>(value),
                         static_cast<uint8_t>(value >> 8u),
                         static_cast<uint8_t>(value >> 16u));
        });
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"lzw-code-widths.gif");
    CHECK(Export(source, path, SharpOptions(), diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.width == 192 && decoded.height == 128);
    CHECK(decoded.frames.size() == 1);
    CHECK(!directory.HasPartialFiles());
}

void TestCumulativeAndVariableTiming(const TestDirectory& directory) {
    GeneratedSource thirty_fps(
        3, 2, 30, [](std::size_t) { return MediaTime{333'333}; },
        [](std::size_t frame, uint32_t, uint32_t) {
            return Color(static_cast<uint8_t>(frame * 7u), 10, 20);
        });
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"timing.gif");
    CHECK(Export(thirty_fps, path, SharpOptions(), diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(TotalDelay64(decoded) == 100);
    CHECK(diagnostics.encoded_duration_centiseconds == 100);

    const std::array<int64_t, 4> durations = {
        100'000, 250'000, 125'000, 500'000};
    GeneratedSource variable(
        3, 2, durations.size(),
        [durations](std::size_t frame) { return MediaTime{durations[frame]}; },
        [](std::size_t frame, uint32_t, uint32_t) {
            return Color(static_cast<uint8_t>(frame * 60u), 30, 90);
        });
    const std::wstring variable_path = directory.File(L"variable.gif");
    CHECK(Export(variable, variable_path, SharpOptions(), diagnostics));
    CHECK(DecodeWithWic(variable_path, decoded));
    CHECK(decoded.frames.size() == durations.size());
    const std::array<uint16_t, 4> expected = {1, 3, 1, 5};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(decoded.frames[index].delay == expected[index]);
    }
}

void TestTerminalSubCentisecondRemainder(const TestDirectory& directory) {
    GeneratedSource source(
        4, 4, 3,
        [](std::size_t frame) {
            return frame < 2 ? MediaTime{6 * kCentisecond + 60'000}
                             : MediaTime{10'000};
        },
        [](std::size_t frame, uint32_t, uint32_t) {
            return frame == 0 ? Color(220, 20, 20)
                              : (frame == 1 ? Color(20, 220, 20)
                                            : Color(20, 20, 220));
        });
    GifEncodeOptions options = SharpOptions();
    options.maximum_frame_rate = 100;
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"terminal-sub-centisecond.gif");
    CHECK(Export(source, path, options, diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(TotalDelay64(decoded) == 13);
    CHECK(decoded.frames.size() == 3);
    CHECK(NearColor(decoded.frames.back().canvas.front(), Color(20, 20, 220),
                    20));
    CHECK(!directory.HasPartialFiles());
}

void TestLongTimingWithoutDrift(const TestDirectory& directory) {
    constexpr std::size_t kFrames = 3000;
    GeneratedSource source(
        2, 2, kFrames, [](std::size_t) { return MediaTime{333'333}; },
        [](std::size_t frame, uint32_t, uint32_t) {
            return Color(static_cast<uint8_t>(frame & 0xFFu),
                         static_cast<uint8_t>((frame >> 2) & 0xFFu), 33);
        });
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"long-timing.gif");
    CHECK(Export(source, path, SharpOptions(), diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(TotalDelay64(decoded) == 10'000);
    CHECK(diagnostics.encoded_duration_centiseconds == 10'000);
}

void TestUnchangedCoalescingAndLongDelay(const TestDirectory& directory) {
    GeneratedSource unchanged(
        4, 3, 40, [](std::size_t) { return MediaTime{5 * kCentisecond}; },
        [](std::size_t, uint32_t, uint32_t) { return Color(40, 80, 120); });
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"unchanged.gif");
    CHECK(Export(unchanged, path, SharpOptions(), diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.frames.size() == 1);
    CHECK(decoded.frames[0].delay == 200);
    CHECK(diagnostics.unchanged_frames_coalesced >= 39);

    GeneratedSource long_hold(
        2, 2, 1, [](std::size_t) { return MediaTime{700'000 * kCentisecond}; },
        [](std::size_t, uint32_t, uint32_t) { return Color(12, 34, 56); });
    const std::wstring long_path = directory.File(L"long-delay.gif");
    CHECK(Export(long_hold, long_path, SharpOptions(), diagnostics));
    CHECK(DecodeWithWic(long_path, decoded));
    CHECK(decoded.frames.size() == 11);
    CHECK(TotalDelay64(decoded) == 700'000);
    CHECK(decoded.frames[0].delay == 65'535);
    CHECK(NearColor(decoded.frames.back().canvas.front(), Color(12, 34, 56)));
}

void TestDeltaReconstruction(const TestDirectory& directory) {
    GeneratedSource source(
        6, 4, 3, [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [](std::size_t frame, uint32_t x, uint32_t y) {
            if (frame >= 1 && x == 2 && y == 1) {
                return Color(255, 0, 0);
            }
            if (frame >= 2 && x == 4 && y == 3) {
                return Color(0, 255, 0);
            }
            return Color(0, 0, 0);
        });
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"delta.gif");
    CHECK(Export(source, path, SharpOptions(), diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.frames.size() == 3);
    CHECK(diagnostics.full_frames == 1);
    CHECK(diagnostics.delta_frames == 2);
    CHECK(decoded.frames[1].width < decoded.width ||
          decoded.frames[1].height < decoded.height);
    CHECK(NearColor(decoded.frames[1].canvas[1 * decoded.width + 2],
                    Color(255, 0, 0)));
    CHECK(NearColor(decoded.frames[2].canvas[1 * decoded.width + 2],
                    Color(255, 0, 0)));
    CHECK(NearColor(decoded.frames[2].canvas[3 * decoded.width + 4],
                    Color(0, 255, 0)));
}

void TestCancellationInvalidInputAndCleanup(const TestDirectory& directory) {
    GeneratedSource source(
        16, 12, 100, [](std::size_t) { return MediaTime{5 * kCentisecond}; },
        [](std::size_t frame, uint32_t x, uint32_t y) {
            return Color(static_cast<uint8_t>(frame), static_cast<uint8_t>(x * 9u),
                         static_cast<uint8_t>(y * 11u));
        });
    GifEncodeDiagnostics diagnostics;
    std::wstring error;
    const std::wstring cancelled = directory.File(L"cancelled.gif");
    CHECK(!easnap::media::ExportGif(
        source, cancelled, SharpOptions(),
        [](float progress) { return progress < 0.6f; }, false, diagnostics,
        error));
    CHECK(::GetFileAttributesW(cancelled.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(!directory.HasPartialFiles());

    GifEncodeOptions invalid = SharpOptions();
    invalid.maximum_frame_rate = 0;
    CHECK(!easnap::media::ExportGif(source, directory.File(L"invalid.gif"),
                                    invalid, {}, false, diagnostics, error));
    CHECK(!directory.HasPartialFiles());
    CHECK(!easnap::media::ExportGif(source, L"relative.gif", SharpOptions(), {},
                                    false, diagnostics, error));

    const std::wstring write_failure = directory.File(L"bad?.gif");
    CHECK(!easnap::media::ExportGif(source, write_failure, SharpOptions(), {},
                                    false, diagnostics, error));
    CHECK(::GetFileAttributesW(write_failure.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(!directory.HasPartialFiles());
}

void TestLowSizeLateFailureAndCancellation(const TestDirectory& directory) {
    GeneratedSource source(
        16, 12, 40, [](std::size_t) { return MediaTime{5 * kCentisecond}; },
        [](std::size_t frame, uint32_t x, uint32_t y) {
            return Color(static_cast<uint8_t>(frame * 5u),
                         static_cast<uint8_t>(x * 9u), static_cast<uint8_t>(y * 11u));
        });
    const std::wstring path = directory.File(L"low-preserved.gif");
    GifEncodeDiagnostics diagnostics;
    CHECK(Export(source, path, LowSizeOptions(), diagnostics));
    const uint64_t original = Fingerprint(path);

    class LateMalformedFrame final : public IGifFrameSource {
    public:
        explicit LateMalformedFrame(IGifFrameSource& source) : source_(source) {}
        uint32_t width() const noexcept override { return source_.width(); }
        uint32_t height() const noexcept override { return source_.height(); }
        MediaTime duration() const noexcept override { return source_.duration(); }
        bool Reset() override { frames_ = 0; return source_.Reset(); }
        GifFrameReadResult Read(VideoFrame& frame) override {
            const auto result = source_.Read(frame);
            if (result == GifFrameReadResult::Frame && ++frames_ == 30) {
                frame.duration = MediaTime::zero();
            }
            return result;
        }
        std::wstring last_error() const override { return source_.last_error(); }
    private:
        IGifFrameSource& source_;
        uint32_t frames_{0};
    } malformed(source);
    std::wstring error;
    CHECK(!easnap::media::ExportGif(malformed, path, LowSizeOptions(), {}, true,
                                   diagnostics, error));
    CHECK(!error.empty());
    CHECK(Fingerprint(path) == original);
    CHECK(!directory.HasPartialFiles());

    for (const float stop_at : {0.6f, 1.0f}) {
        bool cancelled = false;
        CHECK(!easnap::media::ExportGif(
            source, path, LowSizeOptions(),
            [&](float progress) {
                if (progress >= stop_at) { cancelled = true; return false; }
                return true;
            }, true, diagnostics, error));
        CHECK(cancelled);
        CHECK(Fingerprint(path) == original);
        CHECK(!directory.HasPartialFiles());
    }
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(TotalDelay64(decoded) == 200);
}

void TestBoundedStreaming(const TestDirectory& directory) {
    const auto duration = [](std::size_t) { return MediaTime{2 * kCentisecond}; };
    const auto pixels = [](std::size_t, uint32_t x, uint32_t y) {
        return Color(static_cast<uint8_t>(x * 10u), static_cast<uint8_t>(y * 10u),
                     44);
    };
    GeneratedSource short_source(16, 16, 20, duration, pixels);
    GeneratedSource long_source(16, 16, 5000, duration, pixels);
    GifEncodeDiagnostics short_diagnostics;
    GifEncodeDiagnostics long_diagnostics;
    CHECK(Export(short_source, directory.File(L"bounded-short.gif"), SharpOptions(),
                 short_diagnostics));
    CHECK(Export(long_source, directory.File(L"bounded-long.gif"), SharpOptions(),
                 long_diagnostics));
    CHECK(short_source.reset_count() == 2);
    CHECK(long_source.reset_count() == 2);
    CHECK(long_source.frames_read() == 10'000);
    CHECK(long_diagnostics.source_frames_read == 5000);
    CHECK(long_diagnostics.encoded_frames == 1);
    CHECK(long_diagnostics.peak_working_bytes ==
          short_diagnostics.peak_working_bytes);
    CHECK(long_diagnostics.peak_working_bytes < 4u * 1024u * 1024u);

    GeneratedSource low_short_source(16, 16, 20, duration, pixels);
    GeneratedSource low_long_source(16, 16, 5000, duration, pixels);
    GifEncodeDiagnostics low_short_diagnostics;
    GifEncodeDiagnostics low_long_diagnostics;
    CHECK(Export(low_short_source, directory.File(L"bounded-low-short.gif"),
                 LowSizeOptions(), low_short_diagnostics));
    CHECK(Export(low_long_source, directory.File(L"bounded-low-long.gif"),
                 LowSizeOptions(), low_long_diagnostics));
    CHECK(low_short_source.reset_count() == 1);
    CHECK(low_long_source.reset_count() == 1);
    CHECK(low_long_source.frames_read() == 5000);
    CHECK(low_long_diagnostics.palette_samples == 0);
    CHECK(low_long_diagnostics.source_frames_read == 5000);
    CHECK(low_long_diagnostics.encoded_frames == 1);
    CHECK(low_long_diagnostics.peak_working_bytes ==
          low_short_diagnostics.peak_working_bytes);
    CHECK(low_long_diagnostics.peak_working_bytes < 4u * 1024u * 1024u);
}

void TestCadenceAdaptationPreservesDurationAndEndpoints(
    const TestDirectory& directory) {
    GeneratedSource source(
        3, 2, 10, [](std::size_t) { return MediaTime{2 * kCentisecond}; },
        [](std::size_t frame, uint32_t, uint32_t) {
            return Color(static_cast<uint8_t>(frame * 20u), 30, 40);
        });
    GifEncodeOptions options = SharpOptions(20);
    GifEncodeDiagnostics diagnostics;
    const std::wstring path = directory.File(L"cadence.gif");
    CHECK(Export(source, path, options, diagnostics));
    DecodedGif decoded;
    CHECK(DecodeWithWic(path, decoded));
    CHECK(decoded.frames.size() < 10);
    CHECK(TotalDelay64(decoded) == 20);
    CHECK(NearColor(decoded.frames.front().canvas.front(), Color(0, 30, 40)));
    CHECK(NearColor(decoded.frames.back().canvas.front(), Color(180, 30, 40)));
}

void TestTransactionalReplacement(const TestDirectory& directory) {
    GeneratedSource first(
        3, 3, 1, [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [](std::size_t, uint32_t, uint32_t) { return Color(220, 10, 10); });
    GeneratedSource second(
        3, 3, 1, [](std::size_t) { return MediaTime{10 * kCentisecond}; },
        [](std::size_t, uint32_t, uint32_t) { return Color(10, 10, 220); });
    const std::wstring path = directory.File(L"replace.gif");
    GifEncodeDiagnostics diagnostics;
    std::wstring error;
    CHECK(easnap::media::ExportGif(first, path, SharpOptions(), {}, false,
                                   diagnostics, error));
    const uint64_t original = Fingerprint(path);
    CHECK(!easnap::media::ExportGif(second, path, SharpOptions(), {}, false,
                                    diagnostics, error));
    CHECK(Fingerprint(path) == original);
    CHECK(easnap::media::ExportGif(second, path, SharpOptions(), {}, true,
                                   diagnostics, error));
    CHECK(Fingerprint(path) != original);
    CHECK(!directory.HasPartialFiles());
}

bool WriteSyntheticMp4(const std::wstring& path) {
    auto writer = easnap::media::CreateMediaFoundationWriter();
    if (writer == nullptr) {
        return false;
    }
    easnap::media::RecordingOptions options;
    options.region = RECT{0, 0, 64, 50};
    options.format = easnap::media::RecordingFormat::Mp4;
    options.audio = easnap::media::AudioSource::None;
    options.target_frame_rate = 10;
    if (!writer->Open(path, options)) {
        return false;
    }
    for (int frame_index = 0; frame_index < 6; ++frame_index) {
        auto image = std::make_shared<easnap::platform::BgraImage>();
        image->Reset(64, 50);
        std::fill(image->pixels.begin(), image->pixels.end(),
                  frame_index < 2 ? Color(200, 20, 20)
                                  : (frame_index < 4 ? Color(20, 200, 20)
                                                     : Color(20, 20, 200)));
        VideoFrame frame;
        frame.image = std::move(image);
        frame.timestamp = MediaTime{frame_index * 10 * kCentisecond};
        frame.duration = MediaTime{10 * kCentisecond};
        if (!writer->WriteVideoFrame(frame)) {
            writer->Abort();
            return false;
        }
    }
    std::wstring finalized;
    return writer->Finalize(finalized) && finalized == path;
}

void TestProductionMp4AdapterPreservesSource(const TestDirectory& directory) {
    const std::wstring source_path = directory.File(L"source.mp4");
    const std::wstring gif_path = directory.File(L"from-mp4.gif");
    CHECK(WriteSyntheticMp4(source_path));
    const uint64_t before = Fingerprint(source_path);
    auto exporter = easnap::media::CreateGifExporter();
    CHECK(exporter != nullptr);
    easnap::media::RecordingOptions options;
    options.format = easnap::media::RecordingFormat::Gif;
    options.gif_fps = 10;
    options.gif_dither = false;
    CHECK(exporter->Export(source_path, gif_path, MediaTime{0},
                           MediaTime{60 * kCentisecond}, options,
                           [](float) { return true; }));
    CHECK(Fingerprint(source_path) == before);
    DecodedGif decoded;
    CHECK(DecodeWithWic(gif_path, decoded));
    CHECK(decoded.width == 64 && decoded.height == 50);
    CHECK(decoded.frames.size() >= 2);
    CHECK(NearColor(decoded.frames.front().canvas.front(), Color(200, 20, 20),
                    20));
    CHECK(NearColor(decoded.frames.back().canvas.front(), Color(20, 20, 200),
                    20));

    // Compare the same decoded interval: MF can expose an initial timestamp
    // offset in a generated MP4. Low Size must preserve all time/pictures the
    // adapter yielded, and its palette error is relative to decoded RGB.
    const auto reference_delay = TotalDelay64(decoded);
    const auto reference_first = decoded.frames.front().canvas.front();
    const auto reference_last = decoded.frames.back().canvas.back();
    const auto low_path = directory.File(L"from-mp4-low.gif");
    options.gif_low_size = true;
    options.gif_dither = true;
    options.gif_sparse_transparency = true;
    options.gif_distributed_cadence = true;
    CHECK(exporter->Export(source_path, low_path, MediaTime{0},
                           MediaTime{60 * kCentisecond}, options,
                           [](float) { return true; }));
    CHECK(DecodeWithWic(low_path, decoded));
    CHECK(decoded.width == 64 && decoded.height == 50);
    CHECK(decoded.frames.size() >= 2 && TotalDelay64(decoded) == reference_delay);
    CHECK(!decoded.frames.empty());
    if (!decoded.frames.empty()) {
        CHECK(NearColor(decoded.frames.front().canvas.front(), reference_first, 36));
        CHECK(NearColor(decoded.frames.back().canvas.back(), reference_last, 36));
    }
    CHECK(Fingerprint(source_path) == before);

    const std::wstring invalid_interval =
        directory.File(L"invalid-adapter-interval.gif");
    CHECK(!exporter->Export(source_path, invalid_interval, MediaTime{0},
                            MediaTime{70 * kCentisecond}, options,
                            [](float) { return true; }));
    const easnap::media::TrimExportFailure failure = exporter->LastFailure();
    CHECK(failure.stage ==
          easnap::media::TrimExportFailureStage::SourceValidation);
    CHECK(!failure.detail.empty());
    CHECK(::GetFileAttributesW(invalid_interval.c_str()) ==
          INVALID_FILE_ATTRIBUTES);
    CHECK(Fingerprint(source_path) == before);
    CHECK(!directory.HasPartialFiles());
}

}  // namespace

int main() {
    const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CHECK(SUCCEEDED(com_result));
    CHECK(SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_FULL)));
    TestDirectory directory;
    CHECK(directory.Create());
    if (g_fail == 0) {
        TestSingleFrameAndLoopMetadata(directory);
        TestMultiFrameFirstFinalAndKnownColors(directory);
        TestScalingOddAndSmallDimensions(directory);
        TestGradientQuantization(directory);
        TestLowSizePaletteSparseDeltasAndStability(directory);
        TestLowSizeOpaqueFallbackAndTransparentDuration(directory);
        TestLowSizeDesktopBenchmark(directory);
        TestLowSizeTemporalStabilityAndMotion(directory);
        TestLzwCodeWidthTransitions(directory);
        TestCumulativeAndVariableTiming(directory);
        TestTerminalSubCentisecondRemainder(directory);
        TestLongTimingWithoutDrift(directory);
        TestUnchangedCoalescingAndLongDelay(directory);
        TestDeltaReconstruction(directory);
        TestCancellationInvalidInputAndCleanup(directory);
        TestLowSizeLateFailureAndCancellation(directory);
        TestBoundedStreaming(directory);
        TestCadenceAdaptationPreservesDurationAndEndpoints(directory);
        TestTransactionalReplacement(directory);
        TestProductionMp4AdapterPreservesSource(directory);
    }
    static_cast<void>(::MFShutdown());
    if (SUCCEEDED(com_result)) {
        ::CoUninitialize();
    }
    if (g_fail == 0) {
        std::printf("PASSED  %d/%d tests\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d failed\n", g_pass, g_fail);
    return 1;
}
