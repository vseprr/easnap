// Easnap - bounded animated GIF89a encoder implementation.
#include "media/gif_encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <shlwapi.h>

namespace easnap::media {
namespace {

constexpr int64_t kMediaTicksPerCentisecond = 100'000;
constexpr int64_t kMediaTicksPerSecond = 10'000'000;
constexpr uint16_t kMaximumGifDelay = 65'535;
constexpr uint32_t kHistogramDimension = 32;
constexpr uint32_t kHistogramBinCount =
    kHistogramDimension * kHistogramDimension * kHistogramDimension;
constexpr std::size_t kPaletteSamplesPerFrame = 65'536;

struct Rgb final {
    uint8_t red{0};
    uint8_t green{0};
    uint8_t blue{0};
};

struct HistogramBin final {
    uint64_t count{0};
    uint64_t red_sum{0};
    uint64_t green_sum{0};
    uint64_t blue_sum{0};
};

struct PaletteBin final {
    uint64_t count{0};
    uint64_t red_sum{0};
    uint64_t green_sum{0};
    uint64_t blue_sum{0};
    uint8_t red{0};
    uint8_t green{0};
    uint8_t blue{0};
};

struct PaletteBox final {
    std::size_t begin{0};
    std::size_t end{0};
    uint64_t weight{0};
    uint8_t red_min{0};
    uint8_t red_max{0};
    uint8_t green_min{0};
    uint8_t green_max{0};
    uint8_t blue_min{0};
    uint8_t blue_max{0};
};

struct EncodedRect final {
    uint16_t left{0};
    uint16_t top{0};
    uint16_t width{0};
    uint16_t height{0};
    std::vector<uint8_t> indices;
    uint8_t hold_index{0};
    std::optional<uint8_t> transparent_index;
    bool delta{false};
    MediaTime duration{0};
};

void SetError(std::wstring& error, const wchar_t* message) {
    error = message;
    platform::LogMessage(L"[GIF] %s", message);
}

void RecordFailure(TrimExportFailure* failure, TrimExportFailureStage stage,
                   const std::wstring& detail) {
    if (failure == nullptr) {
        return;
    }
    failure->stage = stage;
    failure->has_hresult = false;
    failure->hresult = 0;
    failure->detail = detail;
}

void RecordSourceFailure(TrimExportFailure* failure,
                         const IGifFrameSource& source,
                         TrimExportFailureStage fallback_stage,
                         const std::wstring& detail) {
    if (failure == nullptr) {
        return;
    }
    *failure = source.last_failure();
    if (failure->stage == TrimExportFailureStage::None) {
        RecordFailure(failure, fallback_stage, detail);
    }
}

bool ReportProgress(const GifProgressCallback& callback, float value,
                    std::wstring& error) {
    if (!callback || callback((std::max)(0.0f, (std::min)(1.0f, value)))) {
        return true;
    }
    SetError(error, L"GIF export was cancelled");
    return false;
}

bool CheckedPixelCount(uint32_t width, uint32_t height,
                       std::size_t& count) noexcept {
    if (width == 0 || height == 0 || width > 65'535 || height > 65'535) {
        return false;
    }
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    if (pixels > (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }
    count = static_cast<std::size_t>(pixels);
    return true;
}

bool ResolveDimensions(uint32_t source_width, uint32_t source_height,
                       const GifEncodeOptions& options, uint32_t& output_width,
                       uint32_t& output_height, std::wstring& error) {
    std::size_t source_pixels = 0;
    if (!CheckedPixelCount(source_width, source_height, source_pixels)) {
        SetError(error, L"The GIF source dimensions are invalid");
        return false;
    }
    output_width = options.output_width;
    output_height = options.output_height;
    if (output_width == 0 && output_height == 0) {
        output_width = source_width;
        output_height = source_height;
    } else if (output_width == 0) {
        const uint64_t scaled =
            static_cast<uint64_t>(source_width) * output_height + source_height / 2u;
        output_width = static_cast<uint32_t>((std::max)(uint64_t{1}, scaled / source_height));
    } else if (output_height == 0) {
        const uint64_t scaled =
            static_cast<uint64_t>(source_height) * output_width + source_width / 2u;
        output_height = static_cast<uint32_t>((std::max)(uint64_t{1}, scaled / source_width));
    }

    std::size_t output_pixels = 0;
    if (!CheckedPixelCount(output_width, output_height, output_pixels)) {
        SetError(error, L"The requested GIF output dimensions are invalid");
        return false;
    }
    if (options.maximum_frame_rate == 0 || options.maximum_frame_rate > 100) {
        SetError(error, L"The GIF frame-rate cap must be between 1 and 100 fps");
        return false;
    }
    if (options.maximum_palette_colors < 2 ||
        options.maximum_palette_colors > 256) {
        SetError(error, L"The GIF palette size must be between 2 and 256 colors");
        return false;
    }
    const bool quantization_valid =
        options.quantization == GifQuantizationPolicy::Sharp ||
        options.quantization == GifQuantizationPolicy::Balanced ||
        options.quantization == GifQuantizationPolicy::Rgb332Ordered;
    const bool palette_valid =
        options.palette == GifPalettePolicy::AdaptiveMedianCut ||
        options.palette == GifPalettePolicy::FixedRgb332 ||
        options.palette == GifPalettePolicy::FixedWebSafe;
    if (!quantization_valid || !palette_valid) {
        SetError(error, L"The GIF palette or quantization policy is invalid");
        return false;
    }
    if (options.palette == GifPalettePolicy::FixedRgb332 &&
        (options.maximum_palette_colors != 256 ||
         options.quantization != GifQuantizationPolicy::Rgb332Ordered)) {
        SetError(error,
                 L"The fixed RGB332 GIF palette requires its 256-color ordered mapping");
        return false;
    }
    if (options.palette == GifPalettePolicy::FixedWebSafe &&
        options.maximum_palette_colors != 256) {
        SetError(error, L"The fixed neutral GIF palette requires the 256-color table budget");
        return false;
    }
    return true;
}

std::shared_ptr<const platform::BgraImage> ScaleFrame(
    const std::shared_ptr<const platform::BgraImage>& source,
    uint32_t output_width, uint32_t output_height, std::wstring& error) {
    if (source == nullptr || source->empty()) {
        SetError(error, L"The GIF source returned an empty video frame");
        return nullptr;
    }
    if (source->width == static_cast<int>(output_width) &&
        source->height == static_cast<int>(output_height)) {
        return source;
    }

    try {
        auto output = std::make_shared<platform::BgraImage>();
        output->Reset(static_cast<int>(output_width), static_cast<int>(output_height));
        const double source_width = static_cast<double>(source->width);
        const double source_height = static_cast<double>(source->height);
        for (uint32_t y = 0; y < output_height; ++y) {
            const double source_y =
                (static_cast<double>(y) + 0.5) * source_height /
                    static_cast<double>(output_height) -
                0.5;
            const int y0 = std::clamp(static_cast<int>(std::floor(source_y)), 0,
                                      source->height - 1);
            const int y1 = (std::min)(y0 + 1, source->height - 1);
            const double fy = std::clamp(source_y - std::floor(source_y), 0.0, 1.0);
            for (uint32_t x = 0; x < output_width; ++x) {
                const double source_x =
                    (static_cast<double>(x) + 0.5) * source_width /
                        static_cast<double>(output_width) -
                    0.5;
                const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0,
                                          source->width - 1);
                const int x1 = (std::min)(x0 + 1, source->width - 1);
                const double fx =
                    std::clamp(source_x - std::floor(source_x), 0.0, 1.0);
                const uint32_t p00 = source->row(y0)[x0];
                const uint32_t p10 = source->row(y0)[x1];
                const uint32_t p01 = source->row(y1)[x0];
                const uint32_t p11 = source->row(y1)[x1];
                const auto channel = [&](uint32_t shift) {
                    const double top =
                        static_cast<double>((p00 >> shift) & 0xFFu) * (1.0 - fx) +
                        static_cast<double>((p10 >> shift) & 0xFFu) * fx;
                    const double bottom =
                        static_cast<double>((p01 >> shift) & 0xFFu) * (1.0 - fx) +
                        static_cast<double>((p11 >> shift) & 0xFFu) * fx;
                    return static_cast<uint32_t>(std::clamp(
                        std::lround(top * (1.0 - fy) + bottom * fy), 0L, 255L));
                };
                output->row(static_cast<int>(y))[x] =
                    0xFF000000u | (channel(16) << 16) | (channel(8) << 8) |
                    channel(0);
            }
        }
        return output;
    } catch (const std::bad_alloc&) {
        SetError(error, L"Allocating a bounded scaled GIF frame failed");
        return nullptr;
    }
}

bool ImagesEqual(const std::shared_ptr<const platform::BgraImage>& left,
                 const std::shared_ptr<const platform::BgraImage>& right) {
    return left != nullptr && right != nullptr && left->width == right->width &&
           left->height == right->height && left->pixels == right->pixels;
}

class PaletteHistogram final {
public:
    void Add(const platform::BgraImage& image) noexcept {
        const std::size_t pixel_count = image.pixels.size();
        const std::size_t step =
            (std::max)(std::size_t{1},
                       (pixel_count + kPaletteSamplesPerFrame - 1) /
                           kPaletteSamplesPerFrame);
        for (std::size_t index = 0; index < pixel_count; index += step) {
            const uint32_t pixel = image.pixels[index];
            const uint8_t blue = static_cast<uint8_t>(pixel & 0xFFu);
            const uint8_t green = static_cast<uint8_t>((pixel >> 8) & 0xFFu);
            const uint8_t red = static_cast<uint8_t>((pixel >> 16) & 0xFFu);
            const uint32_t bin_index =
                (static_cast<uint32_t>(red >> 3) << 10) |
                (static_cast<uint32_t>(green >> 3) << 5) |
                static_cast<uint32_t>(blue >> 3);
            HistogramBin& bin = bins_[bin_index];
            if (bin.count != (std::numeric_limits<uint64_t>::max)()) {
                ++bin.count;
                bin.red_sum += red;
                bin.green_sum += green;
                bin.blue_sum += blue;
            }
            ++samples_;
        }
    }

    [[nodiscard]] uint64_t samples() const noexcept { return samples_; }

    bool Build(uint16_t maximum_colors, std::vector<Rgb>& palette,
               std::wstring& error) const {
        std::vector<PaletteBin> bins;
        try {
            bins.reserve(kHistogramBinCount);
            for (uint32_t index = 0; index < kHistogramBinCount; ++index) {
                const HistogramBin& source = bins_[index];
                if (source.count == 0) {
                    continue;
                }
                bins.push_back(PaletteBin{
                    source.count, source.red_sum, source.green_sum,
                    source.blue_sum,
                    static_cast<uint8_t>((index >> 10) << 3),
                    static_cast<uint8_t>(((index >> 5) & 31u) << 3),
                    static_cast<uint8_t>((index & 31u) << 3)});
            }
        } catch (const std::bad_alloc&) {
            SetError(error, L"Allocating bounded GIF palette analysis failed");
            return false;
        }
        if (bins.empty()) {
            SetError(error, L"The GIF source contains no palette samples");
            return false;
        }

        const auto describe = [&bins](std::size_t begin, std::size_t end) {
            PaletteBox box;
            box.begin = begin;
            box.end = end;
            box.red_min = box.green_min = box.blue_min = 255;
            for (std::size_t index = begin; index < end; ++index) {
                const PaletteBin& bin = bins[index];
                box.weight += bin.count;
                box.red_min = (std::min)(box.red_min, bin.red);
                box.red_max = (std::max)(box.red_max, bin.red);
                box.green_min = (std::min)(box.green_min, bin.green);
                box.green_max = (std::max)(box.green_max, bin.green);
                box.blue_min = (std::min)(box.blue_min, bin.blue);
                box.blue_max = (std::max)(box.blue_max, bin.blue);
            }
            return box;
        };

        std::vector<PaletteBox> boxes;
        try {
            boxes.reserve(maximum_colors);
            boxes.push_back(describe(0, bins.size()));
            while (boxes.size() < maximum_colors) {
                std::size_t selected = boxes.size();
                uint64_t selected_score = 0;
                for (std::size_t index = 0; index < boxes.size(); ++index) {
                    const PaletteBox& box = boxes[index];
                    if (box.end - box.begin < 2) {
                        continue;
                    }
                    const uint32_t range = (std::max)(
                        {static_cast<uint32_t>(box.red_max - box.red_min),
                         static_cast<uint32_t>(box.green_max - box.green_min),
                         static_cast<uint32_t>(box.blue_max - box.blue_min)});
                    const uint64_t score = box.weight * (range + 1u);
                    if (selected == boxes.size() || score > selected_score) {
                        selected = index;
                        selected_score = score;
                    }
                }
                if (selected == boxes.size()) {
                    break;
                }

                const PaletteBox box = boxes[selected];
                const uint32_t red_range = box.red_max - box.red_min;
                const uint32_t green_range = box.green_max - box.green_min;
                const uint32_t blue_range = box.blue_max - box.blue_min;
                enum class Channel { Red, Green, Blue };
                const Channel channel =
                    red_range >= green_range && red_range >= blue_range
                        ? Channel::Red
                        : (green_range >= blue_range ? Channel::Green
                                                     : Channel::Blue);
                std::stable_sort(
                    bins.begin() + static_cast<std::ptrdiff_t>(box.begin),
                    bins.begin() + static_cast<std::ptrdiff_t>(box.end),
                    [channel](const PaletteBin& left, const PaletteBin& right) {
                        if (channel == Channel::Red) {
                            return left.red < right.red;
                        }
                        if (channel == Channel::Green) {
                            return left.green < right.green;
                        }
                        return left.blue < right.blue;
                    });
                const uint64_t half = (box.weight + 1u) / 2u;
                uint64_t accumulated = 0;
                std::size_t split = box.begin + 1;
                for (std::size_t index = box.begin;
                     index + 1 < box.end; ++index) {
                    accumulated += bins[index].count;
                    split = index + 1;
                    if (accumulated >= half) {
                        break;
                    }
                }
                boxes[selected] = describe(box.begin, split);
                boxes.push_back(describe(split, box.end));
            }

            palette.clear();
            palette.reserve(boxes.size());
            for (const PaletteBox& box : boxes) {
                uint64_t weight = 0;
                uint64_t red = 0;
                uint64_t green = 0;
                uint64_t blue = 0;
                for (std::size_t index = box.begin; index < box.end; ++index) {
                    weight += bins[index].count;
                    red += bins[index].red_sum;
                    green += bins[index].green_sum;
                    blue += bins[index].blue_sum;
                }
                if (weight == 0) {
                    continue;
                }
                palette.push_back(Rgb{
                    static_cast<uint8_t>((red + weight / 2u) / weight),
                    static_cast<uint8_t>((green + weight / 2u) / weight),
                    static_cast<uint8_t>((blue + weight / 2u) / weight)});
            }
            if (palette.size() == 1) {
                palette.push_back(palette.front());
            }
        } catch (const std::bad_alloc&) {
            SetError(error, L"Generating the bounded GIF palette failed");
            return false;
        }
        return palette.size() >= 2 && palette.size() <= maximum_colors;
    }

private:
    std::array<HistogramBin, kHistogramBinCount> bins_{};
    uint64_t samples_{0};
};

std::vector<Rgb> BuildRgb332Palette() {
    std::vector<Rgb> palette;
    palette.reserve(256);
    for (uint16_t red = 0; red < 8; ++red) {
        for (uint16_t green = 0; green < 8; ++green) {
            for (uint16_t blue = 0; blue < 4; ++blue) {
                palette.push_back(Rgb{
                    static_cast<uint8_t>(red * 36u),
                    static_cast<uint8_t>(green * 36u),
                    static_cast<uint8_t>(blue * 85u)});
            }
        }
    }
    return palette;
}

// A neutral ramp keeps desktop grays neutral. The 6x6x6 color cube
// preserves icon colors while a restrained 4x4 dither avoids RGB332's
// high-amplitude blue pattern and its expensive changes at palette boundaries.
std::vector<Rgb> BuildWebSafePalette() {
    std::vector<Rgb> palette;
    palette.reserve(226);
    for (unsigned r = 0; r < 6; ++r) {
        for (unsigned g = 0; g < 6; ++g) {
            for (unsigned b = 0; b < 6; ++b) {
                palette.push_back(Rgb{static_cast<uint8_t>(r * 51),
                                      static_cast<uint8_t>(g * 51),
                                      static_cast<uint8_t>(b * 51)});
            }
        }
    }
    for (unsigned i = 1; i < 15; ++i) {
        if (i % 3 == 0) continue;  // Already present on the cube diagonal.
        const auto gray = static_cast<uint8_t>(i * 17);
        palette.push_back(Rgb{gray, gray, gray});
    }
    return palette;
}

class PaletteQuantizer final {
public:
    bool Initialize(std::vector<Rgb> palette, GifQuantizationPolicy policy,
                    std::wstring& error) {
        if (palette.size() < 2 || palette.size() > 256) {
            SetError(error, L"The generated GIF palette is invalid");
            return false;
        }
        palette_ = std::move(palette);
        policy_ = policy;
        for (uint32_t bin = 0; bin < kHistogramBinCount; ++bin) {
            const int red = static_cast<int>(((bin >> 10) & 31u) * 8u + 4u);
            const int green = static_cast<int>(((bin >> 5) & 31u) * 8u + 4u);
            const int blue = static_cast<int>((bin & 31u) * 8u + 4u);
            uint32_t best_distance = (std::numeric_limits<uint32_t>::max)();
            uint8_t best = 0;
            for (std::size_t index = 0; index < palette_.size(); ++index) {
                const int dr = red - palette_[index].red;
                const int dg = green - palette_[index].green;
                const int db = blue - palette_[index].blue;
                const uint32_t distance = static_cast<uint32_t>(
                    dr * dr * 3 + dg * dg * 6 + db * db * 2);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = static_cast<uint8_t>(index);
                }
            }
            lookup_[bin] = best;
        }
        return true;
    }

    [[nodiscard]] const std::vector<Rgb>& palette() const noexcept {
        return palette_;
    }

    bool Quantize(const platform::BgraImage& image,
                  std::vector<uint8_t>& indices, std::wstring& error) const {
        static constexpr std::array<int, 16> kBayer4x4 = {
            0, 8, 2, 10, 12, 4, 14, 6,
            3, 11, 1, 9, 15, 7, 13, 5};
        try {
            indices.resize(image.pixels.size());
        } catch (const std::bad_alloc&) {
            SetError(error, L"Allocating a bounded quantized GIF frame failed");
            return false;
        }
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const uint32_t pixel = image.row(y)[x];
                int blue = static_cast<int>(pixel & 0xFFu);
                int green = static_cast<int>((pixel >> 8) & 0xFFu);
                int red = static_cast<int>((pixel >> 16) & 0xFFu);
                if (policy_ == GifQuantizationPolicy::Balanced) {
                    const int offset = kBayer4x4[static_cast<std::size_t>(
                                           (y & 3) * 4 + (x & 3))] -
                                       8;
                    red = std::clamp(red + offset, 0, 255);
                    green = std::clamp(green + offset, 0, 255);
                    blue = std::clamp(blue + offset, 0, 255);
                } else if (policy_ == GifQuantizationPolicy::Rgb332Ordered) {
                    const int pattern = kBayer4x4[static_cast<std::size_t>(
                                            (y & 3) * 4 + (x & 3))] -
                                        8;
                    red = std::clamp(red + pattern * 36 / 16, 0, 255);
                    green = std::clamp(green + pattern * 36 / 16, 0, 255);
                    blue = std::clamp(blue + pattern * 85 / 16, 0, 255);
                }
                const uint32_t bin =
                    (static_cast<uint32_t>(red >> 3) << 10) |
                    (static_cast<uint32_t>(green >> 3) << 5) |
                    static_cast<uint32_t>(blue >> 3);
                indices[static_cast<std::size_t>(y) * image.width + x] =
                    lookup_[bin];
            }
        }
        return true;
    }

private:
    std::vector<Rgb> palette_;
    std::array<uint8_t, kHistogramBinCount> lookup_{};
    GifQuantizationPolicy policy_{GifQuantizationPolicy::Balanced};
};

class FileWriter final {
public:
    explicit FileWriter(const std::wstring& path) {
        handle_ = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    ~FileWriter() { Close(); }

    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    bool Write(std::span<const uint8_t> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(
                bytes.size() - offset,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (::WriteFile(handle_, bytes.data() + offset, chunk, &written,
                            nullptr) == FALSE ||
                written != chunk) {
                return false;
            }
            offset += written;
            bytes_written_ += written;
        }
        return true;
    }

    bool WriteByte(uint8_t value) {
        return Write(std::span<const uint8_t>(&value, 1));
    }

    bool WriteWord(uint16_t value) {
        const std::array<uint8_t, 2> bytes = {
            static_cast<uint8_t>(value & 0xFFu),
            static_cast<uint8_t>(value >> 8)};
        return Write(bytes);
    }

    [[nodiscard]] uint64_t bytes_written() const noexcept {
        return bytes_written_;
    }

    bool FlushAndClose() {
        if (!valid()) {
            return false;
        }
        const bool flushed = ::FlushFileBuffers(handle_) != FALSE;
        const bool closed = ::CloseHandle(handle_) != FALSE;
        handle_ = INVALID_HANDLE_VALUE;
        return flushed && closed;
    }

private:
    void Close() noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE handle_{INVALID_HANDLE_VALUE};
    uint64_t bytes_written_{0};
};

class IncompleteFile final {
public:
    explicit IncompleteFile(std::wstring path) : path_(std::move(path)) {}
    ~IncompleteFile() {
        if (armed_ && !path_.empty()) {
            static_cast<void>(::DeleteFileW(path_.c_str()));
        }
    }
    IncompleteFile(const IncompleteFile&) = delete;
    IncompleteFile& operator=(const IncompleteFile&) = delete;
    void Disarm() noexcept { armed_ = false; }

private:
    std::wstring path_;
    bool armed_{true};
};

bool IsAbsoluteGifPath(const std::wstring& path) noexcept {
    return !path.empty() && ::PathIsRelativeW(path.c_str()) == FALSE &&
           _wcsicmp(::PathFindExtensionW(path.c_str()), L".gif") == 0;
}

bool ValidateDestination(const std::wstring& destination, bool replace_existing,
                         std::wstring& error) {
    if (!IsAbsoluteGifPath(destination)) {
        SetError(error, L"The GIF destination must be an absolute .gif path");
        return false;
    }
    const DWORD destination_attributes = ::GetFileAttributesW(destination.c_str());
    if (destination_attributes != INVALID_FILE_ATTRIBUTES &&
        (!replace_existing ||
         (destination_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        SetError(error, L"The GIF destination already exists without replacement approval");
        return false;
    }
    const std::size_t separator = destination.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        SetError(error, L"The GIF destination directory could not be located");
        return false;
    }
    const std::wstring parent = destination.substr(0, separator);
    const DWORD parent_attributes = ::GetFileAttributesW(parent.c_str());
    if (parent_attributes == INVALID_FILE_ATTRIBUTES ||
        (parent_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        SetError(error, L"The GIF destination directory does not exist");
        return false;
    }
    return true;
}

std::optional<std::wstring> MakeStagingPath(const std::wstring& destination,
                                             std::wstring& error) {
    GUID identifier{};
    if (FAILED(::CoCreateGuid(&identifier))) {
        SetError(error, L"Creating the GIF staging identifier failed");
        return std::nullopt;
    }
    wchar_t text[40]{};
    if (::StringFromGUID2(identifier, text, static_cast<int>(std::size(text))) == 0) {
        SetError(error, L"Formatting the GIF staging identifier failed");
        return std::nullopt;
    }
    std::wstring staging = destination;
    staging += L".easnap-";
    staging += text;
    staging += L".partial.gif";
    if (::GetFileAttributesW(staging.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetError(error, L"The unique GIF staging path already exists");
        return std::nullopt;
    }
    return staging;
}

uint8_t ColorTableBits(std::size_t palette_size) noexcept {
    uint8_t bits = 1;
    std::size_t entries = 2;
    while (entries < palette_size && bits < 8) {
        ++bits;
        entries <<= 1;
    }
    return bits;
}

class GifStreamWriter final {
public:
    GifStreamWriter(FileWriter& file, uint16_t width, uint16_t height,
                    const PaletteQuantizer& quantizer,
                    bool transparent_sparse_deltas,
                    uint8_t temporal_tolerance,
                    uint64_t scaling_working_bytes,
                    GifEncodeDiagnostics& diagnostics)
        : file_(file), width_(width), height_(height), quantizer_(quantizer),
          transparent_sparse_deltas_(transparent_sparse_deltas),
          temporal_tolerance_(temporal_tolerance),
          scaling_working_bytes_(scaling_working_bytes),
          diagnostics_(diagnostics) {}

    bool Open(uint16_t loop_count, std::wstring& error) {
        const std::array<uint8_t, 6> signature = {'G', 'I', 'F', '8', '9', 'a'};
        const uint8_t table_bits = ColorTableBits(quantizer_.palette().size());
        const uint8_t packed = static_cast<uint8_t>(0x80u | 0x70u |
                                                    (table_bits - 1u));
        if (!file_.Write(signature) || !file_.WriteWord(width_) ||
            !file_.WriteWord(height_) || !file_.WriteByte(packed) ||
            !file_.WriteByte(0) || !file_.WriteByte(0)) {
            SetError(error, L"Writing the GIF logical screen descriptor failed");
            return false;
        }
        const std::size_t table_size = std::size_t{1} << table_bits;
        for (std::size_t index = 0; index < table_size; ++index) {
            const Rgb color = index < quantizer_.palette().size()
                                  ? quantizer_.palette()[index]
                                  : quantizer_.palette().back();
            const std::array<uint8_t, 3> bytes = {color.red, color.green,
                                                   color.blue};
            if (!file_.Write(bytes)) {
                SetError(error, L"Writing the GIF global color table failed");
                return false;
            }
        }

        static constexpr std::array<uint8_t, 14> kLoopPrefix = {
            0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2',
            '.', '0'};
        if (!file_.Write(kLoopPrefix) || !file_.WriteByte(3) ||
            !file_.WriteByte(1) || !file_.WriteWord(loop_count) ||
            !file_.WriteByte(0)) {
            SetError(error, L"Writing the GIF loop extension failed");
            return false;
        }
        opened_ = true;
        return true;
    }

    bool Add(const platform::BgraImage& image, MediaTime duration,
             std::wstring& error) {
        std::vector<uint8_t> current;
        const platform::BgraImage* input = &image;
        if (temporal_tolerance_ != 0) {
            if (stable_pixels_.empty()) {
                stable_pixels_ = image;
            } else {
                for (std::size_t i = 0; i < image.pixels.size(); ++i) {
                    const uint32_t now = image.pixels[i], anchor = stable_pixels_.pixels[i];
                    const auto distance = [now, anchor](unsigned shift) {
                        return std::abs(static_cast<int>((now >> shift) & 255u) -
                                        static_cast<int>((anchor >> shift) & 255u));
                    };
                    if (distance(0) > temporal_tolerance_ || distance(8) > temporal_tolerance_ ||
                        distance(16) > temporal_tolerance_) stable_pixels_.pixels[i] = now;
                }
            }
            input = &stable_pixels_;
        }
        if (!quantizer_.Quantize(*input, current, error)) {
            return false;
        }
        UpdatePeak(current.capacity());
        diagnostics_.intended_duration_100ns +=
            static_cast<uint64_t>(duration.count());
        if (previous_canvas_.empty()) {
            previous_canvas_ = current;
            pending_ = MakeFullFrame(current, duration);
            UpdatePeak(current.capacity());
            return true;
        }
        if (current == previous_canvas_) {
            pending_->duration += duration;
            ++diagnostics_.unchanged_frames_coalesced;
            return true;
        }
        const uint64_t reserved_centiseconds =
            ShouldReserveCentisecondForNext(duration) ? 1u : 0u;
        if (!FlushPending(error, reserved_centiseconds)) {
            return false;
        }
        pending_ = MakeDeltaFrame(previous_canvas_, current, duration);
        previous_canvas_.swap(current);
        UpdatePeak(current.capacity());
        return true;
    }

    bool Finalize(std::wstring& error) {
        if (!opened_ || !pending_.has_value()) {
            SetError(error, L"The GIF stream has no encodable frames");
            return false;
        }
        if (!FlushPending(error) || !file_.WriteByte(0x3B)) {
            if (error.empty()) {
                SetError(error, L"Writing the GIF trailer failed");
            }
            return false;
        }
        return true;
    }

private:
    EncodedRect MakeFullFrame(const std::vector<uint8_t>& canvas,
                              MediaTime duration) const {
        EncodedRect frame;
        frame.width = width_;
        frame.height = height_;
        frame.indices = canvas;
        frame.hold_index = canvas.front();
        frame.duration = duration;
        return frame;
    }

    EncodedRect MakeDeltaFrame(const std::vector<uint8_t>& previous,
                               const std::vector<uint8_t>& current,
                               MediaTime duration) const {
        uint16_t left = width_;
        uint16_t top = height_;
        uint16_t right = 0;
        uint16_t bottom = 0;
        for (uint16_t y = 0; y < height_; ++y) {
            for (uint16_t x = 0; x < width_; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width_ + x;
                if (previous[index] == current[index]) {
                    continue;
                }
                left = (std::min)(left, x);
                top = (std::min)(top, y);
                right = (std::max)(right, x);
                bottom = (std::max)(bottom, y);
            }
        }
        EncodedRect frame;
        frame.left = left;
        frame.top = top;
        frame.width = static_cast<uint16_t>(right - left + 1u);
        frame.height = static_cast<uint16_t>(bottom - top + 1u);
        frame.duration = duration;
        frame.hold_index = current.front();
        frame.delta = true;
        frame.indices.reserve(static_cast<std::size_t>(frame.width) * frame.height);
        std::optional<uint8_t> transparent_index;
        if (transparent_sparse_deltas_) {
            std::array<bool, 256> used_by_changes{};
            for (uint16_t y = top; y <= bottom; ++y) {
                for (uint16_t x = left; x <= right; ++x) {
                    const std::size_t index =
                        static_cast<std::size_t>(y) * width_ + x;
                    if (previous[index] != current[index]) {
                        used_by_changes[current[index]] = true;
                    }
                }
            }
            for (std::size_t index = 0;
                 index < quantizer_.palette().size(); ++index) {
                if (!used_by_changes[index]) {
                    transparent_index = static_cast<uint8_t>(index);
                    break;
                }
            }
            if (!transparent_index.has_value()) {
                ++diagnostics_.opaque_delta_fallback_frames;
            }
        }
        frame.transparent_index = transparent_index;
        for (uint16_t y = top; y <= bottom; ++y) {
            for (uint16_t x = left; x <= right; ++x) {
                const std::size_t index =
                    static_cast<std::size_t>(y) * width_ + x;
                frame.indices.push_back(
                    transparent_index.has_value() &&
                            previous[index] == current[index]
                        ? *transparent_index
                        : current[index]);
            }
        }
        return frame;
    }

    class SubBlockBitWriter final {
    public:
        explicit SubBlockBitWriter(FileWriter& file) : file_(file) {}

        bool Put(uint16_t code, uint8_t bits) {
            accumulator_ |= static_cast<uint32_t>(code) << bit_count_;
            bit_count_ = static_cast<uint8_t>(bit_count_ + bits);
            while (bit_count_ >= 8) {
                if (!PutByte(static_cast<uint8_t>(accumulator_ & 0xFFu))) {
                    return false;
                }
                accumulator_ >>= 8;
                bit_count_ = static_cast<uint8_t>(bit_count_ - 8);
            }
            return true;
        }

        bool Finish() {
            if (bit_count_ > 0 &&
                !PutByte(static_cast<uint8_t>(accumulator_ & 0xFFu))) {
                return false;
            }
            return Flush() && file_.WriteByte(0);
        }

        [[nodiscard]] uint64_t payload_bytes() const noexcept {
            return payload_bytes_;
        }

    private:
        bool PutByte(uint8_t value) {
            block_[block_size_++] = value;
            ++payload_bytes_;
            return block_size_ < block_.size() || Flush();
        }

        bool Flush() {
            if (block_size_ == 0) {
                return true;
            }
            const bool ok = file_.WriteByte(static_cast<uint8_t>(block_size_)) &&
                            file_.Write(std::span<const uint8_t>(block_.data(),
                                                                 block_size_));
            block_size_ = 0;
            return ok;
        }

        FileWriter& file_;
        std::array<uint8_t, 255> block_{};
        std::size_t block_size_{0};
        uint32_t accumulator_{0};
        uint8_t bit_count_{0};
        uint64_t payload_bytes_{0};
    };

    bool WriteLzw(std::span<const uint8_t> indices, std::wstring& error) {
        if (indices.empty()) {
            SetError(error, L"A GIF image rectangle is empty");
            return false;
        }
        const uint8_t table_bits = ColorTableBits(quantizer_.palette().size());
        const uint8_t minimum_code_size = (std::max)(uint8_t{2}, table_bits);
        if (!file_.WriteByte(minimum_code_size)) {
            SetError(error, L"Writing the GIF LZW code size failed");
            return false;
        }
        const uint16_t clear_code = static_cast<uint16_t>(1u << minimum_code_size);
        const uint16_t end_code = static_cast<uint16_t>(clear_code + 1u);
        uint16_t next_code = static_cast<uint16_t>(end_code + 1u);
        uint8_t code_size = static_cast<uint8_t>(minimum_code_size + 1u);
        std::unordered_map<uint32_t, uint16_t> dictionary;
        try {
            dictionary.reserve(4096);
            dictionary.max_load_factor(1.0f);
        } catch (const std::bad_alloc&) {
            SetError(error, L"Allocating the bounded GIF LZW dictionary failed");
            return false;
        }
        SubBlockBitWriter bits(file_);
        if (!bits.Put(clear_code, code_size)) {
            SetError(error, L"Writing GIF LZW data failed");
            return false;
        }
        const auto put_data_code = [&](uint16_t code) {
            if (!bits.Put(code, code_size)) {
                return false;
            }
            // The encoder's next dictionary slot is one entry ahead of the
            // decoder's. Grow only after emitting the threshold code at the old
            // width, so the decoder can add its corresponding table entry.
            if (next_code >= static_cast<uint16_t>(1u << code_size) &&
                code_size < 12) {
                ++code_size;
            }
            return true;
        };
        uint16_t prefix = indices.front();
        for (std::size_t index = 1; index < indices.size(); ++index) {
            const uint8_t suffix = indices[index];
            const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | suffix;
            const auto found = dictionary.find(key);
            if (found != dictionary.end()) {
                prefix = found->second;
                continue;
            }
            if (!put_data_code(prefix)) {
                SetError(error, L"Writing GIF LZW data failed");
                return false;
            }
            if (next_code < 4096) {
                dictionary.emplace(key, next_code++);
            } else {
                if (!bits.Put(clear_code, code_size)) {
                    SetError(error, L"Writing GIF LZW reset data failed");
                    return false;
                }
                dictionary.clear();
                next_code = static_cast<uint16_t>(end_code + 1u);
                code_size = static_cast<uint8_t>(minimum_code_size + 1u);
            }
            prefix = suffix;
        }
        if (!put_data_code(prefix) || !bits.Put(end_code, code_size) ||
            !bits.Finish()) {
            SetError(error, L"Finalizing GIF LZW data failed");
            return false;
        }
        diagnostics_.compressed_image_bytes += bits.payload_bytes();
        return true;
    }

    bool WriteFrame(const EncodedRect& frame, uint16_t delay,
                    bool duration_only, std::wstring& error) {
        const std::optional<uint8_t> transparent_index =
            duration_only && transparent_sparse_deltas_
                ? std::optional<uint8_t>{static_cast<uint8_t>(0)}
                : frame.transparent_index;
        const std::array<uint8_t, 4> graphic_control = {
            0x21, 0xF9, 0x04,
            static_cast<uint8_t>(transparent_index.has_value() ? 0x05u
                                                                : 0x04u)};
        if (!file_.Write(graphic_control) || !file_.WriteWord(delay) ||
            !file_.WriteByte(transparent_index.value_or(0)) ||
            !file_.WriteByte(0) ||
            !file_.WriteByte(0x2C)) {
            SetError(error, L"Writing GIF frame control metadata failed");
            return false;
        }
        const uint16_t left = duration_only ? 0 : frame.left;
        const uint16_t top = duration_only ? 0 : frame.top;
        const uint16_t width = duration_only ? 1 : frame.width;
        const uint16_t height = duration_only ? 1 : frame.height;
        if (!file_.WriteWord(left) || !file_.WriteWord(top) ||
            !file_.WriteWord(width) || !file_.WriteWord(height) ||
            !file_.WriteByte(0)) {
            SetError(error, L"Writing the GIF image descriptor failed");
            return false;
        }
        const uint8_t hold = transparent_index.value_or(frame.hold_index);
        const uint64_t image_data_start = file_.bytes_written();
        if (!WriteLzw(duration_only ? std::span<const uint8_t>(&hold, 1)
                                    : std::span<const uint8_t>(frame.indices),
                      error)) {
            return false;
        }
        diagnostics_.image_data_bytes +=
            file_.bytes_written() - image_data_start;
        diagnostics_.image_rectangle_pixels +=
            static_cast<uint64_t>(width) * height;
        if (duration_only) {
            ++diagnostics_.duration_only_frames;
        } else if (frame.delta) {
            diagnostics_.delta_rectangle_pixels +=
                static_cast<uint64_t>(frame.width) * frame.height;
            if (transparent_index.has_value()) {
                ++diagnostics_.transparent_delta_frames;
            }
        }
        ++diagnostics_.encoded_frames;
        return true;
    }

    [[nodiscard]] static uint64_t RoundedCentiseconds(int64_t ticks) noexcept {
        const uint64_t whole =
            static_cast<uint64_t>(ticks / kMediaTicksPerCentisecond);
        const uint64_t remainder =
            static_cast<uint64_t>(ticks % kMediaTicksPerCentisecond);
        return whole +
               (remainder >=
                        static_cast<uint64_t>(kMediaTicksPerCentisecond / 2)
                    ? 1u
                    : 0u);
    }

    [[nodiscard]] bool ShouldReserveCentisecondForNext(
        MediaTime next_duration) const noexcept {
        if (!pending_.has_value() || next_duration <= MediaTime::zero() ||
            pending_->duration.count() <= 0 ||
            cumulative_ticks_ >
                (std::numeric_limits<int64_t>::max)() -
                    pending_->duration.count()) {
            return false;
        }
        const int64_t pending_ticks =
            cumulative_ticks_ + pending_->duration.count();
        if (pending_ticks > (std::numeric_limits<int64_t>::max)() -
                                next_duration.count()) {
            return false;
        }
        const uint64_t pending_target = RoundedCentiseconds(pending_ticks);
        const uint64_t combined_target =
            RoundedCentiseconds(pending_ticks + next_duration.count());
        return combined_target == pending_target &&
               pending_target >= emitted_centiseconds_ + 2u;
    }

    bool FlushPending(std::wstring& error,
                      uint64_t reserved_centiseconds = 0) {
        const int64_t duration = pending_->duration.count();
        if (duration <= 0 ||
            cumulative_ticks_ > (std::numeric_limits<int64_t>::max)() - duration) {
            SetError(error, L"A GIF frame has an invalid or overflowing duration");
            return false;
        }
        cumulative_ticks_ += duration;
        uint64_t desired_centiseconds =
            RoundedCentiseconds(cumulative_ticks_);
        if (desired_centiseconds <= emitted_centiseconds_) {
            if (diagnostics_.encoded_frames == 0) {
                // GIF has no useful zero-delay representation. A lone visible
                // fragment is displayed for the smallest deterministic delay.
                desired_centiseconds = 1;
            } else {
                // This frame contributes no displayable time after cumulative
                // rounding. Omitting it preserves the exact GIF-time-base
                // duration instead of rejecting an otherwise valid interval.
                pending_.reset();
                diagnostics_.encoded_duration_centiseconds =
                    emitted_centiseconds_;
                return true;
            }
        }
        if (reserved_centiseconds >=
            desired_centiseconds - emitted_centiseconds_) {
            SetError(error,
                     L"The GIF cadence could not reserve a final display interval");
            return false;
        }
        uint64_t remaining = desired_centiseconds - emitted_centiseconds_ -
                             reserved_centiseconds;
        bool duration_only = false;
        while (remaining > 0) {
            const uint16_t delay = static_cast<uint16_t>(
                (std::min)(remaining, static_cast<uint64_t>(kMaximumGifDelay)));
            if (!WriteFrame(*pending_, delay, duration_only, error)) {
                return false;
            }
            emitted_centiseconds_ += delay;
            remaining -= delay;
            duration_only = true;
        }
        if (pending_->delta) {
            ++diagnostics_.delta_frames;
        } else {
            ++diagnostics_.full_frames;
        }
        pending_.reset();
        diagnostics_.encoded_duration_centiseconds = emitted_centiseconds_;
        return true;
    }

    void UpdatePeak(std::size_t transient_capacity) noexcept {
        const uint64_t owned =
            static_cast<uint64_t>(previous_canvas_.capacity()) +
            static_cast<uint64_t>(stable_pixels_.pixels.capacity()) * sizeof(uint32_t) +
            static_cast<uint64_t>(transient_capacity) +
            (pending_.has_value()
                 ? static_cast<uint64_t>(pending_->indices.capacity())
                 : 0u) +
            sizeof(PaletteQuantizer) + 4096u * 64u + scaling_working_bytes_;
        diagnostics_.peak_working_bytes =
            (std::max)(diagnostics_.peak_working_bytes, owned);
    }

    FileWriter& file_;
    uint16_t width_{0};
    uint16_t height_{0};
    const PaletteQuantizer& quantizer_;
    bool transparent_sparse_deltas_{false};
    uint8_t temporal_tolerance_{0};
    platform::BgraImage stable_pixels_;
    uint64_t scaling_working_bytes_{0};
    GifEncodeDiagnostics& diagnostics_;
    std::vector<uint8_t> previous_canvas_;
    std::optional<EncodedRect> pending_;
    int64_t cumulative_ticks_{0};
    uint64_t emitted_centiseconds_{0};
    bool opened_{false};
};

struct VisitCounters final {
    uint64_t source_frames{0};
    uint64_t adapted_frames{0};
    uint64_t cadence_coalesced{0};
};

template <typename Callback>
bool VisitAdaptedFrames(IGifFrameSource& source, uint32_t output_width,
                        uint32_t output_height,
                        const GifEncodeOptions& options, float progress_begin,
                        float progress_end, const GifProgressCallback& progress,
                        Callback&& callback, VisitCounters& counters,
                        std::wstring& error) {
    struct CancellationScope final {
        IGifFrameSource& source;
        ~CancellationScope() { source.SetCancellationProbe({}); }
    } cancellation_scope{source};
    source.SetCancellationProbe([&progress, progress_begin, &error] {
        return ReportProgress(progress, progress_begin, error);
    });
    if (!source.Reset()) {
        error = source.last_error();
        if (error.empty()) {
            SetError(error, L"Resetting the GIF frame source failed");
        }
        return false;
    }
    const int64_t minimum_duration =
        (kMediaTicksPerSecond + options.maximum_frame_rate - 1u) /
        options.maximum_frame_rate;
    std::optional<VideoFrame> pending;
    std::shared_ptr<const platform::BgraImage> last_group_image;
    uint64_t group_frames = 0;
    bool first_group = true;
    bool distributed_group_ready = false;
    int64_t cumulative_source_ticks = 0;
    int64_t last_timestamp = -1;

    const auto emit = [&](VideoFrame frame) {
        if (!callback(frame)) {
            if (error.empty()) {
                SetError(error, L"Processing a GIF frame failed");
            }
            return false;
        }
        ++counters.adapted_frames;
        return true;
    };

    for (;;) {
        VideoFrame frame;
        const GifFrameReadResult result = source.Read(frame);
        if (result == GifFrameReadResult::Error) {
            error = source.last_error();
            if (error.empty()) {
                SetError(error, L"Reading the GIF frame source failed");
            }
            return false;
        }
        if (result == GifFrameReadResult::End) {
            break;
        }
        ++counters.source_frames;
        if (frame.image == nullptr || frame.image->empty() ||
            frame.image->width != static_cast<int>(source.width()) ||
            frame.image->height != static_cast<int>(source.height()) ||
            frame.timestamp.count() < 0 || frame.duration <= MediaTime::zero() ||
            frame.timestamp.count() < last_timestamp) {
            SetError(error, L"The GIF source returned an invalid video frame");
            return false;
        }
        last_timestamp = frame.timestamp.count();
        frame.image = ScaleFrame(frame.image, output_width, output_height, error);
        if (frame.image == nullptr) {
            return false;
        }

        if (options.distributed_cadence && pending.has_value() &&
            distributed_group_ready) {
            if (!emit(*pending)) {
                return false;
            }
            first_group = false;
            pending = frame;
            last_group_image = frame.image;
            group_frames = 1;
        } else if (!pending.has_value()) {
            pending = frame;
            last_group_image = frame.image;
            group_frames = 1;
        } else if (options.distributed_cadence ||
                   pending->duration.count() < minimum_duration) {
            if (pending->duration.count() >
                (std::numeric_limits<int64_t>::max)() - frame.duration.count()) {
                SetError(error, L"The adapted GIF frame duration overflowed");
                return false;
            }
            pending->duration += frame.duration;
            last_group_image = frame.image;
            ++group_frames;
            ++counters.cadence_coalesced;
        } else {
            if (!emit(*pending)) {
                return false;
            }
            first_group = false;
            pending = frame;
            last_group_image = frame.image;
            group_frames = 1;
        }

        if (options.distributed_cadence) {
            if (cumulative_source_ticks >
                (std::numeric_limits<int64_t>::max)() -
                    frame.duration.count()) {
                SetError(error, L"The distributed GIF cadence overflowed");
                return false;
            }
            cumulative_source_ticks += frame.duration.count();
            const uint64_t cadence_slot = counters.adapted_frames + 1u;
            const int64_t frame_rate =
                static_cast<int64_t>(options.maximum_frame_rate);
            const uint64_t maximum_slot = static_cast<uint64_t>(
                ((std::numeric_limits<int64_t>::max)() - frame_rate + 1) /
                kMediaTicksPerSecond);
            if (cadence_slot > maximum_slot) {
                SetError(error, L"The distributed GIF cadence overflowed");
                return false;
            }
            const int64_t cadence_threshold =
                (static_cast<int64_t>(cadence_slot) *
                     kMediaTicksPerSecond +
                 frame_rate - 1) /
                frame_rate;
            distributed_group_ready =
                cumulative_source_ticks >= cadence_threshold;
        }

        const int64_t duration_ticks = (std::max)(int64_t{1}, source.duration().count());
        const int64_t frame_end =
            frame.timestamp.count() <=
                    (std::numeric_limits<int64_t>::max)() - frame.duration.count()
                ? frame.timestamp.count() + frame.duration.count()
                : duration_ticks;
        const float fraction = static_cast<float>((std::min)(frame_end, duration_ticks)) /
                               static_cast<float>(duration_ticks);
        if (!ReportProgress(progress,
                            progress_begin +
                                (progress_end - progress_begin) * fraction,
                            error)) {
            return false;
        }
    }

    if (!pending.has_value()) {
        SetError(error, L"The GIF source contains no video frames");
        return false;
    }
    if (group_frames > 1 && !ImagesEqual(pending->image, last_group_image)) {
        // Retain the true final visual even when the cadence cap combined the
        // final group. One centisecond is borrowed from that group's duration.
        if (pending->duration.count() >= 2 * kMediaTicksPerCentisecond) {
            pending->duration -= MediaTime{kMediaTicksPerCentisecond};
            if (!emit(*pending)) {
                return false;
            }
            VideoFrame final_frame = *pending;
            final_frame.image = last_group_image;
            final_frame.timestamp += pending->duration;
            final_frame.duration = MediaTime{kMediaTicksPerCentisecond};
            if (!emit(std::move(final_frame))) {
                return false;
            }
        } else if (first_group) {
            // The GIF time base cannot display two distinct sub-centisecond
            // visuals without lengthening the animation; preserve the first.
            if (!emit(*pending)) {
                return false;
            }
        } else {
            pending->image = last_group_image;
            if (!emit(*pending)) {
                return false;
            }
        }
    } else if (!emit(*pending)) {
        return false;
    }
    return true;
}

}  // namespace

bool ExportGif(IGifFrameSource& source, const std::wstring& destination_path,
               const GifEncodeOptions& options, GifProgressCallback progress,
               bool replace_existing, GifEncodeDiagnostics& diagnostics,
               std::wstring& error, TrimExportFailure* failure) {
    diagnostics = {};
    error.clear();
    if (failure != nullptr) {
        *failure = {};
    }
    if (!ValidateDestination(destination_path, replace_existing, error)) {
        RecordFailure(failure, TrimExportFailureStage::StagingWrite, error);
        return false;
    }
    if (source.duration() <= MediaTime::zero()) {
        SetError(error, L"The GIF source duration is invalid");
        RecordFailure(failure, TrimExportFailureStage::SourceValidation, error);
        return false;
    }
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    if (!ResolveDimensions(source.width(), source.height(), options, output_width,
                           output_height, error) ||
        !ReportProgress(progress, 0.0f, error)) {
        RecordFailure(failure, TrimExportFailureStage::SourceValidation, error);
        return false;
    }

    try {
        // During scaling, pending, last-in-group and current can own distinct
        // adapted BGRA images. Source-owned decode buffers remain excluded.
        const uint64_t scaling_working_bytes =
            output_width != source.width() || output_height != source.height()
                ? static_cast<uint64_t>(output_width) * output_height * 12u
                : 0u;
        std::vector<Rgb> palette;
        float encoding_progress_begin = 0.0f;
        if (options.palette == GifPalettePolicy::FixedRgb332) {
            palette = BuildRgb332Palette();
        } else if (options.palette == GifPalettePolicy::FixedWebSafe) {
            palette = BuildWebSafePalette();
        } else {
            auto histogram = std::make_unique<PaletteHistogram>();
            VisitCounters analysis_counters;
            if (!VisitAdaptedFrames(
                    source, output_width, output_height, options, 0.0f, 0.45f,
                    progress,
                    [&histogram](const VideoFrame& frame) {
                        histogram->Add(*frame.image);
                        return true;
                    },
                    analysis_counters, error)) {
                RecordSourceFailure(failure, source,
                                    TrimExportFailureStage::VideoDecode, error);
                return false;
            }
            if (!histogram->Build(options.maximum_palette_colors, palette, error)) {
                RecordFailure(failure, TrimExportFailureStage::PaletteQuantization,
                              error);
                return false;
            }
            diagnostics.palette_samples = histogram->samples();
            diagnostics.peak_working_bytes =
                static_cast<uint64_t>(sizeof(PaletteHistogram)) +
                scaling_working_bytes +
                static_cast<uint64_t>(kHistogramBinCount) * sizeof(PaletteBin);
            encoding_progress_begin = 0.45f;
        }
        diagnostics.palette_entries = palette.size();

        PaletteQuantizer quantizer;
        if (!quantizer.Initialize(std::move(palette), options.quantization, error)) {
            RecordFailure(failure, TrimExportFailureStage::PaletteQuantization,
                          error);
            return false;
        }
        const auto staging_path = MakeStagingPath(destination_path, error);
        if (!staging_path.has_value()) {
            RecordFailure(failure, TrimExportFailureStage::StagingWrite, error);
            return false;
        }
        IncompleteFile incomplete(*staging_path);
        FileWriter file(*staging_path);
        if (!file.valid()) {
            SetError(error, L"Creating the GIF staging output failed");
            RecordFailure(failure, TrimExportFailureStage::StagingWrite, error);
            return false;
        }
        GifStreamWriter writer(file, static_cast<uint16_t>(output_width),
                               static_cast<uint16_t>(output_height), quantizer,
                               options.transparent_sparse_deltas,
                               options.temporal_tolerance,
                               scaling_working_bytes, diagnostics);
        if (!writer.Open(options.loop_count, error)) {
            RecordFailure(failure, TrimExportFailureStage::GifEncoding, error);
            return false;
        }

        VisitCounters encoding_counters;
        const bool encoded = VisitAdaptedFrames(
            source, output_width, output_height, options, encoding_progress_begin,
            0.98f, progress,
            [&writer, &error](const VideoFrame& frame) {
                return writer.Add(*frame.image, frame.duration, error);
            },
            encoding_counters, error);
        if (!encoded) {
            RecordSourceFailure(failure, source,
                                TrimExportFailureStage::GifEncoding, error);
            return false;
        }
        if (!writer.Finalize(error) || !file.FlushAndClose()) {
            if (error.empty()) {
                SetError(error, L"Finalizing the GIF staging output failed");
            }
            RecordFailure(failure, TrimExportFailureStage::StagingWrite, error);
            return false;
        }
        diagnostics.source_frames_read = encoding_counters.source_frames;
        diagnostics.adapted_frames = encoding_counters.adapted_frames;
        diagnostics.unchanged_frames_coalesced +=
            encoding_counters.cadence_coalesced;
        if (!ReportProgress(progress, 1.0f, error)) {
            return false;
        }
        const DWORD flags = MOVEFILE_WRITE_THROUGH |
                            (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0);
        if (::MoveFileExW(staging_path->c_str(), destination_path.c_str(), flags) ==
            FALSE) {
            const HRESULT result = HRESULT_FROM_WIN32(::GetLastError());
            SetError(error, L"Publishing the finalized GIF destination failed");
            RecordFailure(failure, TrimExportFailureStage::Publication, error);
            if (failure != nullptr) {
                failure->has_hresult = true;
                failure->hresult = static_cast<int32_t>(result);
            }
            return false;
        }
        incomplete.Disarm();
        return true;
    } catch (const std::bad_alloc&) {
        SetError(error, L"Allocating bounded GIF encoder state failed");
    } catch (...) {
        SetError(error, L"An unexpected GIF encoding error occurred");
    }
    RecordFailure(failure, TrimExportFailureStage::GifEncoding, error);
    return false;
}

}  // namespace easnap::media
