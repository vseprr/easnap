// Easnap tests - the generated application for Scrollshot tests.
//
// Layout and rasterisation run once per application. Text goes through
// DirectWrite with ClearType and fixed rendering parameters on a WARP device,
// so the pixels do not depend on the GPU or on the machine's ClearType
// settings. Every block of the page is rendered on its own at its own origin
// and copied into the content layer at a whole-pixel row; pictures,
// placeholders, the caret, the spinners and the scrollbar are computed on the
// CPU. See docs/decisions/ADR-041-scrollshot-realistic-test-application.md.
#include "scrollshot_realistic_app.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <functional>
#include <utility>

namespace easnap_test::realistic {
namespace {

using Microsoft::WRL::ComPtr;

/// Text is Turkish-like; the locale is fixed so the result does not depend
/// on the machine's regional format.
constexpr wchar_t kLocale[] = L"tr-tr";
constexpr const wchar_t* kFamilies[] = {L"Segoe UI", L"Consolas", L"Times New Roman", L"Georgia", L"Arial"};

constexpr uint32_t Rgb(int r, int g, int b) {
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

constexpr uint32_t kPage = Rgb(255, 255, 255);
constexpr uint32_t kBlack = Rgb(0, 0, 0);
constexpr uint32_t kWhite = Rgb(255, 255, 255);
constexpr uint32_t kInk = Rgb(31, 35, 40);
constexpr uint32_t kMuted = Rgb(94, 102, 112);
constexpr uint32_t kLink = Rgb(11, 92, 173);
constexpr uint32_t kKeyword = Rgb(130, 80, 223);
constexpr uint32_t kComment = Rgb(26, 127, 55);
constexpr uint32_t kRule = Rgb(222, 226, 230);
constexpr uint32_t kCodeBack = Rgb(246, 248, 250);
constexpr uint32_t kCodeBorder = Rgb(208, 215, 222);
constexpr uint32_t kQuoteBar = Rgb(196, 204, 214);
constexpr uint32_t kCardBorder = Rgb(216, 222, 228);
constexpr uint32_t kCommentBorder = Rgb(80, 140, 220);
constexpr uint32_t kStatusBack = Rgb(255, 248, 225);
constexpr uint32_t kStatusBorder = Rgb(240, 200, 120);
constexpr uint32_t kSectionBack = Rgb(244, 246, 249);
constexpr uint32_t kBarBack = Rgb(36, 41, 47);
constexpr uint32_t kBarText = Rgb(240, 246, 252);
constexpr uint32_t kBarField = Rgb(56, 62, 70);
constexpr uint32_t kBarHint = Rgb(150, 160, 170);
constexpr uint32_t kBarIcon = Rgb(110, 118, 129);
constexpr uint32_t kPlaceholder = Rgb(234, 237, 240);
constexpr uint32_t kPlaceholderMark = Rgb(204, 209, 214);
constexpr uint32_t kSolidBackground = Rgb(243, 246, 250);

[[nodiscard]] D2D1_COLOR_F ToColor(uint32_t color) {
    return D2D1::ColorF(static_cast<float>((color >> 16) & 0xFFu) / 255.0f,
                        static_cast<float>((color >> 8) & 0xFFu) / 255.0f, static_cast<float>(color & 0xFFu) / 255.0f,
                        1.0f);
}

[[nodiscard]] int Channel(uint32_t color, int shift) { return static_cast<int>((color >> shift) & 0xFFu); }

[[nodiscard]] uint32_t Mix(uint32_t from, uint32_t to, double amount) {
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        const double value = Channel(from, shift) + (Channel(to, shift) - Channel(from, shift)) * amount;
        out |= static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255)) << shift;
    }
    return out;
}

[[nodiscard]] uint32_t Hash(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = (a * 0x9E3779B1u) ^ ((b + 0x7F4A7C15u) * 0x85EBCA77u) ^ (c * 0xC2B2AE3Du);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

void FillBox(Pixels& pixels, const Box& box, uint32_t color) {
    for (int y = std::max(0, box.top); y < std::min(pixels.height, box.bottom); ++y) {
        uint32_t* row = pixels.Row(y);
        for (int x = std::max(0, box.left); x < std::min(pixels.width, box.right); ++x) { row[x] = color; }
    }
}

[[nodiscard]] Box Shifted(const Box& box, int dx, int dy) {
    return Box{box.left + dx, box.top + dy, box.right + dx, box.bottom + dy};
}

// --- Generated text -------------------------------------------------------------

constexpr const wchar_t* kSyllables[] = {
    L"ka", L"le", L"mi", L"nu", L"ra", L"so", L"te", L"vi", L"ya", L"zo", L"ba", L"de", L"fo", L"gi",
    L"ha", L"je", L"ko", L"la", L"me", L"ne", L"pa", L"re", L"tu", L"ve", L"an", L"el", L"in", L"ol",
    L"ur", L"ek", L"ğa", L"şe", L"ır", L"ın", L"lı", L"tı", L"şi", L"ğu",
    L"sı", L"zı", L"yü", L"çe", L"ön", L"gü", L"şa", L"dağ"};
constexpr int kAsciiSyllables = 30;  ///< The first syllables are plain ASCII, for code.

[[nodiscard]] wchar_t Capital(wchar_t letter) {
    switch (letter) {
        case L'i': return L'İ';
        case L'ı': return L'I';
        case L'ş': return L'Ş';
        case L'ğ': return L'Ğ';
        case L'ç': return L'Ç';
        case L'ö': return L'Ö';
        case L'ü': return L'Ü';
        default: return static_cast<wchar_t>(std::towupper(letter));
    }
}

[[nodiscard]] std::wstring Word(Random& random, bool ascii = false) {
    const int count = random.Chance(30) ? 1 : (random.Chance(70) ? 2 : random.Range(3, 4));
    std::wstring word;
    for (int i = 0; i < count; ++i) {
        const int limit = ascii ? kAsciiSyllables : static_cast<int>(std::size(kSyllables));
        word += kSyllables[random.Range(0, limit - 1)];
    }
    return word;
}

[[nodiscard]] std::wstring Capitalized(std::wstring word) {
    if (!word.empty()) { word[0] = Capital(word[0]); }
    return word;
}

struct WordSpan {
    UINT32 start;
    UINT32 length;
};

/// Sentences of generated words; `spans` receives every word's position.
[[nodiscard]] std::wstring Sentences(Random& random, int sentences, int min_words, int max_words,
                                     std::vector<WordSpan>* spans = nullptr) {
    std::wstring text;
    for (int s = 0; s < sentences; ++s) {
        if (!text.empty()) { text += L' '; }
        const int words = random.Range(min_words, max_words);
        for (int w = 0; w < words; ++w) {
            if (w > 0) { text += (w > 2 && random.Chance(8)) ? L", " : L" "; }
            std::wstring word = Word(random);
            if (w == 0) { word = Capitalized(word); }
            if (spans != nullptr) { spans->push_back(WordSpan{static_cast<UINT32>(text.size()), static_cast<UINT32>(word.size())}); }
            text += word;
        }
        text += random.Chance(12) ? L'?' : L'.';
    }
    return text;
}

[[nodiscard]] std::wstring Title(Random& random, int min_words, int max_words) {
    std::wstring text;
    const int words = random.Range(min_words, max_words);
    for (int w = 0; w < words; ++w) {
        if (w > 0) { text += L' '; }
        text += w == 0 || random.Chance(40) ? Capitalized(Word(random)) : Word(random);
    }
    return text;
}

// --- Direct2D on WARP -------------------------------------------------------------

struct Font {
    const wchar_t* family;
    double points;
    DWRITE_FONT_WEIGHT weight{DWRITE_FONT_WEIGHT_NORMAL};
    DWRITE_FONT_STYLE style{DWRITE_FONT_STYLE_NORMAL};
};

class Raster {
public:
    bool Create(std::wstring& error) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
        if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d_, nullptr,
                                       nullptr))) {
            error = L"the WARP Direct3D device is not available";
            return false;
        }
        D2D1_FACTORY_OPTIONS factory_options{};
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &factory_options,
                                       reinterpret_cast<void**>(factory_.GetAddressOf()))) ||
            FAILED(d3d_.As(&dxgi)) || FAILED(factory_->CreateDevice(dxgi.Get(), &device_)) ||
            FAILED(device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_))) {
            error = L"Direct2D is not available on the WARP device";
            return false;
        }
        if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
            error = L"DirectWrite is not available";
            return false;
        }
        ComPtr<IDWriteFontCollection> fonts;
        if (FAILED(dwrite_->GetSystemFontCollection(&fonts, FALSE))) {
            error = L"the system font collection cannot be read";
            return false;
        }
        for (const wchar_t* family : kFamilies) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (FAILED(fonts->FindFamilyName(family, &index, &exists)) || exists == FALSE) {
                error = std::wstring(L"the font \"") + family + L"\" is not installed";
                return false;
            }
        }
        // Fixed ClearType parameters: the pixels must not depend on the
        // machine's ClearType tuning.
        if (FAILED(dwrite_->CreateCustomRenderingParams(1.8f, 0.5f, 1.0f, DWRITE_PIXEL_GEOMETRY_RGB,
                                                        DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &params_))) {
            error = L"DirectWrite rendering parameters cannot be created";
            return false;
        }
        context_->SetDpi(96.0f, 96.0f);  // One DIP is one device pixel; sizes are scaled explicitly.
        context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        context_->SetTextRenderingParams(params_.Get());
        return true;
    }

    [[nodiscard]] ID2D1DeviceContext* context() const { return context_.Get(); }

    [[nodiscard]] ComPtr<IDWriteTextLayout> Layout(const std::wstring& text, const Font& font, double scale,
                                                   float max_width, bool wrap = true,
                                                   DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        ComPtr<IDWriteTextFormat> format;
        const float size = static_cast<float>(font.points * 96.0 / 72.0 * scale);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_->CreateTextFormat(font.family, nullptr, font.weight, font.style, DWRITE_FONT_STRETCH_NORMAL,
                                             size, kLocale, &format))) {
            return layout;
        }
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(alignment);
        if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), max_width,
                                             100000.0f, &layout))) {
            layout.Reset();
        }
        return layout;
    }

    /// A brush of the device context, created once per colour.
    [[nodiscard]] ID2D1SolidColorBrush* Brush(uint32_t color) {
        for (const auto& [key, brush] : brushes_) {
            if (key == color) { return brush.Get(); }
        }
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(context_->CreateSolidColorBrush(ToColor(color), &brush))) { return nullptr; }
        brushes_.emplace_back(color, brush);
        return brush.Get();
    }

    /// Draws into an opaque `width` x `height` target cleared to `clear` and
    /// reads the pixels back.
    [[nodiscard]] bool Render(int width, int height, uint32_t clear,
                              const std::function<void(ID2D1DeviceContext*)>& draw, Pixels& out) {
        if (width <= 0 || height <= 0 || !EnsureTarget(width, height)) { return false; }
        context_->SetTarget(target_.Get());
        context_->BeginDraw();
        context_->SetTransform(D2D1::Matrix3x2F::Identity());
        context_->Clear(ToColor(clear));
        context_->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
                                      D2D1_ANTIALIAS_MODE_ALIASED);
        draw(context_.Get());
        context_->PopAxisAlignedClip();
        context_->SetTransform(D2D1::Matrix3x2F::Identity());
        const HRESULT drawn = context_->EndDraw();
        context_->SetTarget(nullptr);
        if (FAILED(drawn)) { return false; }
        const D2D1_POINT_2U origin{0, 0};
        const D2D1_RECT_U source{0, 0, static_cast<UINT32>(width), static_cast<UINT32>(height)};
        if (FAILED(readback_->CopyFromBitmap(&origin, target_.Get(), &source))) { return false; }
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(readback_->Map(D2D1_MAP_OPTIONS_READ, &mapped))) { return false; }
        out.Reset(width, height, 0);
        for (int y = 0; y < height; ++y) {
            const auto* source_row = reinterpret_cast<const uint32_t*>(mapped.bits + static_cast<size_t>(y) * mapped.pitch);
            uint32_t* target_row = out.Row(y);
            for (int x = 0; x < width; ++x) { target_row[x] = source_row[x] | 0xFF000000u; }
        }
        readback_->Unmap();
        return true;
    }

private:
    bool EnsureTarget(int width, int height) {
        if (target_ != nullptr && width <= target_width_ && height <= target_height_) { return true; }
        const int w = std::max(width, target_width_), h = std::max(height, target_height_);
        target_.Reset();
        readback_.Reset();
        const D2D1_PIXEL_FORMAT format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
        const D2D1_BITMAP_PROPERTIES1 target = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96.0f, 96.0f);
        const D2D1_BITMAP_PROPERTIES1 readback = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96.0f, 96.0f);
        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h));
        if (FAILED(context_->CreateBitmap(size, nullptr, 0, &target, &target_)) ||
            FAILED(context_->CreateBitmap(size, nullptr, 0, &readback, &readback_))) {
            target_.Reset();
            readback_.Reset();
            return false;
        }
        target_width_ = w;
        target_height_ = h;
        return true;
    }

    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1Device> device_;
    ComPtr<ID2D1DeviceContext> context_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteRenderingParams> params_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1Bitmap1> readback_;
    int target_width_{0};
    int target_height_{0};
    std::vector<std::pair<uint32_t, ComPtr<ID2D1SolidColorBrush>>> brushes_;
};

// --- Blocks of the page ------------------------------------------------------------

struct Shape {
    enum Kind : uint8_t { Fill,
                          RoundFill,
                          RoundStroke,
                          Ellipse };
    Kind kind{Fill};
    D2D1_RECT_F rect{};
    float radius{0};
    float stroke{1};
    uint32_t color{kInk};
};

struct PlacedText {
    ComPtr<IDWriteTextLayout> layout;
    float x{0};
    float y{0};
    uint32_t color{kInk};
};

struct Picture {
    Box box;  ///< Block coordinates.
    uint32_t seed{0};
};

struct Block {
    int top{0};
    int height{0};
    std::vector<Shape> shapes;
    std::vector<PlacedText> texts;
    std::vector<Picture> pictures;
};

void DrawBlock(Raster& raster, const Block& block, float dy) {
    ID2D1DeviceContext* context = raster.context();
    context->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, dy));
    for (const Shape& shape : block.shapes) {
        ID2D1SolidColorBrush* brush = raster.Brush(shape.color);
        if (brush == nullptr) { continue; }
        switch (shape.kind) {
            case Shape::Fill: context->FillRectangle(shape.rect, brush); break;
            case Shape::RoundFill:
                context->FillRoundedRectangle(D2D1::RoundedRect(shape.rect, shape.radius, shape.radius), brush);
                break;
            case Shape::RoundStroke:
                context->DrawRoundedRectangle(D2D1::RoundedRect(shape.rect, shape.radius, shape.radius), brush,
                                              shape.stroke);
                break;
            case Shape::Ellipse: {
                const float rx = (shape.rect.right - shape.rect.left) / 2.0f;
                const float ry = (shape.rect.bottom - shape.rect.top) / 2.0f;
                context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(shape.rect.left + rx, shape.rect.top + ry), rx, ry),
                                     brush);
                break;
            }
        }
    }
    for (const PlacedText& text : block.texts) {
        ID2D1SolidColorBrush* brush = raster.Brush(text.color);
        if (brush != nullptr && text.layout != nullptr) {
            context->DrawTextLayout(D2D1::Point2F(text.x, text.y), text.layout.Get(), brush,
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
    context->SetTransform(D2D1::Matrix3x2F::Identity());
}

/// A photo-like picture: sky, sun, two ridges and grain, from a seed.
/// Each pixel depends only on its position inside `box`, so any clipped part
/// is the same as the corresponding part of the whole.
void PaintPicture(Pixels& target, const Box& box, uint32_t seed) {
    Random random(seed);
    const int w = box.right - box.left, h = box.bottom - box.top;
    if (w <= 0 || h <= 0) { return; }
    const int sky_top[3] = {random.Range(60, 110), random.Range(110, 160), random.Range(190, 235)};
    const int horizon[3] = {random.Range(225, 250), random.Range(196, 226), random.Range(160, 205)};
    const double sun_x = random.Range(w / 5, 4 * w / 5), sun_y = random.Range(h / 8, h / 3);
    const double sun_r = random.Range(h / 14, h / 8) + 2.0;
    const double far_base = h * (0.48 + random.Range(0, 8) / 100.0);
    const double near_base = h * (0.68 + random.Range(0, 8) / 100.0);
    constexpr double kPi = 3.14159265358979323846;
    const double f1 = (2 + random.Range(0, 3)) * kPi / w, f2 = (5 + random.Range(0, 5)) * kPi / w;
    const double f3 = (3 + random.Range(0, 4)) * kPi / w;
    const double p1 = random.Range(0, 628) / 100.0, p2 = random.Range(0, 628) / 100.0, p3 = random.Range(0, 628) / 100.0;
    const int far_tint[3] = {random.Range(90, 120), random.Range(105, 135), random.Range(140, 175)};
    const int near_tint[3] = {random.Range(40, 70), random.Range(85, 120), random.Range(45, 75)};
    for (int y = std::max(0, box.top); y < std::min(target.height, box.bottom); ++y) {
        const int ly = y - box.top;
        uint32_t* row = target.Row(y);
        for (int x = std::max(0, box.left); x < std::min(target.width, box.right); ++x) {
            const int lx = x - box.left;
            const double t = static_cast<double>(ly) / h;
            double colour[3];
            for (int c = 0; c < 3; ++c) { colour[c] = sky_top[c] + (horizon[c] - sky_top[c]) * t; }
            const double distance = std::hypot(lx - sun_x, ly - sun_y);
            if (distance < sun_r + 1.5) {
                const double amount = std::clamp((sun_r + 1.5 - distance) / 1.5, 0.0, 1.0);
                const double sun[3] = {255.0, 240.0, 200.0};
                for (int c = 0; c < 3; ++c) { colour[c] += (sun[c] - colour[c]) * amount; }
            }
            const double far_ridge = far_base + h * 0.07 * std::sin(lx * f1 + p1) + h * 0.03 * std::sin(lx * f2 + p2);
            if (ly > far_ridge) {
                const double shade = 1.0 - 0.35 * (ly - far_ridge) / h;
                for (int c = 0; c < 3; ++c) { colour[c] = far_tint[c] * shade + 20.0; }
            }
            const double near_ridge = near_base + h * 0.06 * std::sin(lx * f3 + p3) + h * 0.02 * std::sin(lx * f2 * 1.7 + p1);
            if (ly > near_ridge) {
                const double shade = 1.0 + 0.6 * (ly - near_ridge) / h;
                for (int c = 0; c < 3; ++c) { colour[c] = near_tint[c] * shade; }
            }
            const int grain = static_cast<int>(Hash(static_cast<uint32_t>(lx), static_cast<uint32_t>(ly), seed) % 11u) - 5;
            const auto channel = [&](int c) {
                return static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(colour[c])) + grain, 0, 255));
            };
            row[x] = 0xFF000000u | (channel(0) << 16) | (channel(1) << 8) | channel(2);
        }
    }
}

/// Eight dots on a circle; the darkest moves on by one every frame.
void DrawSpinner(Pixels& target, const Box& box, uint64_t frame, uint32_t background, bool light) {
    FillBox(target, box, background);
    const int size = std::min(box.right - box.left, box.bottom - box.top);
    const int dot = std::max(2, size / 7);
    const double radius = (size - dot) / 2.0;
    const double cx = (box.left + box.right) / 2.0, cy = (box.top + box.bottom) / 2.0;
    const int head = static_cast<int>(frame % 8u);
    for (int k = 0; k < 8; ++k) {
        const double angle = k * 3.14159265358979323846 / 4.0;
        const int x = static_cast<int>(std::lround(cx + radius * std::cos(angle) - dot / 2.0));
        const int y = static_cast<int>(std::lround(cy + radius * std::sin(angle) - dot / 2.0));
        const int age = (k - head + 8) % 8;
        const int level = light ? 240 - age * 22 : 50 + age * 24;
        FillBox(target, Box{x, y, x + dot, y + dot}, Rgb(level, level, std::min(255, level + 8)));
    }
}

// --- Layout ---------------------------------------------------------------------------

class Builder {
public:
    Builder(Raster& raster, const AppOptions& options) : raster_(raster), options_(options), random_(options.seed) {
        left_ = Rows(32);
        right_ = options.width - Rows(32);
    }

    [[nodiscard]] int Rows(double dips) const { return static_cast<int>(std::lround(dips * options_.scale)); }
    [[nodiscard]] float Px(double dips) const { return static_cast<float>(Rows(dips)); }
    [[nodiscard]] float Column() const { return static_cast<float>(right_ - left_); }
    Random& random() { return random_; }

    [[nodiscard]] int TextHeight(const PlacedText& text) const {
        DWRITE_TEXT_METRICS metrics{};
        if (text.layout == nullptr || FAILED(text.layout->GetMetrics(&metrics))) { return 0; }
        return static_cast<int>(std::ceil(metrics.height));
    }

    PlacedText Text(const std::wstring& content, const Font& font, float x, float y, float width, uint32_t color,
                    bool wrap = true, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        PlacedText text;
        text.layout = raster_.Layout(content, font, options_.scale, width, wrap, alignment);
        text.x = x;
        text.y = y;
        text.color = color;
        if (text.layout == nullptr) { failed_ = true; }
        return text;
    }

    void Colour(PlacedText& text, WordSpan span, uint32_t color) {
        if (text.layout != nullptr) {
            text.layout->SetDrawingEffect(raster_.Brush(color), DWRITE_TEXT_RANGE{span.start, span.length});
        }
    }

    Block Heading(int level) {
        Block block;
        const Font font = level == 1   ? Font{L"Georgia", 24.0, DWRITE_FONT_WEIGHT_BOLD}
                          : level == 2 ? Font{L"Segoe UI", 17.0, DWRITE_FONT_WEIGHT_SEMI_BOLD}
                                       : Font{L"Arial", 13.0, DWRITE_FONT_WEIGHT_BOLD};
        PlacedText text = Text(Title(random_, 2, level == 1 ? 5 : 6), font, static_cast<float>(left_), Px(10), Column(), kInk);
        const int height = TextHeight(text);
        block.texts.push_back(std::move(text));
        block.height = Rows(10) + height + Rows(6);
        if (level == 2) {
            block.shapes.push_back(Shape{Shape::Fill, D2D1::RectF(static_cast<float>(left_), static_cast<float>(block.height), static_cast<float>(right_), static_cast<float>(block.height + 1)),
                                         0, 1, kRule});
            block.height += 1 + Rows(4);
        }
        return block;
    }

    Block Paragraph() {
        Block block;
        std::vector<WordSpan> spans;
        const std::wstring content = Sentences(random_, random_.Range(2, 5), 6, 14, &spans);
        PlacedText text = Text(content, Font{L"Segoe UI", 11.0}, static_cast<float>(left_), 0.0f, Column(), kInk);
        if (text.layout != nullptr && !spans.empty()) {
            // A few words in bold, italic, semibold and as underlined links.
            for (int i = 0; i < 4; ++i) {
                const WordSpan span = spans[static_cast<size_t>(random_.Range(0, static_cast<int>(spans.size()) - 1))];
                const DWRITE_TEXT_RANGE range{span.start, span.length};
                switch (i) {
                    case 0: text.layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range); break;
                    case 1: text.layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range); break;
                    case 2: text.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range); break;
                    default:
                        Colour(text, span, kLink);
                        text.layout->SetUnderline(TRUE, range);
                        break;
                }
            }
        }
        block.height = TextHeight(text) + Rows(2);
        block.texts.push_back(std::move(text));
        return block;
    }

    Block Code() {
        Block block;
        std::wstring content;
        std::vector<std::pair<WordSpan, uint32_t>> colours;
        const int lines = random_.Range(4, 9);
        int depth = 0;
        for (int line = 0; line < lines; ++line) {
            if (line > 0) { content += L'\n'; }
            content.append(static_cast<size_t>(depth) * 4, L' ');
            const int kind = random_.Range(0, 4);
            const auto keyword = [&](const wchar_t* word) {
                colours.push_back({WordSpan{static_cast<UINT32>(content.size()), static_cast<UINT32>(std::wcslen(word))}, kKeyword});
                content += word;
            };
            if (kind == 0 && depth < 2 && line + 1 < lines) {
                keyword(L"if");
                content += L" (" + Word(random_, true) + L" > " + std::to_wstring(random_.Range(1, 99)) + L") {";
                ++depth;
            } else if (kind == 1 && depth > 0) {
                keyword(L"return");
                content += L" " + Word(random_, true) + L"." + Word(random_, true) + L"();";
            } else if (kind == 2) {
                const UINT32 start = static_cast<UINT32>(content.size());
                content += L"// " + Word(random_, true) + L" " + Word(random_, true) + L" " + Word(random_, true);
                colours.push_back({WordSpan{start, static_cast<UINT32>(content.size()) - start}, kComment});
            } else {
                keyword(L"auto");
                content += L" " + Word(random_, true) + L" = " + Word(random_, true) + L"(" + Word(random_, true) +
                           L", " + std::to_wstring(random_.Range(0, 512)) + L");";
            }
            if (depth > 0 && line + 1 == lines) {
                content += L" }";
            }
        }
        const float pad = Px(12);
        PlacedText text = Text(content, Font{L"Consolas", 10.0}, static_cast<float>(left_) + pad, Px(10),
                               Column() - 2 * pad, kInk, false);
        for (const auto& [span, colour] : colours) { Colour(text, span, colour); }
        block.height = Rows(10) + TextHeight(text) + Rows(10);
        const D2D1_RECT_F box = D2D1::RectF(static_cast<float>(left_) + 0.5f, 0.5f, static_cast<float>(right_) - 0.5f,
                                            static_cast<float>(block.height) - 0.5f);
        block.shapes.push_back(Shape{Shape::RoundFill, box, Px(4), 1, kCodeBack});
        block.shapes.push_back(Shape{Shape::RoundStroke, box, Px(4), 1, kCodeBorder});
        block.texts.push_back(std::move(text));
        return block;
    }

    Block Quote() {
        Block block;
        PlacedText text = Text(Sentences(random_, random_.Range(1, 3), 6, 12),
                               Font{L"Times New Roman", 13.0, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC},
                               static_cast<float>(left_) + Px(18), Px(4), Column() - Px(18), kMuted);
        block.height = Rows(4) + TextHeight(text) + Rows(4);
        block.shapes.push_back(Shape{Shape::Fill,
                                     D2D1::RectF(static_cast<float>(left_), 0.0f, static_cast<float>(left_) + Px(4),
                                                 static_cast<float>(block.height)),
                                     0, 1, kQuoteBar});
        block.texts.push_back(std::move(text));
        return block;
    }

    Block List() {
        Block block;
        const bool numbered = random_.Chance(40);
        const int items = random_.Range(3, 6);
        int y = 0;
        for (int item = 0; item < items; ++item) {
            const std::wstring mark = numbered ? std::to_wstring(item + 1) + L"." : std::wstring(L"•");
            block.texts.push_back(Text(mark, Font{L"Arial", 11.0}, static_cast<float>(left_) + Px(6),
                                       static_cast<float>(y), Px(18), kMuted, false));
            PlacedText text = Text(Sentences(random_, 1, 4, 11), Font{L"Arial", 11.0},
                                   static_cast<float>(left_) + Px(26), static_cast<float>(y), Column() - Px(26), kInk);
            y += TextHeight(text) + Rows(4);
            block.texts.push_back(std::move(text));
        }
        block.height = y;
        return block;
    }

    /// A picture card; `slot` receives the picture's rectangle (block coordinates).
    Block PictureCard(Box* slot = nullptr) {
        Block block;
        const int pad = Rows(8);
        const int picture_height = Rows(random_.Range(130, 190));
        const Box picture{left_ + pad, pad, right_ - pad, pad + picture_height};
        block.pictures.push_back(Picture{picture, random_.Next()});
        PlacedText caption = Text(Sentences(random_, 1, 3, 8), Font{L"Arial", 9.0}, static_cast<float>(left_ + pad),
                                  static_cast<float>(picture.bottom + Rows(6)), Column() - 2.0f * pad, kMuted);
        block.height = picture.bottom + Rows(6) + TextHeight(caption) + pad;
        block.shapes.push_back(Shape{Shape::RoundStroke,
                                     D2D1::RectF(static_cast<float>(left_) + 0.5f, 0.5f, static_cast<float>(right_) - 0.5f,
                                                 static_cast<float>(block.height) - 0.5f),
                                     Px(6), 1, kCardBorder});
        block.texts.push_back(std::move(caption));
        if (slot != nullptr) { *slot = picture; }
        return block;
    }

    /// A comment box being typed into; `caret` receives where the caret blinks.
    Block Comment(Box& caret) {
        Block block;
        PlacedText label = Text(Title(random_, 2, 3), Font{L"Segoe UI", 9.0, DWRITE_FONT_WEIGHT_SEMI_BOLD},
                                static_cast<float>(left_), 0.0f, Column(), kMuted);
        const int label_height = TextHeight(label);
        const int pad = Rows(10);
        const int box_top = label_height + Rows(4);
        const std::wstring typed = Sentences(random_, 1, 5, 9);
        PlacedText text = Text(typed, Font{L"Segoe UI", 11.0}, static_cast<float>(left_ + pad),
                               static_cast<float>(box_top + pad), Column() - 2.0f * pad, kInk);
        const int text_height = TextHeight(text);
        block.height = box_top + pad + std::max(text_height, Rows(40)) + pad + Rows(2);
        block.shapes.push_back(Shape{Shape::RoundStroke,
                                     D2D1::RectF(static_cast<float>(left_) + 1.0f, static_cast<float>(box_top) + 1.0f,
                                                 static_cast<float>(right_) - 1.0f, static_cast<float>(block.height - Rows(2)) - 1.0f),
                                     Px(6), 1.5f, kCommentBorder});
        // The caret stands after the last character typed.
        DWRITE_HIT_TEST_METRICS hit{};
        FLOAT x = 0, y = 0;
        if (text.layout != nullptr) {
            text.layout->HitTestTextPosition(static_cast<UINT32>(typed.size()), FALSE, &x, &y, &hit);
        }
        const int caret_left = static_cast<int>(std::lround(text.x + x)) + 1;
        const int caret_top = static_cast<int>(std::lround(text.y + hit.top));
        caret = Box{caret_left, caret_top, caret_left + std::max(1, Rows(1)),
                    caret_top + std::max(1, static_cast<int>(std::lround(hit.height)))};
        block.texts.push_back(std::move(label));
        block.texts.push_back(std::move(text));
        return block;
    }

    /// A status card with an activity indicator at `spinner`.
    Block Status(Box& spinner) {
        Block block;
        const int size = Rows(22);
        const int pad = Rows(12);
        PlacedText text = Text(Sentences(random_, 1, 3, 6), Font{L"Segoe UI", 10.0, DWRITE_FONT_WEIGHT_SEMI_BOLD},
                               static_cast<float>(left_ + pad + size + pad), static_cast<float>(pad),
                               Column() - static_cast<float>(3 * pad + size), kInk);
        block.height = pad + std::max(size, TextHeight(text)) + pad;
        const D2D1_RECT_F box = D2D1::RectF(static_cast<float>(left_) + 0.5f, 0.5f, static_cast<float>(right_) - 0.5f,
                                            static_cast<float>(block.height) - 0.5f);
        block.shapes.push_back(Shape{Shape::RoundFill, box, Px(6), 1, kStatusBack});
        block.shapes.push_back(Shape{Shape::RoundStroke, box, Px(6), 1, kStatusBorder});
        const int top = (block.height - size) / 2;
        spinner = Box{left_ + pad, top, left_ + pad + size, top + size};
        block.texts.push_back(std::move(text));
        return block;
    }

    Block Section() {
        Block block;
        block.height = Rows(44);
        block.shapes.push_back(Shape{Shape::Fill, D2D1::RectF(0.0f, 0.0f, static_cast<float>(options_.width), static_cast<float>(block.height)),
                                     0, 1, kSectionBack});
        block.shapes.push_back(Shape{Shape::Fill, D2D1::RectF(0.0f, static_cast<float>(block.height - 1), static_cast<float>(options_.width), static_cast<float>(block.height)),
                                     0, 1, kRule});
        block.texts.push_back(Text(Title(random_, 2, 4), Font{L"Georgia", 15.0, DWRITE_FONT_WEIGHT_BOLD},
                                   static_cast<float>(left_), Px(10), Column(), kInk, false));
        return block;
    }

    Block FeedRow() {
        Block block;
        block.height = Rows(68);
        const int avatar = Rows(36);
        const int top = (block.height - avatar) / 2;
        block.shapes.push_back(Shape{Shape::Ellipse,
                                     D2D1::RectF(static_cast<float>(left_), static_cast<float>(top),
                                                 static_cast<float>(left_ + avatar), static_cast<float>(top + avatar)),
                                     0, 1, Rgb(random_.Range(60, 200), random_.Range(60, 200), random_.Range(60, 200))});
        const float text_left = static_cast<float>(left_ + avatar + Rows(12));
        const float text_width = static_cast<float>(right_) - text_left;
        block.texts.push_back(Text(Capitalized(Word(random_)) + L" " + Capitalized(Word(random_)),
                                   Font{L"Segoe UI", 10.5, DWRITE_FONT_WEIGHT_SEMI_BOLD}, text_left, Px(10),
                                   text_width - Px(80), kInk, false));
        block.texts.push_back(Text(Sentences(random_, 1, 6, 11), Font{L"Segoe UI", 10.0}, text_left, Px(32), text_width,
                                   kInk, false));
        block.texts.push_back(Text(std::to_wstring(random_.Range(1, 59)) + L" dk", Font{L"Arial", 9.0}, text_left,
                                   Px(12), text_width, kMuted, false, DWRITE_TEXT_ALIGNMENT_TRAILING));
        block.shapes.push_back(Shape{Shape::Fill, D2D1::RectF(text_left, static_cast<float>(block.height - 1), static_cast<float>(right_), static_cast<float>(block.height)),
                                     0, 1, kRule});
        return block;
    }

    Block FeedEnd() {
        Block block;
        block.height = Rows(56);
        block.texts.push_back(Text(Title(random_, 2, 4), Font{L"Arial", 9.0}, static_cast<float>(left_), Px(20), Column(),
                                   kMuted, false, DWRITE_TEXT_ALIGNMENT_CENTER));
        return block;
    }

    /// The row shown while the next batch of the list loads; `spinner`
    /// receives the indicator's rectangle.
    Block LoadingRow(Box& spinner) {
        Block block;
        block.height = Rows(52);
        PlacedText text = Text(Capitalized(Word(random_)) + L" " + Word(random_) + L"…", Font{L"Segoe UI", 10.0},
                               0.0f, Px(16), static_cast<float>(options_.width), kMuted, false,
                               DWRITE_TEXT_ALIGNMENT_CENTER);
        float text_width = 0;
        DWRITE_TEXT_METRICS metrics{};
        if (text.layout != nullptr && SUCCEEDED(text.layout->GetMetrics(&metrics))) { text_width = metrics.width; }
        const int size = Rows(20);
        const int right = static_cast<int>(std::lround((options_.width - text_width) / 2.0f)) - Rows(10);
        spinner = Box{right - size, (block.height - size) / 2, right, (block.height - size) / 2 + size};
        block.texts.push_back(std::move(text));
        return block;
    }

    /// The application bar; `spinner` receives where an activity indicator goes.
    Block AppBar(int height, Box& spinner) {
        Block block;
        block.height = height;
        block.shapes.push_back(Shape{Shape::Fill, D2D1::RectF(0.0f, 0.0f, static_cast<float>(options_.width), static_cast<float>(height)),
                                     0, 1, kBarBack});
        block.shapes.push_back(Shape{Shape::Fill, D2D1::RectF(0.0f, static_cast<float>(height - 1), static_cast<float>(options_.width), static_cast<float>(height)),
                                     0, 1, Rgb(20, 24, 28)});
        PlacedText title = Text(Title(random_, 1, 2), Font{L"Segoe UI", 13.0, DWRITE_FONT_WEIGHT_SEMI_BOLD}, Px(16),
                                0.0f, Px(220), kBarText, false);
        const int title_height = TextHeight(title);
        title.y = static_cast<float>((height - title_height) / 2);
        block.texts.push_back(std::move(title));
        const float field_left = Px(200);
        const float field_right = static_cast<float>(options_.width) - Px(150);
        if (field_right > field_left + Px(60)) {
            block.shapes.push_back(Shape{Shape::RoundFill,
                                         D2D1::RectF(field_left, Px(10), field_right, static_cast<float>(height) - Px(10)),
                                         Px(5), 1, kBarField});
            PlacedText hint = Text(Word(random_) + L" " + Word(random_), Font{L"Segoe UI", 10.0}, field_left + Px(10),
                                   0.0f, field_right - field_left - Px(20), kBarHint, false);
            hint.y = static_cast<float>((height - TextHeight(hint)) / 2);
            block.texts.push_back(std::move(hint));
        }
        const int icon = Rows(18);
        for (int i = 0; i < 3; ++i) {
            const int x = options_.width - Rows(16) - (i + 1) * icon - i * Rows(12);
            block.shapes.push_back(Shape{Shape::Ellipse,
                                         D2D1::RectF(static_cast<float>(x), static_cast<float>((height - icon) / 2),
                                                     static_cast<float>(x + icon), static_cast<float>((height + icon) / 2)),
                                         0, 1, kBarIcon});
        }
        const int size = Rows(20);
        const int x = options_.width - Rows(16) - 3 * icon - 3 * Rows(12) - size;
        spinner = Box{x, (height - size) / 2, x + size, (height - size) / 2 + size};
        return block;
    }

    [[nodiscard]] bool failed() const { return failed_; }

private:
    Raster& raster_;
    const AppOptions& options_;
    Random random_;
    int left_{0};
    int right_{0};
    bool failed_{false};
};

}  // namespace

// --- Pixels ---------------------------------------------------------------------------

void Pixels::Reset(int w, int h, uint32_t fill) {
    width = std::max(0, w);
    height = std::max(0, h);
    data.assign(static_cast<size_t>(width) * static_cast<size_t>(height), fill);
}

std::wstring UnavailableReason() {
    Raster raster;
    std::wstring error;
    return raster.Create(error) ? std::wstring() : error;
}

bool WritePng(const std::wstring& path, int width, int height, const uint32_t* pixels, int stride) {
    if (width <= 0 || height <= 0 || stride < width || pixels == nullptr) { return false; }
    std::vector<uint32_t> opaque(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        const uint32_t* source = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
        uint32_t* target = opaque.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; ++x) { target[x] = source[x] | 0xFF000000u; }
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const UINT row_bytes = static_cast<UINT>(width) * 4u;
    return SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
           SUCCEEDED(factory->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) &&
           SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA &&
           SUCCEEDED(frame->WritePixels(static_cast<UINT>(height), row_bytes, row_bytes * static_cast<UINT>(height),
                                        reinterpret_cast<BYTE*>(opaque.data()))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// --- Application --------------------------------------------------------------------

struct Application::Impl {
    AppOptions options;
    Raster raster;
    std::vector<Block> blocks;
    Pixels content;
    Pixels transmission;
    Pixels header;
    Pixels loading_row;
    std::vector<Pixels> placeholders;
    Pixels background;  ///< The fixed background, viewport-sized; empty for a page background.
    std::vector<Box> slots;
    std::vector<int> batch_ends;  ///< Loaded height after each batch of the tail.
    Box caret;                    ///< Document coordinates.
    Box status_spinner;           ///< Document coordinates.
    Box loading_spinner;          ///< Loading-row coordinates.
    Box header_spinner;           ///< Viewport coordinates.
    int header_rows{0};
    int base_height{0};
    int section_top{-1};
    int section_height{0};
    int scrollbar_columns{0};
    int thumb_min_rows{0};  ///< Shortest overlay scrollbar thumb.
    int notch_rows{0};
    int page_rows{0};

    [[nodiscard]] bool fixed_background() const { return options.background != Background::Page; }

    bool Build(std::wstring& error);
    bool RenderBlocks(std::wstring& error);
    void BaseRows(const State& state, Pixels& layer, int rows) const;
    void ScrollingOverlays(const State& state, Pixels& layer) const;
    void FixedOverlays(const State& state, Pixels& frame) const;
    [[nodiscard]] uint32_t ComputeBackground(int x, int y) const;
    [[nodiscard]] uint32_t BackgroundPixel(int x, int y) const;
    void ComposeRow(int row, int view_row, uint32_t* out) const;
};

bool Application::Impl::Build(std::wstring& error) {
    if (!raster.Create(error)) { return false; }
    Builder builder(raster, options);
    const auto rows = [&](double dips) { return builder.Rows(dips); };
    notch_rows = rows(100.0);  // A wheel notch scrolls 100 CSS pixels in a browser.
    thumb_min_rows = rows(32.0);
    header_rows = options.header == Header::None ? 0 : rows(48.0);
    page_rows = static_cast<int>(std::lround((options.view_height - header_rows) * 0.875));
    scrollbar_columns = options.overlay_scrollbar ? rows(10.0) : 0;
    const int gap = rows(14.0);

    // Where the blocks that carry a behaviour go, as a share of the page.
    struct Planned {
        double at;
        int kind;  // 0 status, 1 comment, 2 lazy slot, 3 sticky section
    };
    std::vector<Planned> planned{{0.12, 0}, {0.30, 1}};
    for (int i = 0; i < options.lazy_slots; ++i) {
        planned.push_back({options.lazy_slots == 1 ? 0.45 : 0.22 + 0.6 * i / (options.lazy_slots - 1), 2});
    }
    if (options.sticky_section) { planned.push_back({0.36, 3}); }
    std::sort(planned.begin(), planned.end(), [](const Planned& a, const Planned& b) { return a.at < b.at; });
    size_t next_planned = 0;

    int y = header_rows + rows(20.0);
    const auto add = [&](Block block) {
        block.top = y;
        y += block.height + gap;
        blocks.push_back(std::move(block));
    };
    add(builder.Heading(1));
    add(builder.Paragraph());
    while (y < options.document_rows) {
        if (next_planned < planned.size() && y >= planned[next_planned].at * options.document_rows) {
            const int kind = planned[next_planned++].kind;
            Box box;
            if (kind == 0) {
                Block block = builder.Status(box);
                status_spinner = Shifted(box, 0, y);
                add(std::move(block));
            } else if (kind == 1) {
                Block block = builder.Comment(box);
                caret = Shifted(box, 0, y);
                add(std::move(block));
            } else if (kind == 2) {
                Block block = builder.PictureCard(&box);
                slots.push_back(Shifted(box, 0, y));
                add(std::move(block));
            } else {
                section_top = y;
                Block block = builder.Section();
                section_height = block.height;
                add(std::move(block));
            }
            continue;
        }
        const int pick = builder.random().Range(0, 99);
        if (pick < 34) {
            add(builder.Paragraph());
        } else if (pick < 44) {
            add(builder.Heading(2));
        } else if (pick < 51) {
            add(builder.Heading(3));
        } else if (pick < 63) {
            add(builder.PictureCard());
        } else if (pick < 73) {
            add(builder.Code());
        } else if (pick < 80) {
            add(builder.Quote());
        } else {
            add(builder.List());
        }
    }
    add(builder.Heading(2));
    for (int i = 0; i < 4; ++i) { add(builder.FeedRow()); }
    if (options.growing_tail) {
        base_height = y;
        for (int batch = 0; batch < options.tail_batches; ++batch) {
            for (int i = 0; i < 5; ++i) { add(builder.FeedRow()); }
            if (batch + 1 == options.tail_batches) { add(builder.FeedEnd()); }
            batch_ends.push_back(y);
        }
    } else {
        add(builder.FeedEnd());
        base_height = y;
    }
    Block loading = builder.LoadingRow(loading_spinner);
    Block bar = builder.AppBar(std::max(header_rows, 1), header_spinner);
    if (builder.failed()) {
        error = L"a DirectWrite text layout could not be created";
        return false;
    }
    if (!raster.Render(options.width, loading.height, kPage, [&](ID2D1DeviceContext*) { DrawBlock(raster, loading, 0.0f); }, loading_row) || (header_rows > 0 && !raster.Render(options.width, header_rows, kBarBack, [&](ID2D1DeviceContext*) { DrawBlock(raster, bar, 0.0f); }, header))) {
        error = L"Direct2D could not render the application bar or the loading row";
        return false;
    }
    for (const Box& slot : slots) {
        Pixels placeholder;
        const int w = slot.right - slot.left, h = slot.bottom - slot.top;
        placeholder.Reset(w, h, kPlaceholder);
        // A picture symbol: a frame with a mountain in it.
        const int mark_w = std::min(w, h) / 2, mark_h = mark_w * 3 / 4;
        const Box mark{(w - mark_w) / 2, (h - mark_h) / 2, (w + mark_w) / 2, (h + mark_h) / 2};
        const int line = std::max(2, rows(2.0));
        FillBox(placeholder, Box{mark.left, mark.top, mark.right, mark.top + line}, kPlaceholderMark);
        FillBox(placeholder, Box{mark.left, mark.bottom - line, mark.right, mark.bottom}, kPlaceholderMark);
        FillBox(placeholder, Box{mark.left, mark.top, mark.left + line, mark.bottom}, kPlaceholderMark);
        FillBox(placeholder, Box{mark.right - line, mark.top, mark.right, mark.bottom}, kPlaceholderMark);
        for (int row = 0; row < mark_h / 2; ++row) {
            const int yy = mark.bottom - line - 1 - row;
            const int cx = mark.left + mark_w / 2;
            FillBox(placeholder, Box{cx - (mark_h / 2 - row), yy, cx + (mark_h / 2 - row), yy + 1}, kPlaceholderMark);
        }
        placeholders.push_back(std::move(placeholder));
    }
    return RenderBlocks(error);
}

bool Application::Impl::RenderBlocks(std::wstring& error) {
    const int height = batch_ends.empty() ? base_height : batch_ends.back();
    const bool fixed = fixed_background();
    if (fixed) {
        background.Reset(options.width, options.view_height, 0);
        for (int y = 0; y < options.view_height; ++y) {
            for (int x = 0; x < options.width; ++x) { background.Row(y)[x] = ComputeBackground(x, y); }
        }
    }
    content.Reset(options.width, height, fixed ? kBlack : kPage);
    if (fixed) { transmission.Reset(options.width, height, kWhite); }
    Pixels dark, light;
    for (const Block& block : blocks) {
        const auto draw = [&](ID2D1DeviceContext*) { DrawBlock(raster, block, 0.0f); };
        if (!raster.Render(options.width, block.height, fixed ? kBlack : kPage, draw, dark) ||
            (fixed && !raster.Render(options.width, block.height, kWhite, draw, light))) {
            error = L"Direct2D could not render a block of the page";
            return false;
        }
        for (const Picture& picture : block.pictures) {
            PaintPicture(dark, picture.box, picture.seed);
            if (fixed) { PaintPicture(light, picture.box, picture.seed); }
        }
        for (int row = 0; row < block.height && block.top + row < height; ++row) {
            std::memcpy(content.Row(block.top + row), dark.Row(row), static_cast<size_t>(options.width) * sizeof(uint32_t));
            if (fixed) {
                uint32_t* through = transmission.Row(block.top + row);
                for (int x = 0; x < options.width; ++x) {
                    uint32_t value = 0xFF000000u;
                    for (int shift = 0; shift < 24; shift += 8) {
                        const int amount = std::max(0, Channel(light.Row(row)[x], shift) - Channel(dark.Row(row)[x], shift));
                        value |= static_cast<uint32_t>(amount) << shift;
                    }
                    through[x] = value;
                }
            }
        }
    }
    return true;
}

uint32_t Application::Impl::ComputeBackground(int x, int y) const {
    switch (options.background) {
        case Background::Page: return kPage;
        case Background::FixedSolid: return kSolidBackground;
        case Background::FixedGradient: {
            const double t = options.view_height > 1 ? static_cast<double>(y) / (options.view_height - 1) : 0.0;
            return Mix(Rgb(250, 246, 238), Rgb(226, 236, 250), t);
        }
        case Background::FixedTextured: {
            const int noise = static_cast<int>(Hash(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 77u) % 21u) - 10;
            const int stripe = (x + y) % 11 < 2 ? -14 : 0;
            const int v = 236 + noise + stripe;
            return Rgb(std::clamp(v - 4, 0, 255), std::clamp(v, 0, 255), std::clamp(v + 4, 0, 255));
        }
    }
    return kPage;
}

uint32_t Application::Impl::BackgroundPixel(int x, int y) const {
    if (background.data.empty()) { return kPage; }
    return background.Row(std::clamp(y, 0, background.height - 1))[std::clamp(x, 0, background.width - 1)];
}

void Application::Impl::ComposeRow(int row, int view_row, uint32_t* out) const {
    const uint32_t* ink = content.Row(row);
    if (!fixed_background()) {
        std::memcpy(out, ink, static_cast<size_t>(options.width) * sizeof(uint32_t));
        return;
    }
    const uint32_t* through = transmission.Row(row);
    const uint32_t* behind = background.Row(std::clamp(view_row, 0, background.height - 1));
    for (int x = 0; x < options.width; ++x) {
        uint32_t value = 0xFF000000u;
        for (int shift = 0; shift < 24; shift += 8) {
            const int composed = Channel(ink[x], shift) + (Channel(through[x], shift) * Channel(behind[x], shift) + 127) / 255;
            value |= static_cast<uint32_t>(std::min(composed, 255)) << shift;
        }
        out[x] = value;
    }
}

void Application::Impl::BaseRows(const State& state, Pixels& layer, int rows) const {
    const int loading_end = state.loading ? state.loaded_height + loading_row.height : state.loaded_height;
    for (int y = 0; y < rows; ++y) {
        const int row = state.offset + y;
        const int view_row = std::min(y, options.view_height - 1);
        uint32_t* out = layer.Row(y);
        if (row >= 0 && row < state.loaded_height && row < content.height) {
            ComposeRow(row, view_row, out);
        } else if (row >= state.loaded_height && row < loading_end) {
            std::memcpy(out, loading_row.Row(row - state.loaded_height), static_cast<size_t>(options.width) * sizeof(uint32_t));
        } else {
            if (background.data.empty()) {
                std::fill(out, out + options.width, kPage);
            } else {
                std::memcpy(out, background.Row(view_row), static_cast<size_t>(options.width) * sizeof(uint32_t));
            }
        }
    }
}

void Application::Impl::ScrollingOverlays(const State& state, Pixels& layer) const {
    const int shift = -state.offset;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i < state.lazy_loaded.size() && state.lazy_loaded[i] != 0) { continue; }
        const Box at = Shifted(slots[i], 0, shift);
        const Pixels& placeholder = placeholders[i];
        for (int y = std::max(0, at.top); y < std::min(layer.height, at.bottom); ++y) {
            if (slots[i].top + (y - at.top) >= state.loaded_height) { break; }
            std::memcpy(layer.Row(y) + at.left, placeholder.Row(y - at.top),
                        static_cast<size_t>(placeholder.width) * sizeof(uint32_t));
        }
    }
    if (state.loading) {
        DrawSpinner(layer, Shifted(loading_spinner, 0, state.loaded_height + shift), state.frame, kPage, false);
    }
    if (options.caret && state.caret_on && !caret.Empty()) { FillBox(layer, Shifted(caret, 0, shift), kInk); }
    if (options.spinner && !status_spinner.Empty()) {
        // Clipped by hand: the spinner box can be partly outside the layer.
        Pixels box;
        const Box at = Shifted(status_spinner, 0, shift);
        if (at.bottom > 0 && at.top < layer.height) {
            box.Reset(at.right - at.left, at.bottom - at.top, kStatusBack);
            DrawSpinner(box, Box{0, 0, box.width, box.height}, state.frame, kStatusBack, false);
            for (int y = std::max(0, at.top); y < std::min(layer.height, at.bottom); ++y) {
                std::memcpy(layer.Row(y) + at.left, box.Row(y - at.top), static_cast<size_t>(box.width) * sizeof(uint32_t));
            }
        }
    }
}

void Application::Impl::FixedOverlays(const State& state, Pixels& frame) const {
    if (section_top >= 0 && section_top < state.offset + header_rows) {
        for (int y = 0; y < section_height && header_rows + y < frame.height; ++y) {
            ComposeRow(section_top + y, header_rows + y, frame.Row(header_rows + y));
        }
    }
    if (scrollbar_columns > 0 && state.scrollbar_alpha > 0.0) {
        const int track_top = header_rows, track = options.view_height - header_rows;
        const int extent = std::max(options.view_height, state.max_scroll + options.view_height);
        const int thumb = std::max(thumb_min_rows, static_cast<int>(static_cast<int64_t>(track) * options.view_height / extent));
        const int travel = track - thumb;
        const int top = track_top + (state.max_scroll > 0
                                         ? static_cast<int>(static_cast<int64_t>(travel) * state.offset / state.max_scroll)
                                         : 0);
        const int right = options.width - (scrollbar_columns * 3) / 10;
        const int left = right - (scrollbar_columns * 6) / 10;
        const double amount = 0.55 * state.scrollbar_alpha;
        for (int y = std::max(track_top, top); y < std::min(frame.height, top + thumb); ++y) {
            uint32_t* row = frame.Row(y);
            for (int x = left; x < right; ++x) { row[x] = Mix(row[x], Rgb(100, 100, 100), amount); }
        }
    }
    if (header_rows > 0) {
        for (int y = 0; y < header_rows && y < frame.height; ++y) {
            std::memcpy(frame.Row(y), header.Row(y), static_cast<size_t>(options.width) * sizeof(uint32_t));
        }
        if (options.header == Header::Animating) { DrawSpinner(frame, header_spinner, state.frame, kBarBack, true); }
    }
}

Application::Application(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Application::~Application() = default;

std::unique_ptr<Application> Application::Create(const AppOptions& options, std::wstring& error) {
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    if (options.width < 200 || options.view_height < 160 || options.scale < 1.0 ||
        options.lazy_slots > SharedHeader::kMaxBoxes) {
        error = L"invalid options";
        return nullptr;
    }
    if (!impl->Build(error)) { return nullptr; }
    return std::unique_ptr<Application>(new Application(std::move(impl)));
}

const AppOptions& Application::options() const { return impl_->options; }
int Application::width() const { return impl_->options.width; }
int Application::view_height() const { return impl_->options.view_height; }
int Application::header_rows() const { return impl_->header_rows; }
int Application::content_height() const { return impl_->content.height; }
int Application::base_height() const { return impl_->base_height; }
int Application::loaded_height_after(int batches) const {
    if (batches <= 0 || impl_->batch_ends.empty()) { return impl_->base_height; }
    return impl_->batch_ends[static_cast<size_t>(std::min<int>(batches, static_cast<int>(impl_->batch_ends.size())) - 1)];
}
int Application::loading_row_height() const { return impl_->loading_row.height; }
int Application::notch_rows() const { return impl_->notch_rows; }
int Application::page_rows() const { return impl_->page_rows; }
const Pixels& Application::content() const { return impl_->content; }
const Pixels& Application::transmission() const { return impl_->transmission; }
const Pixels& Application::header() const { return impl_->header; }
const Pixels& Application::placeholder(size_t slot) const { return impl_->placeholders[slot]; }
const std::vector<Box>& Application::lazy_slots() const { return impl_->slots; }
int Application::scrollbar_columns() const { return impl_->scrollbar_columns; }
int Application::sticky_section_top() const { return impl_->section_top; }
uint32_t Application::BackgroundPixel(int x, int y) const { return impl_->BackgroundPixel(x, y); }
void Application::ComposeContentRow(int row, int view_row, uint32_t* out) const { impl_->ComposeRow(row, view_row, out); }

std::vector<Box> Application::masks() const {
    std::vector<Box> boxes;
    if (impl_->options.caret && !impl_->caret.Empty()) { boxes.push_back(impl_->caret); }
    if (impl_->options.spinner && !impl_->status_spinner.Empty()) { boxes.push_back(impl_->status_spinner); }
    return boxes;
}

void Application::Compose(const State& state, Pixels& frame) const {
    const int width = impl_->options.width, height = impl_->options.view_height;
    if (state.fraction > 0.0) {
        // A layer scrolled by a fraction of a pixel is resampled between rows,
        // as a compositor does; what is fixed to the viewport is not.
        Pixels layer;
        layer.Reset(width, height + 1, 0);
        impl_->BaseRows(state, layer, height + 1);
        impl_->ScrollingOverlays(state, layer);
        frame.Reset(width, height, 0);
        const uint32_t weight = static_cast<uint32_t>(std::lround(state.fraction * 256.0));
        for (int y = 0; y < height; ++y) {
            const uint32_t* a = layer.Row(y);
            const uint32_t* b = layer.Row(y + 1);
            uint32_t* out = frame.Row(y);
            for (int x = 0; x < width; ++x) {
                uint32_t value = 0xFF000000u;
                for (int shift = 0; shift < 24; shift += 8) {
                    const uint32_t blended = (((a[x] >> shift) & 0xFFu) * (256u - weight) + ((b[x] >> shift) & 0xFFu) * weight + 128u) >> 8;
                    value |= blended << shift;
                }
                out[x] = value;
            }
        }
    } else {
        frame.Reset(width, height, 0);
        impl_->BaseRows(state, frame, height);
        impl_->ScrollingOverlays(state, frame);
    }
    impl_->FixedOverlays(state, frame);
}

bool Application::RenderImmediate(const State& state, Pixels& frame) {
    Impl& impl = *impl_;
    if (impl.fixed_background() || state.fraction > 0.0) { return false; }
    const int width = impl.options.width, height = impl.options.view_height;
    const int top = state.offset, bottom = state.offset + height;
    const bool rendered = impl.raster.Render(width, height, kPage, [&](ID2D1DeviceContext*) {
        for (const Block& block : impl.blocks) {
            if (block.top + block.height <= top || block.top >= bottom || block.top >= state.loaded_height) { continue; }
            DrawBlock(impl.raster, block, static_cast<float>(block.top - top));
        } }, frame);
    if (!rendered) { return false; }
    for (const Block& block : impl.blocks) {
        if (block.top + block.height <= top || block.top >= bottom || block.top >= state.loaded_height) { continue; }
        for (const Picture& picture : block.pictures) {
            PaintPicture(frame, Shifted(picture.box, 0, block.top - top), picture.seed);
        }
    }
    // Rows past what is loaded show what Compose shows there.
    if (state.offset + height > state.loaded_height) {
        Pixels rest;
        rest.Reset(width, height, 0);
        impl.BaseRows(state, rest, height);
        for (int y = std::max(0, state.loaded_height - state.offset); y < height; ++y) {
            std::memcpy(frame.Row(y), rest.Row(y), static_cast<size_t>(width) * sizeof(uint32_t));
        }
    }
    impl.ScrollingOverlays(state, frame);
    impl.FixedOverlays(state, frame);
    return true;
}

// --- Simulation ------------------------------------------------------------------------

Simulation::Simulation(const Application& app, uint32_t seed) : app_(app), random_(seed) {
    first_seen_ms_.assign(app.lazy_slots().size(), -1.0);
    state_.lazy_loaded.assign(app.lazy_slots().size(), 0);
    state_.loaded_height = app.loaded_height_after(0);
    state_.complete = !app.options().growing_tail;
    state_.max_scroll = max_scroll();
}

int Simulation::max_scroll() const {
    const int extent = state_.complete ? app_.content_height()
                                       : state_.loaded_height + (state_.loading ? app_.loading_row_height() : 0);
    return std::max(0, extent - app_.view_height());
}

int Simulation::target() const { return static_cast<int>(std::lround(target_)); }

int Simulation::pending_lazy() const {
    int pending = 0;
    for (size_t i = 0; i < first_seen_ms_.size(); ++i) {
        if (first_seen_ms_[i] >= 0.0 && state_.lazy_loaded[i] == 0) { ++pending; }
    }
    return pending;
}

bool Simulation::settled() const { return !state_.animating && pending_lazy() == 0; }

bool Simulation::at_end() const {
    return settled() && state_.complete && !state_.loading && state_.offset == max_scroll() && state_.fraction == 0.0;
}

double Simulation::PositionAt(double time_ms) const {
    if (duration_ms_ <= 0.0 || time_ms >= started_ms_ + duration_ms_) { return target_; }
    const double u = std::clamp((time_ms - started_ms_) / duration_ms_, 0.0, 1.0);
    const double eased = 1.0 - (1.0 - u) * (1.0 - u) * (1.0 - u);  // Ease-out cubic.
    return start_ + (target_ - start_) * eased;
}

void Simulation::Retarget(double time_ms, double rows) {
    start_ = PositionAt(time_ms);
    target_ = std::clamp(target_ + rows, 0.0, static_cast<double>(max_scroll()));
    started_ms_ = time_ms;
    duration_ms_ = random_.Range(150, 250);
}

void Simulation::Wheel(double time_ms, int notches) { Retarget(time_ms, static_cast<double>(notches) * app_.notch_rows()); }

void Simulation::PageDown(double time_ms) { Retarget(time_ms, app_.page_rows()); }

void Simulation::Present(double time_ms) {
    const AppOptions& options = app_.options();
    state_.time_ms = time_ms;
    if (state_.loading && time_ms >= loading_since_ms_ + options.tail_delay_ms) {
        state_.loading = false;
        ++batches_;
        state_.loaded_height = app_.loaded_height_after(batches_);
        state_.complete = batches_ >= options.tail_batches;
    }
    const double limit = max_scroll();
    state_.position = std::clamp(PositionAt(time_ms), 0.0, limit);
    if (options.snap_to_pixels) {
        state_.offset = static_cast<int>(std::lround(state_.position));
        state_.fraction = 0.0;
    } else {
        state_.offset = static_cast<int>(std::floor(state_.position));
        state_.fraction = state_.position - state_.offset;
    }
    state_.animating = duration_ms_ > 0.0 && time_ms < started_ms_ + duration_ms_ && start_ != target_;
    if (state_.animating) {
        last_motion_ms_ = time_ms;
        moved_ = true;
    }
    const int visible_top = state_.offset + app_.header_rows(), visible_bottom = state_.offset + app_.view_height();
    for (size_t i = 0; i < first_seen_ms_.size(); ++i) {
        const Box& slot = app_.lazy_slots()[i];
        if (first_seen_ms_[i] < 0.0 && slot.top < visible_bottom && slot.bottom > visible_top &&
            slot.top < state_.loaded_height) {
            first_seen_ms_[i] = time_ms;
        }
        state_.lazy_loaded[i] =
            first_seen_ms_[i] >= 0.0 && time_ms >= first_seen_ms_[i] + options.lazy_delay_ms ? 1 : 0;
    }
    if (!state_.complete && !state_.loading && visible_bottom >= state_.loaded_height) {
        state_.loading = true;
        loading_since_ms_ = time_ms;
    }
    ++state_.frame;
    state_.caret_on = options.caret_blink_ms <= 0 || static_cast<int64_t>(time_ms / options.caret_blink_ms) % 2 == 0;
    if (!moved_) {
        state_.scrollbar_alpha = 0.0;
    } else {
        const double idle = time_ms - last_motion_ms_;
        state_.scrollbar_alpha = idle < 700.0 ? 1.0 : (idle < 1000.0 ? 1.0 - (idle - 700.0) / 300.0 : 0.0);
    }
    state_.max_scroll = max_scroll();
}

// --- Side channel -----------------------------------------------------------------------

std::wstring SharedMappingName(DWORD process_id) {
    return L"Local\\EasnapScrollshotRealisticApp-" + std::to_wstring(process_id);
}

LONG ReadShared(const volatile LONG& value) {
    const LONG number = value;  // An aligned 32-bit read is atomic.
    std::atomic_thread_fence(std::memory_order_acquire);
    return number;
}

void WriteShared(volatile LONG& value, LONG number) { ::InterlockedExchange(&value, number); }

namespace {
constexpr uint64_t kPixelsStart = 4096;
}  // namespace

SharedPublisher::~SharedPublisher() {
    if (header_ != nullptr) { ::UnmapViewOfFile(header_); }
    if (mapping_ != nullptr) { ::CloseHandle(mapping_); }
}

bool SharedPublisher::Create(const std::wstring& name, const Application& app) {
    static_assert(sizeof(SharedHeader) <= kPixelsStart);
    const uint64_t header_bytes = static_cast<uint64_t>(app.header().data.size()) * sizeof(uint32_t);
    const uint64_t content_bytes = static_cast<uint64_t>(app.content().data.size()) * sizeof(uint32_t);
    const uint64_t size = kPixelsStart + header_bytes + content_bytes;
    mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                                    static_cast<DWORD>(size & 0xFFFFFFFFu), name.c_str());
    if (mapping_ == nullptr) { return false; }
    void* view = ::MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, 0);
    if (view == nullptr) { return false; }
    header_ = new (view) SharedHeader();
    auto* base = static_cast<uint8_t*>(view);
    header_->magic = SharedHeader::kMagic;
    header_->version = 1;
    header_->width = app.width();
    header_->view_height = app.view_height();
    header_->header_rows = app.header_rows();
    header_->content_height = app.content_height();
    header_->scrollbar_columns = app.scrollbar_columns();
    const std::vector<Box> masks = app.masks();
    header_->mask_count = static_cast<int32_t>(std::min<size_t>(masks.size(), SharedHeader::kMaxBoxes));
    for (int i = 0; i < header_->mask_count; ++i) { header_->masks[i] = masks[static_cast<size_t>(i)]; }
    header_->slot_count = static_cast<int32_t>(std::min<size_t>(app.lazy_slots().size(), SharedHeader::kMaxBoxes));
    for (int i = 0; i < header_->slot_count; ++i) { header_->slots[i] = app.lazy_slots()[static_cast<size_t>(i)]; }
    header_->header_offset = kPixelsStart;
    header_->content_offset = kPixelsStart + header_bytes;
    if (header_bytes != 0) { std::memcpy(base + header_->header_offset, app.header().data.data(), static_cast<size_t>(header_bytes)); }
    std::memcpy(base + header_->content_offset, app.content().data.data(), static_cast<size_t>(content_bytes));
    return true;
}

void SharedPublisher::Publish(const Simulation& simulation, LONG inputs, LONG presented) {
    if (header_ == nullptr) { return; }
    const State& state = simulation.state();
    WriteShared(header_->target, simulation.target());
    WriteShared(header_->animating, state.animating ? 1 : 0);
    WriteShared(header_->pending_lazy, simulation.pending_lazy());
    WriteShared(header_->loading, state.loading ? 1 : 0);
    WriteShared(header_->loaded_height, state.loaded_height);
    WriteShared(header_->max_scroll, simulation.max_scroll());
    WriteShared(header_->complete, state.complete ? 1 : 0);
    WriteShared(header_->inputs, inputs);
    WriteShared(header_->presented, presented);
    WriteShared(header_->offset, state.offset);  // Last: a reader that sees it sees the rest.
}

void SharedPublisher::SetReady() {
    if (header_ != nullptr) { WriteShared(header_->ready, 1); }
}

SharedReader::~SharedReader() {
    if (header_ != nullptr) { ::UnmapViewOfFile(header_); }
    if (mapping_ != nullptr) { ::CloseHandle(mapping_); }
}

bool SharedReader::Open(const std::wstring& name) {
    if (header_ != nullptr) { return ReadShared(header_->ready) == 1; }
    mapping_ = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (mapping_ == nullptr) { return false; }
    const void* view = ::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        ::CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    header_ = static_cast<const SharedHeader*>(view);
    base_ = static_cast<const uint8_t*>(view);
    if (header_->magic != SharedHeader::kMagic || header_->version != 1) {
        ::UnmapViewOfFile(header_);
        ::CloseHandle(mapping_);
        header_ = nullptr;
        mapping_ = nullptr;
        return false;
    }
    return ReadShared(header_->ready) == 1;
}

const uint32_t* SharedReader::ContentRow(int y) const {
    if (header_ == nullptr || y < 0 || y >= header_->content_height) { return nullptr; }
    return reinterpret_cast<const uint32_t*>(base_ + header_->content_offset) + static_cast<size_t>(y) * static_cast<size_t>(header_->width);
}

const uint32_t* SharedReader::HeaderRow(int y) const {
    if (header_ == nullptr || y < 0 || y >= header_->header_rows) { return nullptr; }
    return reinterpret_cast<const uint32_t*>(base_ + header_->header_offset) + static_cast<size_t>(y) * static_cast<size_t>(header_->width);
}

}  // namespace easnap_test::realistic
