// Easnap - GDI drawing helpers shared by the overlay surfaces.
#include "ui/ui_theme.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <tuple>
#include <vector>
#include <gdiplus.h>

namespace easnap::ui::theme {
namespace {

/// Scoped GDI object selection: restores the previous object on destruction so
/// no device context ever leaks a pen or brush.
class ScopedGdiObject {
public:
    ScopedGdiObject(HDC dc, HGDIOBJ object) : dc_(dc), object_(object) {
        if (dc_ != nullptr && object_ != nullptr) {
            previous_ = ::SelectObject(dc_, object_);
        }
    }
    ~ScopedGdiObject() {
        if (previous_ != nullptr) {
            ::SelectObject(dc_, previous_);
        }
        if (object_ != nullptr) {
            ::DeleteObject(object_);
        }
    }

    ScopedGdiObject(const ScopedGdiObject&) = delete;
    ScopedGdiObject& operator=(const ScopedGdiObject&) = delete;

private:
    HDC dc_{nullptr};
    HGDIOBJ object_{nullptr};
    HGDIOBJ previous_{nullptr};
};

}  // namespace

Palette NormalPalette(app::AppearanceTheme preference) noexcept {
    if (preference == app::AppearanceTheme::Dark) {
        return {RGB(43,46,50), RGB(40,43,47), RGB(45,48,52), RGB(56,59,64), RGB(125,130,138),
            RGB(240,241,243), RGB(181,185,192), RGB(238,94,90), RGB(30,20,20),
            RGB(79,83,90), RGB(12,14,17), RGB(49,52,57), RGB(39,42,46), RGB(43,46,50),
            RGB(255,154,150), RGB(181,185,192), RGB(114,220,155), RGB(255,130,137), RGB(42,15,19),
            RGB(255,130,137), RGB(239,194,107), RGB(177,183,128), RGB(238,94,90), RGB(245,245,245),
            RGB(185,189,195), RGB(34,37,41), RGB(238,94,90), RGB(245,245,245), RGB(43,46,50), RGB(181,185,192)};
    }
    return {RGB(232,232,231), RGB(228,228,227), RGB(233,233,232), RGB(215,216,215), RGB(127,130,132),
        RGB(44,47,51), RGB(88,92,96), RGB(238,94,90), RGB(38,19,18),
        RGB(255,255,255), RGB(158,162,164), RGB(236,236,235), RGB(226,227,226), RGB(232,232,231),
        RGB(167,47,44), RGB(88,92,96), RGB(28,104,63), RGB(178,35,52), RGB(255,255,255),
        RGB(178,35,52), RGB(130,89,12), RGB(177,183,128), RGB(238,94,90), RGB(255,255,255),
        RGB(104,108,112), RGB(216,217,216), RGB(192,67,63), RGB(44,47,51), RGB(232,232,231), RGB(88,92,96)};
}

namespace {
app::AppearanceTheme preference_ = app::AppearanceTheme::Light;
Palette palette_ = NormalPalette(preference_);
}

const Palette& Current() noexcept { return palette_; }
app::AppearanceTheme Preference() noexcept { return preference_; }

void RefreshSystemColors() noexcept {
    palette_ = NormalPalette(preference_);
    HIGHCONTRASTW contrast{sizeof(contrast), 0, nullptr};
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0) {
        const COLORREF surface = ::GetSysColor(COLOR_WINDOW);
        const COLORREF text = ::GetSysColor(COLOR_WINDOWTEXT);
        const COLORREF selected = ::GetSysColor(COLOR_HIGHLIGHT);
        const COLORREF on_selected = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
        palette_ = {surface, surface, surface, text, text, text, text, selected, on_selected,
            surface, surface, surface, selected, surface, selected, ::GetSysColor(COLOR_GRAYTEXT),
            text, selected, on_selected, text, text, text, text, text, text,
            text, selected, text, surface, text, true};
    }
}

void ApplyPreference(app::AppearanceTheme preference) noexcept {
    preference_ = app::IsValidAppearanceTheme(preference) ? preference : app::AppearanceTheme::Light;
    RefreshSystemColors();
}

void NotifyWindows() noexcept {
    // Synchronous, on the owner thread, including hidden retained surfaces. No posted
    // callbacks can outlive an HWND. This message never means a geometry change.
    ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        ::SendMessageW(window, kChangedMessage, 0, 0);
        ::EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL {
            // The preview helper embeds a window from another process. Never
            // synchronously enter that process for a presentation-only update.
            if (::GetWindowThreadProcessId(child, nullptr) == ::GetCurrentThreadId()) {
                ::SendMessageW(child, kChangedMessage, 0, 0);
            }
            return TRUE;
        }, 0);
        return TRUE;
    }, 0);
}

D2D1_COLOR_F D2DColor(COLORREF color, float alpha) noexcept {
    return {GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, alpha};
}

void DrawPanel(HDC dc, const RECT& rect, float scale) {
    const auto& p = Current();
    FillSolid(dc, rect, p.panel);
    if (p.high_contrast) {
        FrameRounded(dc, rect, Scaled(kPanelRadius, scale), p.control_border, Scaled(1, scale));
    }
}

COLORREF Mix(COLORREF a, COLORREF b, float amount) noexcept {
    const float t = std::clamp(amount, 0.0f, 1.0f);
    const auto c = [t](int x, int y) { return static_cast<BYTE>(std::lround(x + (y-x)*t)); };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
}

bool AnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    ::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled && !Current().high_contrast;
}

namespace {
struct MaterialMask {
    int width{}, height{}, radius{}, padding{}, offset{};
    std::vector<float> face, blurred;
};
// UI-thread only, bounded cache. Geometry and scale form the key; colors never
// enter the masks, so theme changes cannot reuse stale colored shadows.
const MaterialMask& MaskFor(int width, int height, int radius, int padding, int offset) {
    static thread_local std::deque<MaterialMask> cache;
    for (const auto& m : cache) {
        if (m.width == width && m.height == height && m.radius == radius &&
            m.padding == padding && m.offset == offset) { return m; }
    }
    if (cache.size() == 64) { cache.pop_front(); }
    MaterialMask m{width, height, radius, padding, offset, {}, {}};
    m.face.resize(static_cast<size_t>(width)*height);
    const float cx = width*0.5f, cy = height*0.5f;
    const float rr = static_cast<float>(std::min({radius, (width-2*padding)/2, (height-2*padding)/2}));
    for (int y=0; y<height; ++y) for (int x=0; x<width; ++x) {
        const float qx = std::abs(x+0.5f-cx)-(cx-padding-rr);
        const float qy = std::abs(y+0.5f-cy)-(cy-padding-rr);
        const float distance = std::hypot(std::max(qx,0.0f),std::max(qy,0.0f)) + std::min(std::max(qx,qy),0.0f)-rr;
        m.face[static_cast<size_t>(y)*width+x] = std::clamp(0.5f-distance,0.0f,1.0f);
    }
    const float sigma = std::max(0.7f, padding/3.0f);
    const int support = static_cast<int>(std::ceil(3*sigma));
    std::vector<float> kernel(static_cast<size_t>(support)*2+1);
    float total=0;
    for(int k=-support; k<=support; ++k) { total += kernel[k+support] = std::exp(-k*k/(2*sigma*sigma)); }
    for(auto& k:kernel) { k/=total; }
    std::vector<float> horizontal(m.face.size());
    m.blurred.resize(m.face.size());
    for(int y=0; y<height; ++y) for(int x=0; x<width; ++x) {
        float value=0;
        for(int k=-support; k<=support; ++k) if(x+k>=0 && x+k<width) { value+=m.face[static_cast<size_t>(y)*width+x+k]*kernel[k+support]; }
        horizontal[static_cast<size_t>(y)*width+x]=value;
    }
    for(int y=0; y<height; ++y) for(int x=0; x<width; ++x) {
        float value=0;
        for(int k=-support; k<=support; ++k) if(y+k>=0 && y+k<height) { value+=horizontal[static_cast<size_t>(y+k)*width+x]*kernel[k+support]; }
        m.blurred[static_cast<size_t>(y)*width+x]=value;
    }
    cache.push_back(std::move(m));
    return cache.back();
}
}

void DrawMaterial(HDC dc, const RECT& rect, float radius, float scale, COLORREF fill, float depth, bool drop_shadow) {
    const auto& p=Current();
    const int pad=Scaled(6,scale), destination_width=rect.right-rect.left+2*pad, height=rect.bottom-rect.top+2*pad;
    if(!dc || destination_width<=2*pad || height<=2*pad) { return; }
    if(p.high_contrast) { FillRoundedBordered(dc,rect,Scaled(radius,scale),fill,p.control_border,Scaled(1,scale)); return; }
    // Beyond each rounded end, the Gaussian shadow is horizontally constant.
    // Keep the complete corner/blur/offset support and repeat only that flat
    // center column, so resizing does not generate a new full-width mask.
    const int corner=std::min({Scaled(radius,scale),(destination_width-2*pad)/2,(height-2*pad)/2});
    const int cap=pad+corner+static_cast<int>(std::ceil(3*std::max(0.7f,pad/3.0f)))+Scaled(3,scale);
    const int width=std::min(destination_width,2*cap+1);
    const auto key=std::make_tuple(width,height,corner,pad,Scaled(2,scale),Scaled(3,scale),
        p.panel,p.shadow,p.highlight,fill,depth,drop_shadow);
    struct Raster { decltype(key) key; std::vector<uint32_t> pixels; };
    // Bounded, UI-thread-local colored rasters. Palette colors and animation
    // depth are part of the key, including across theme/state changes.
    static thread_local std::deque<Raster> cache;
    auto found=std::find_if(cache.begin(),cache.end(),[&](const Raster& item) { return item.key==key; });
    if(found==cache.end()) {
        const auto& m=MaskFor(width,height,corner,pad,Scaled(2,scale));
        std::vector<uint32_t> pixels(static_cast<size_t>(width)*height);
        const auto sample=[&](int x,int y) { return x<0 || y<0 || x>=width || y>=height ? 0.0f : m.blurred[static_cast<size_t>(y)*width+x]; };
        const float raised=std::max(0.0f,depth), inset=std::max(0.0f,-depth);
        const int offset_x=drop_shadow ? Scaled(3,scale) : m.offset;
        for(int y=0; y<height; ++y) for(int x=0; x<width; ++x) {
            const size_t i=static_cast<size_t>(y)*width+x;
            const float face=m.face[i], ul=sample(x+offset_x,y+m.offset), lr=sample(x-offset_x,y-m.offset);
            COLORREF c=Mix(p.panel,p.shadow,lr*0.62f*raised);
            if(!drop_shadow) { c=Mix(c,p.highlight,ul*0.85f*raised); }
            COLORREF inside=Mix(fill,p.shadow,(1-lr)*0.66f*inset);
            inside=Mix(inside,p.highlight,(1-ul)*0.88f*inset);
            c=Mix(c,inside,face);
            pixels[i]=(static_cast<uint32_t>(GetRValue(c))<<16)|(static_cast<uint32_t>(GetGValue(c))<<8)|GetBValue(c);
        }
        if(cache.size()==32) { cache.pop_front(); }
        cache.push_back({key,std::move(pixels)});
        found=std::prev(cache.end());
    }
    const auto& pixels=found->pixels;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    if(width==destination_width) {
        ::SetDIBitsToDevice(dc,rect.left-pad,rect.top-pad,width,height,0,0,0,height,pixels.data(),&info,DIB_RGB_COLORS);
    } else {
        const int previous=::SetStretchBltMode(dc,COLORONCOLOR);
        ::StretchDIBits(dc,rect.left-pad,rect.top-pad,cap,height,0,0,cap,height,pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
        ::StretchDIBits(dc,rect.left-pad+cap,rect.top-pad,destination_width-2*cap,height,cap,0,1,height,pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
        ::StretchDIBits(dc,rect.right+pad-cap,rect.top-pad,cap,height,width-cap,0,cap,height,pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
        if(previous) { ::SetStretchBltMode(dc,previous); }
    }
}

void DrawGrip(HDC dc, const RECT& rect, float scale) {
    const int dot = Scaled(2, scale);
    const int gap = Scaled(5, scale);
    const LONG x = (rect.left + rect.right) / 2 - gap / 2;
    const LONG y = (rect.top + rect.bottom) / 2 - gap;
    for (int row = 0; row != 3; ++row) {
        for (int col = 0; col != 2; ++col) {
            FillRounded(dc, {x + col * gap, y + row * gap, x + col * gap + dot, y + row * gap + dot}, dot, Current().muted_text);
        }
    }
}

int Scaled(float dip, float scale) noexcept {
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(dip) * static_cast<double>(scale))));
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    if (dc == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    HBRUSH brush = ::CreateSolidBrush(color);
    if (brush == nullptr) {
        return;
    }
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);
}

void FrameSolid(HDC dc, const RECT& rect, COLORREF color, int thickness) {
    if (dc == nullptr || thickness <= 0) {
        return;
    }
    const LONG t = thickness;
    FillSolid(dc, RECT{rect.left, rect.top, rect.right, rect.top + t}, color);
    FillSolid(dc, RECT{rect.left, rect.bottom - t, rect.right, rect.bottom}, color);
    FillSolid(dc, RECT{rect.left, rect.top + t, rect.left + t, rect.bottom - t}, color);
    FillSolid(dc, RECT{rect.right - t, rect.top + t, rect.right, rect.bottom - t}, color);
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF fill) {
    FillRoundedBordered(dc, rect, radius, fill, fill, 1);
}

namespace {
struct VectorRuntime {
    ULONG_PTR token{};
    VectorRuntime() { Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&token,&input,nullptr); }
    ~VectorRuntime() { if(token) { Gdiplus::GdiplusShutdown(token); } }
};
bool VectorsReady() { static VectorRuntime runtime; return runtime.token!=0; }
Gdiplus::Color VectorColor(COLORREF color) { return {255,GetRValue(color),GetGValue(color),GetBValue(color)}; }
void ConfigureVectors(Gdiplus::Graphics& graphics, HDC dc) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // Honor the caller's disjoint damage region explicitly in vector paints,
    // preventing antialiased blending outside the region being repainted.
    HRGN region = ::CreateRectRgn(0,0,0,0);
    if (::GetClipRgn(dc,region) == 1) {
        Gdiplus::Region clip(region);
        graphics.SetClip(&clip,Gdiplus::CombineModeIntersect);
    }
    ::DeleteObject(region);
}
void RoundedPath(Gdiplus::GraphicsPath& path,const RECT& r,int radius) {
    const float x=r.left+0.5f,y=r.top+0.5f,w=static_cast<float>(r.right-r.left-1),h=static_cast<float>(r.bottom-r.top-1);
    const float d=std::max(1.0f,std::min({radius*2.0f,w,h}));
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90);
    path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90); path.CloseFigure();
}
}
void FrameRounded(HDC dc,const RECT& rect,int radius,COLORREF color,int thickness) {
    if(!dc || rect.right<=rect.left || rect.bottom<=rect.top || !VectorsReady()) { return; }
    Gdiplus::Graphics graphics(dc); ConfigureVectors(graphics,dc);
    Gdiplus::GraphicsPath path; RoundedPath(path,rect,radius);
    Gdiplus::Pen pen(VectorColor(color),static_cast<float>(thickness)); graphics.DrawPath(&pen,&path);
}
void FillRoundedBordered(HDC dc,const RECT& rect,int radius,COLORREF fill,COLORREF border,int thickness) {
    if(!dc || rect.right<=rect.left || rect.bottom<=rect.top || !VectorsReady()) { return; }
    Gdiplus::Graphics graphics(dc); ConfigureVectors(graphics,dc);
    Gdiplus::GraphicsPath path; RoundedPath(path,rect,radius);
    Gdiplus::SolidBrush brush(VectorColor(fill)); graphics.FillPath(&brush,&path);
    Gdiplus::Pen pen(VectorColor(border),static_cast<float>(thickness)); graphics.DrawPath(&pen,&path);
}
void DrawSolidLine(HDC dc,POINT from,POINT to,COLORREF color,int thickness) {
    if(!dc || !VectorsReady()) { return; }
    Gdiplus::Graphics graphics(dc); ConfigureVectors(graphics,dc);
    Gdiplus::Pen pen(VectorColor(color),static_cast<float>(thickness));
    pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawLine(&pen,static_cast<int>(from.x),static_cast<int>(from.y),static_cast<int>(to.x),static_cast<int>(to.y));
}

void DrawLabel(HDC dc, const RECT& rect, const wchar_t* text, COLORREF color, HFONT font, UINT format) {
    if (dc == nullptr || text == nullptr || *text == L'\0') {
        return;
    }
    HGDIOBJ previous_font = (font != nullptr) ? ::SelectObject(dc, font) : nullptr;
    const int previous_mode = ::SetBkMode(dc, TRANSPARENT);
    const COLORREF previous_color = ::SetTextColor(dc, color);
    RECT box = rect;
    ::DrawTextW(dc, text, -1, &box, format);
    ::SetTextColor(dc, previous_color);
    ::SetBkMode(dc, previous_mode);
    if (previous_font != nullptr) {
        ::SelectObject(dc, previous_font);
    }
}

HFONT CreateUiFont(float size_dip, float scale, bool semi_bold) {
    LOGFONTW desc{};
    desc.lfHeight = -Scaled(size_dip, scale);
    desc.lfWeight = semi_bold ? FW_SEMIBOLD : FW_NORMAL;
    desc.lfCharSet = DEFAULT_CHARSET;
    desc.lfOutPrecision = OUT_TT_PRECIS;
    desc.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    desc.lfQuality = CLEARTYPE_QUALITY;
    desc.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    ::lstrcpynW(desc.lfFaceName, L"Segoe UI", LF_FACESIZE);
    return ::CreateFontIndirectW(&desc);
}

}  // namespace easnap::ui::theme
