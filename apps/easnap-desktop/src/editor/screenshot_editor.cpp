// Easnap - screenshot annotation editor implementation.
#include "editor/screenshot_editor.h"
#include "ui/capture_workspace.h"
#include "ui/native_controls.h"
#include <commctrl.h>

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <deque>
#include <cwctype>

namespace easnap::editor {
namespace {

constexpr wchar_t kEditorClassName[] = L"EasnapScreenshotEditor";
constexpr wchar_t kEditorTitle[] = L"Easnap Editor";

/// Native movable tool island; image planes belong to the capture workspace.
constexpr DWORD kEditorStyle = WS_POPUP | WS_CLIPCHILDREN;
constexpr DWORD kEditorExStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT;

// Toolbar metrics in device-independent pixels.
constexpr float kToolbarRowHeight = 48.0f;
constexpr float kToolButton = 34.0f;
constexpr float kCommandButton = 34.0f;
constexpr float kSwatch = 32.0f;
constexpr float kItemGap = 4.0f;
constexpr float kGroupGap = 14.0f;
constexpr float kEdgePadding = 10.0f;
constexpr float kCanvasGap = 8.0f;

// Smallest useful window, in device-independent pixels of the client area.
constexpr float kMinClientWidth = 460.0f;
constexpr float kMinClientHeight = kToolbarRowHeight;

/// Preferred tool island width, clamped to the current monitor.
constexpr float kIslandWidth = 780.0f;
constexpr float kIslandGrip = 24.0f;

enum CommandId {
    kCommandUndo = 1,
    kCommandRedo,
    kCommandClear,
    kCommandCopy,
    kCommandSave,
    kCommandCancel,
    kCommandTheme,
};

constexpr std::array<D2D1_COLOR_F, 8> kPaletteColors = {{
    {0.898f, 0.243f, 0.259f, 1.0f},  // red
    {0.976f, 0.639f, 0.157f, 1.0f},  // amber
    {0.188f, 0.686f, 0.427f, 1.0f},  // green
    {0.161f, 0.635f, 1.0f, 1.0f},    // Easnap blue
    {0.612f, 0.318f, 0.851f, 1.0f},  // violet
    {1.0f, 1.0f, 1.0f, 1.0f},        // white
    {0.0f, 0.0f, 0.0f, 1.0f},        // black
    {0.5f, 0.5f, 0.5f, 1.0f},        // gray
}};

constexpr std::array<float, 3> kThicknessOptions = {2.0f, 4.0f, 7.0f};

/// Mosaic brush diameters in captured pixels (independent of the preview zoom).
constexpr std::array<float, 3> kBrushSizeOptions = {16.0f, 32.0f, 64.0f};

/// Round caps/joins make freehand strokes and arrows look continuous.
ID2D1StrokeStyle* RoundStrokeStyle() {
    static platform::ComPtr<ID2D1StrokeStyle> style = [] {
        platform::ComPtr<ID2D1StrokeStyle> created;
        auto* factory = platform::GraphicsEnvironment::Get().d2d_factory();
        if (factory != nullptr) {
            const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                D2D1_DASH_STYLE_SOLID, 0.0f);
            factory->CreateStrokeStyle(properties, nullptr, 0, &created);
        }
        return created;
    }();
    return style.Get();
}

D2D1_RECT_F NormalizedD2DRect(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    return D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
}

D2D1_RECT_F ToD2DRect(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

/// Grows `rect` by the non-client frame of `style`. Uses the per-monitor aware
/// entry point when the running build of Windows offers it (capability probing,
/// never an OS version check) and falls back to the classic API otherwise.
void AdjustForNonClient(RECT& rect, UINT dpi) {
    using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const AdjustForDpiFn adjust_for_dpi = [] {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        return user32 == nullptr ? nullptr
                                 : reinterpret_cast<AdjustForDpiFn>(reinterpret_cast<void*>(
                                       ::GetProcAddress(user32, "AdjustWindowRectExForDpi")));
    }();
    if (adjust_for_dpi != nullptr && adjust_for_dpi(&rect, kEditorStyle, FALSE, kEditorExStyle, dpi) != FALSE) {
        return;
    }
    ::AdjustWindowRectEx(&rect, kEditorStyle, FALSE, kEditorExStyle);
}

/// Per-pixel "how many times did the mosaic brush pass here" map.
///
/// Every stroke stamps circular dabs along its path. A pixel can only gain one
/// level per stroke (the `stamp` array remembers the stroke that last touched
/// it), so a slow drag does not accidentally darken more than a fast one - only
/// painting over an area *again* does.
class CoverageMap {
public:
    void Reset(int width, int height) {
        width_ = width;
        height_ = height;
        level_.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
        stamp_.assign(level_.size(), 0);
        stroke_ = 0;
        max_level_ = 0;
        bounds_ = RECT{width, height, 0, 0};
    }

    void BeginStroke() {
        ++stroke_;
        bounds_ = RECT{width_, height_, 0, 0};
    }
    void CurrentMask(std::vector<uint8_t>& mask) const {
        std::fill(mask.begin(), mask.end(), uint8_t{0});
        for (LONG y = bounds_.top; y < bounds_.bottom; ++y) {
            for (LONG x = bounds_.left; x < bounds_.right; ++x) {
                const size_t i = static_cast<size_t>(y) * width_ + x;
                if (stamp_[i] == stroke_) { mask[i] = level_[i]; }
            }
        }
    }

    /// Stamps a filled circle of `radius` pixels centred on (cx, cy).
    void Dab(float cx, float cy, float radius) {
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
        const int x1 = std::min(width_ - 1, static_cast<int>(std::ceil(cx + radius)));
        const int y1 = std::min(height_ - 1, static_cast<int>(std::ceil(cy + radius)));
        const float radius_sq = radius * radius;
        for (int y = y0; y <= y1; ++y) {
            const float dy = static_cast<float>(y) + 0.5f - cy;
            for (int x = x0; x <= x1; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - cx;
                if (dx * dx + dy * dy > radius_sq) {
                    continue;
                }
                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x);
                if (stamp_[index] == stroke_) {
                    continue;
                }
                stamp_[index] = stroke_;
                if (level_[index] < 255) {
                    ++level_[index];
                }
                max_level_ = std::max(max_level_, level_[index]);
                bounds_.left = std::min(bounds_.left, static_cast<LONG>(x));
                bounds_.top = std::min(bounds_.top, static_cast<LONG>(y));
                bounds_.right = std::max(bounds_.right, static_cast<LONG>(x) + 1);
                bounds_.bottom = std::max(bounds_.bottom, static_cast<LONG>(y) + 1);
            }
        }
    }

    /// Walks the polyline and stamps overlapping dabs so the trail is solid.
    void Stroke(const std::vector<D2D1_POINT_2F>& points, float radius) {
        if (points.empty()) {
            return;
        }
        BeginStroke();
        const float step = std::max(1.0f, radius * 0.4f);
        Dab(points.front().x, points.front().y, radius);
        for (size_t i = 1; i < points.size(); ++i) {
            const D2D1_POINT_2F a = points[i - 1];
            const D2D1_POINT_2F b = points[i];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            const int segments = std::max(1, static_cast<int>(std::ceil(length / step)));
            for (int s = 1; s <= segments; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(segments);
                Dab(a.x + dx * t, a.y + dy * t, radius);
            }
        }
    }

    [[nodiscard]] const std::vector<uint8_t>& level() const noexcept { return level_; }
    [[nodiscard]] uint8_t max_level() const noexcept { return max_level_; }
    [[nodiscard]] const RECT& bounds() const noexcept { return bounds_; }
    [[nodiscard]] bool empty() const noexcept { return max_level_ == 0; }

private:
    int width_{0};
    int height_{0};
    std::vector<uint8_t> level_;
    std::vector<uint32_t> stamp_;
    uint32_t stroke_{0};
    uint8_t max_level_{0};
    RECT bounds_{};
};

void DrawArrow(ID2D1RenderTarget* target, const Annotation& annotation, ID2D1SolidColorBrush* brush) {
    const D2D1_POINT_2F start = annotation.points.front();
    const D2D1_POINT_2F end = annotation.points.back();
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f) {
        return;
    }
    const float ux = dx / length;
    const float uy = dy / length;
    const float head_length = std::min(length, std::max(14.0f, annotation.style.thickness * 5.0f));
    const float head_width = head_length * 0.62f;
    const D2D1_POINT_2F base = D2D1::Point2F(end.x - ux * head_length, end.y - uy * head_length);

    target->DrawLine(start, base, brush, annotation.style.thickness, RoundStrokeStyle());

    platform::ComPtr<ID2D1PathGeometry> geometry;
    auto* factory = platform::GraphicsEnvironment::Get().d2d_factory();
    if (factory == nullptr || FAILED(factory->CreatePathGeometry(&geometry))) {
        return;
    }
    platform::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) {
        return;
    }
    const D2D1_POINT_2F left = D2D1::Point2F(base.x - uy * head_width * 0.5f, base.y + ux * head_width * 0.5f);
    const D2D1_POINT_2F right = D2D1::Point2F(base.x + uy * head_width * 0.5f, base.y - ux * head_width * 0.5f);
    sink->BeginFigure(end, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(left);
    sink->AddLine(right);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    target->FillGeometry(geometry.Get(), brush);
}

/// Draws a freehand path with round joins, or a single dot for a lone click.
void DrawFreehand(ID2D1RenderTarget* target, const std::vector<D2D1_POINT_2F>& points, ID2D1SolidColorBrush* brush,
                  float thickness) {
    if (points.size() == 1) {
        target->FillEllipse(D2D1::Ellipse(points.front(), thickness * 0.5f, thickness * 0.5f), brush);
        return;
    }
    for (size_t i = 1; i < points.size(); ++i) {
        target->DrawLine(points[i - 1], points[i], brush, thickness, RoundStrokeStyle());
    }
}

/// Draws one annotation in image space. Mosaic strokes are baked into the base
/// bitmap beforehand, so they only render a translucent trail while being drawn.
ComPtr<IDWriteTextLayout> TextLayout(const Annotation& a);

// DirectWrite supplies glyph outlines, so Outline is a glyph stroke, never a
// selection rectangle or a bitmap dilation of the whole label.
class OutlineRenderer final : public IDWriteTextRenderer {
public:
    OutlineRenderer(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,D2D1_COLOR_F outline,D2D1_COLOR_F fill,float width)
        : target_(target),brush_(brush),outline_(outline),fill_(fill),width_(width) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result) { return E_POINTER; } *result=nullptr;
        if(iid==__uuidof(IUnknown) || iid==__uuidof(IDWritePixelSnapping) || iid==__uuidof(IDWriteTextRenderer)) {
            *result=static_cast<IDWriteTextRenderer*>(this); AddRef(); return S_OK;
        } return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* disabled) override { *disabled=FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* transform) override {
        D2D1_MATRIX_3X2_F matrix; target_->GetTransform(&matrix); std::memcpy(transform,&matrix,sizeof(matrix)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* pixels) override { *pixels=1; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*) override {
        ComPtr<ID2D1PathGeometry> geometry; ComPtr<ID2D1GeometrySink> sink;
        auto* factory=platform::GraphicsEnvironment::Get().d2d_factory();
        HRESULT hr=factory->CreatePathGeometry(&geometry); if(FAILED(hr)) { return hr; }
        hr=geometry->Open(&sink); if(FAILED(hr)) { return hr; }
        hr=run->fontFace->GetGlyphRunOutline(run->fontEmSize,run->glyphIndices,run->glyphAdvances,run->glyphOffsets,
            run->glyphCount,run->isSideways,run->bidiLevel%2,sink.Get());
        if(FAILED(hr)) { return hr; } hr=sink->Close(); if(FAILED(hr)) { return hr; }
        D2D1_MATRIX_3X2_F original; target_->GetTransform(&original);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(x,y)*original);
        brush_->SetColor(outline_); target_->DrawGeometry(geometry.Get(),brush_,width_,RoundStrokeStyle());
        brush_->SetColor(fill_); target_->FillGeometry(geometry.Get(),brush_);
        target_->SetTransform(original); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT x,FLOAT y,const DWRITE_UNDERLINE* line,IUnknown*) override {
        brush_->SetColor(fill_); target_->FillRectangle({x,y+line->offset,x+line->width,y+line->offset+line->thickness},brush_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT x,FLOAT y,const DWRITE_STRIKETHROUGH* line,IUnknown*) override {
        brush_->SetColor(fill_); target_->FillRectangle({x,y+line->offset,x+line->width,y+line->offset+line->thickness},brush_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context,FLOAT x,FLOAT y,IDWriteInlineObject* object,BOOL sideways,BOOL rtl,IUnknown* effect) override {
        return object->Draw(context,this,x,y,sideways,rtl,effect);
    }
private:
    ID2D1RenderTarget* target_; ID2D1SolidColorBrush* brush_; D2D1_COLOR_F outline_,fill_; float width_;
};

void DrawAnnotation(ID2D1RenderTarget* target, const Annotation& annotation,
                    ID2D1SolidColorBrush* brush, bool preview) {
    if (annotation.points.empty() || brush == nullptr) {
        return;
    }
    brush->SetColor(annotation.style.color);
    const float thickness = annotation.style.thickness;

    switch (annotation.tool) {
        case ToolId::Pen:
            DrawFreehand(target, annotation.points, brush, thickness);
            break;
        case ToolId::Arrow:
            if (annotation.points.size() >= 2) {
                DrawArrow(target, annotation, brush);
            }
            break;
        case ToolId::Rectangle:
            if (annotation.points.size() >= 2) {
                target->DrawRectangle(NormalizedD2DRect(annotation.points.front(), annotation.points.back()), brush,
                                      thickness);
            }
            break;
        case ToolId::Ellipse:
            if (annotation.points.size() >= 2) {
                const D2D1_RECT_F r = NormalizedD2DRect(annotation.points.front(), annotation.points.back());
                target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f),
                                                  (r.right - r.left) * 0.5f, (r.bottom - r.top) * 0.5f),
                                    brush, thickness);
            }
            break;
        case ToolId::Line:
            if(annotation.points.size()>=2) { target->DrawLine(annotation.points.front(),annotation.points.back(),brush,thickness,RoundStrokeStyle()); }
            break;
        case ToolId::Text:
        case ToolId::Tag: {
            auto layout=TextLayout(annotation); if(!layout || annotation.text.empty()) { break; }
            const auto box=annotation.bounds();
            auto origin=annotation.points.front();
            const float em=annotation.FontPixels();
            const bool white=annotation.style.color.r>0.98f && annotation.style.color.g>0.98f && annotation.style.color.b>0.98f;
            const auto ink=D2D1::ColorF(white ? D2D1::ColorF::Black : D2D1::ColorF::White);
            if(annotation.tool==ToolId::Tag) {
                const float dot=em*0.16f, body=origin.x+em*0.9f;
                brush->SetColor(annotation.style.color);
                target->FillEllipse(D2D1::Ellipse({origin.x+dot, (box.top+box.bottom)/2},dot,dot),brush);
                brush->SetColor(D2D1::ColorF(0x30343c));
                target->FillRoundedRectangle(D2D1::RoundedRect({body,box.top,box.right,box.bottom},em*0.2f,em*0.2f),brush);
                ComPtr<ID2D1PathGeometry> notch; ComPtr<ID2D1GeometrySink> sink;
                if(SUCCEEDED(platform::GraphicsEnvironment::Get().d2d_factory()->CreatePathGeometry(&notch)) && SUCCEEDED(notch->Open(&sink))) {
                    const float mid=(box.top+box.bottom)/2;
                    sink->BeginFigure({body-em*0.25f,mid},D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({body+1,mid-em*0.25f}); sink->AddLine({body+1,mid+em*0.25f});
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close(); target->FillGeometry(notch.Get(),brush);
                }
                origin={body+em*0.45f,origin.y+em*0.45f}; brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
                target->DrawTextLayout(origin,layout.Get(),brush); break;
            }
            if(annotation.style.text_mode==TextMode::Fill) {
                target->FillRoundedRectangle(D2D1::RoundedRect(box,em*0.22f,em*0.22f),brush);
                origin.x+=em*0.25f; origin.y+=em*0.25f; brush->SetColor(ink);
            }
            if(annotation.style.text_mode==TextMode::Outline) {
                origin.x+=em*0.075f; origin.y+=em*0.075f;
                OutlineRenderer renderer(target,brush,annotation.style.color,ink,em*0.12f);
                layout->Draw(nullptr,&renderer,origin.x,origin.y);
            } else { target->DrawTextLayout(origin,layout.Get(),brush); }
            break;
        }
        case ToolId::Watermark: {
            auto layout=TextLayout(annotation); if(!layout || annotation.text.empty() || annotation.points.size()<2) { break; }
            DWRITE_TEXT_METRICS metrics{}; layout->GetMetrics(&metrics);
            const float stepX=std::max(annotation.FontPixels()*5,metrics.width+annotation.FontPixels()*3);
            const float stepY=std::max(annotation.FontPixels()*4,metrics.height*3);
            D2D1_MATRIX_3X2_F original; target->GetTransform(&original);
            auto color=annotation.style.color; color.a=annotation.style.opacity/100; brush->SetColor(color);
            target->PushAxisAlignedClip({0,0,annotation.points.back().x,annotation.points.back().y},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const D2D1_POINT_2F center{annotation.points.back().x/2,annotation.points.back().y/2};
            const float reach=std::hypot(center.x,center.y)+std::max(stepX,stepY);
            target->SetTransform(D2D1::Matrix3x2F::Rotation(-18,center)*original);
            const int columns=static_cast<int>(std::ceil(reach/stepX)),rows=static_cast<int>(std::ceil(reach/stepY));
            for(int row=-rows;row<=rows;++row) for(int column=-columns;column<=columns;++column) {
                const D2D1_POINT_2F origin{center.x+column*stepX-metrics.widthIncludingTrailingWhitespace/2,
                    center.y+row*stepY-metrics.height/2};
                target->DrawTextLayout(origin,layout.Get(),brush);
            }
            target->SetTransform(original); target->PopAxisAlignedClip(); break;
        }
        case ToolId::Spotlight: break; // Union dim layer is derived with the base pixels.
        case ToolId::Mosaic: {
            if (!preview) {
                break;  // Already destructive on the pixels.
            }
            brush->SetColor(D2D1::ColorF(0.85f, 0.89f, 0.98f, 0.45f));
            DrawFreehand(target, annotation.points, brush, std::max(2.0f, annotation.style.brush_size));
            break;
        }
        case ToolId::Pointer:
        default:
            break;
    }
}

}  // namespace

// Vector-only edits reuse the derived pixels; edits to effects rebuild them
// deterministically from the immutable capture and the ordered effect list.
struct ScreenshotEditor::RedactionCache { std::vector<Annotation> effects; };

// ---------------------------------------------------------------------------
// Annotation / AnnotationDocument
// ---------------------------------------------------------------------------

namespace {
ComPtr<IDWriteTextLayout> TextLayout(const Annotation& a) {
    struct Entry { std::wstring text; float size; ComPtr<IDWriteTextLayout> layout; };
    static thread_local std::deque<Entry> cache;
    const float size=a.FontPixels();
    for(const auto& e:cache) if(e.text==a.text && e.size==size) { return e.layout; }
    auto& graphics=platform::GraphicsEnvironment::Get();
    if(!graphics.EnsureInitialized()) { return {}; }
    auto format=graphics.CreateTextFormat(size,DWRITE_FONT_WEIGHT_NORMAL);
    if(!format) { return {}; }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if(FAILED(graphics.dwrite_factory()->CreateTextLayout(a.text.c_str(),static_cast<UINT32>(a.text.size()),format.Get(),
        65536,65536,&layout))) { return {}; }
    if(cache.size()==64) { cache.pop_front(); }
    cache.push_back({a.text,size,layout}); return layout;
}
float SegmentDistance(D2D1_POINT_2F p,D2D1_POINT_2F a,D2D1_POINT_2F b) {
    const float dx=b.x-a.x,dy=b.y-a.y;
    const float t=dx*dx+dy*dy>0 ? std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/(dx*dx+dy*dy),0.0f,1.0f) : 0;
    return std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy);
}
bool SameList(const std::vector<Annotation>& a,const std::vector<Annotation>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const auto& x,const auto& y){return x.Same(y);});
}
}

D2D1_RECT_F Annotation::bounds() const {
    if(points.empty()) { return {}; }
    auto box=NormalizedD2DRect(points.front(),points.front());
    for(const auto& point:points) {
        box.left=std::min(box.left,point.x); box.top=std::min(box.top,point.y);
        box.right=std::max(box.right,point.x); box.bottom=std::max(box.bottom,point.y);
    }
    if(tool==ToolId::Text || tool==ToolId::Tag) {
        auto layout=TextLayout(*this); DWRITE_TEXT_METRICS metrics{};
        if(layout) { layout->GetMetrics(&metrics); }
        const float padding=tool==ToolId::Tag ? FontPixels()*0.45f : style.text_mode==TextMode::Fill ? FontPixels()*0.25f : style.text_mode==TextMode::Outline ? FontPixels()*0.075f : 0;
        box.right=box.left+std::max(1.0f,metrics.widthIncludingTrailingWhitespace)+padding*2;
        box.bottom=box.top+std::max(FontPixels(),metrics.height)+padding*2;
        if(tool==ToolId::Tag) { box.right+=FontPixels()*0.9f; }
        return box;
    }
    const float pad=tool==ToolId::Mosaic ? (style.method==EffectMethod::Brush ? style.brush_size/2 : 0) :
        tool==ToolId::Spotlight ? 0 : tool==ToolId::Arrow ? style.thickness*3 : style.thickness/2;
    box.left-=pad; box.top-=pad; box.right+=pad; box.bottom+=pad;
    return box;
}

bool Annotation::Same(const Annotation& b) const {
    const auto& x=style; const auto& y=b.style;
    return tool==b.tool && text==b.text && points.size()==b.points.size() &&
        x.color.r==y.color.r && x.color.g==y.color.g && x.color.b==y.color.b && x.color.a==y.color.a &&
        x.thickness==y.thickness && x.font_size==y.font_size && x.image_scale==y.image_scale &&
        x.brush_size==y.brush_size && x.text_mode==y.text_mode && x.effect==y.effect && x.method==y.method &&
        x.shape==y.shape && x.strength==y.strength && x.opacity==y.opacity &&
        std::equal(points.begin(),points.end(),b.points.begin(),[](auto a,auto c){ return a.x==c.x && a.y==c.y; });
}

bool Annotation::HitTest(D2D1_POINT_2F p,float tolerance) const {
    if(points.empty() || tool==ToolId::Watermark) { return false; }
    const auto r=bounds();
    if(p.x<r.left-tolerance || p.x>r.right+tolerance || p.y<r.top-tolerance || p.y>r.bottom+tolerance) { return false; }
    // Text remains directly editable. Closed geometry only responds at its
    // outline, so another annotation can start inside an existing shape.
    if(tool==ToolId::Text || tool==ToolId::Tag) { return true; }
    if(tool==ToolId::Arrow && points.size()>=2) {
        const auto a=points.front(),b=points.back();
        const float dx=b.x-a.x,dy=b.y-a.y,length=std::hypot(dx,dy);
        if(length<1) { return false; }
        const float ux=dx/length,uy=dy/length;
        const float head=std::min(length,std::max(14.0f,style.thickness*5));
        const D2D1_POINT_2F base{b.x-ux*head,b.y-uy*head};
        const D2D1_POINT_2F left{base.x-uy*head*.31f,base.y+ux*head*.31f};
        const D2D1_POINT_2F right{base.x+uy*head*.31f,base.y-ux*head*.31f};
        return SegmentDistance(p,a,base)<=style.thickness/2+tolerance ||
            SegmentDistance(p,b,left)<=tolerance || SegmentDistance(p,left,right)<=tolerance || SegmentDistance(p,right,b)<=tolerance;
    }
    if(tool==ToolId::Line || tool==ToolId::Pen || (tool==ToolId::Mosaic && style.method==EffectMethod::Brush)) {
        float distance=std::hypot(p.x-points[0].x,p.y-points[0].y);
        for(size_t i=1;i<points.size();++i) { distance=std::min(distance,SegmentDistance(p,points[i-1],points[i])); }
        if(tool==ToolId::Mosaic) { return std::abs(distance-style.brush_size/2)<=tolerance; }
        return distance<=style.thickness/2+tolerance;
    }
    const auto box=NormalizedD2DRect(points.front(),points.back());
    const float margin=tolerance+((tool==ToolId::Rectangle || tool==ToolId::Ellipse) ? style.thickness/2:0);
    if(tool==ToolId::Ellipse || (tool==ToolId::Spotlight && style.shape==SpotlightShape::Ellipse)) {
        const float rx=(box.right-box.left)/2,ry=(box.bottom-box.top)/2;
        const float dx=p.x-(box.left+box.right)/2,dy=p.y-(box.top+box.bottom)/2;
        const auto ellipse=[&](float x,float y) { return x>0 && y>0 && dx*dx/(x*x)+dy*dy/(y*y)<1; };
        return ellipse(rx+margin,ry+margin) && !ellipse(rx-margin,ry-margin);
    }
    if(tool==ToolId::Spotlight && style.shape==SpotlightShape::Rounded) {
        const float rx=(box.right-box.left)/2,ry=(box.bottom-box.top)/2;
        const float radius=std::min({rx,ry,16*style.image_scale});
        const float dx=std::abs(p.x-(box.left+rx))-(rx-radius),dy=std::abs(p.y-(box.top+ry))-(ry-radius);
        const float distance=std::hypot(std::max(dx,0.0f),std::max(dy,0.0f))+std::min(std::max(dx,dy),0.0f)-radius;
        return std::abs(distance)<=margin;
    }
    const D2D1_POINT_2F tl{box.left,box.top},tr{box.right,box.top},br{box.right,box.bottom},bl{box.left,box.bottom};
    return std::min({SegmentDistance(p,tl,tr),SegmentDistance(p,tr,br),SegmentDistance(p,br,bl),SegmentDistance(p,bl,tl)})<=margin;
}

std::vector<D2D1_POINT_2F> Annotation::Handles() const {
    if(points.size()<2) { return {}; }
    if(tool==ToolId::Line || tool==ToolId::Arrow) { return {points.front(),points.back()}; }
    if(tool==ToolId::Rectangle || tool==ToolId::Ellipse || tool==ToolId::Spotlight ||
        (tool==ToolId::Mosaic && style.method==EffectMethod::Region)) {
        const auto r=NormalizedD2DRect(points.front(),points.back());
        return {{r.left,r.top},{r.right,r.top},{r.right,r.bottom},{r.left,r.bottom}};
    }
    return {};
}

void AnnotationDocument::Checkpoint() {
    if(!transaction_) { undo_stack_.push_back(annotations_); redo_stack_.clear(); }
}
void AnnotationDocument::Apply(Annotation annotation) { Checkpoint(); annotations_.push_back(std::move(annotation)); ++revision_; }
void AnnotationDocument::Replace(size_t index,Annotation annotation) {
    if(index>=annotations_.size() || annotations_[index].Same(annotation)) { return; }
    Checkpoint(); annotations_[index]=std::move(annotation); ++revision_;
}
void AnnotationDocument::Remove(size_t index) {
    if(index>=annotations_.size()) { return; }
    Checkpoint(); annotations_.erase(annotations_.begin()+static_cast<ptrdiff_t>(index)); ++revision_;
}
void AnnotationDocument::BeginTransaction() { if(!transaction_) { transaction_=annotations_; } }
void AnnotationDocument::CommitTransaction() {
    if(!transaction_) { return; }
    if(!SameList(*transaction_,annotations_)) { undo_stack_.push_back(std::move(*transaction_)); redo_stack_.clear(); }
    transaction_.reset();
}
void AnnotationDocument::CancelTransaction() {
    if(!transaction_) { return; }
    if(!SameList(*transaction_,annotations_)) { annotations_=std::move(*transaction_); ++revision_; }
    transaction_.reset();
}
bool AnnotationDocument::Undo() {
    CancelTransaction(); if(undo_stack_.empty()) { return false; }
    redo_stack_.push_back(std::move(annotations_)); annotations_=std::move(undo_stack_.back()); undo_stack_.pop_back(); ++revision_; return true;
}
bool AnnotationDocument::Redo() {
    CancelTransaction(); if(redo_stack_.empty()) { return false; }
    undo_stack_.push_back(std::move(annotations_)); annotations_=std::move(redo_stack_.back()); redo_stack_.pop_back(); ++revision_; return true;
}
void AnnotationDocument::Clear() { if(annotations_.empty()) { return; } Checkpoint(); annotations_.clear(); ++revision_; }
void AnnotationDocument::Reset() {
    annotations_={}; undo_stack_={}; redo_stack_={}; transaction_.reset(); revision_=0;
}
bool AnnotationDocument::HasRedactions() const {
    return std::any_of(annotations_.begin(),annotations_.end(),[](const auto& a){ return a.tool==ToolId::Mosaic; });
}
std::optional<size_t> AnnotationDocument::HitTest(D2D1_POINT_2F point,float tolerance) const {
    for(size_t i=annotations_.size();i>0;--i) if(annotations_[i-1].HitTest(point,tolerance)) { return i-1; }
    return {};
}

// ---------------------------------------------------------------------------
// ScreenshotEditor - toolbar model
// ---------------------------------------------------------------------------

struct ScreenshotEditor::ToolbarItem {
    enum class Kind { Tool, Color, Thickness, BrushSize, Option, Command };

    Kind kind{Kind::Tool};
    ToolId tool{ToolId::Pointer};
    D2D1_COLOR_F color{};
    float thickness{0.0f};
    float brush_size{0.0f};
    int command{0};
    const wchar_t* label{L""};
    const wchar_t* tooltip{L""};
    D2D1_RECT_F rect{};
    int identifier{};
    bool shown{true};
};

ScreenshotEditor::ScreenshotEditor(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

ScreenshotEditor::~ScreenshotEditor() { Destroy(); }

bool ScreenshotEditor::CreateToolbarControls() {
    using namespace ui;
    constexpr std::array<Icon, 11> tools{Icon::Cursor, Icon::Pen, Icon::Arrow, Icon::Rectangle,
        Icon::Ellipse, Icon::Text, Icon::Mosaic, Icon::Line, Icon::Tag, Icon::Spotlight, Icon::Watermark};
    constexpr std::array<const wchar_t*, 8> colors{L"Red", L"Amber", L"Green", L"Blue", L"Violet", L"White", L"Black", L"Gray"};
    tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0,0,0,0, hwnd_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    StyleTooltip(tooltip_);
    for (const auto& item : toolbar_) {
        ButtonStyle style;
        std::wstring name = item.tooltip;
        style.icon_only = true;
        switch (item.kind) {
            case ToolbarItem::Kind::Tool:
                style.icon = tools[static_cast<size_t>(item.tool)];
                break;
            case ToolbarItem::Kind::Color:
                name = std::wstring(colors[static_cast<size_t>(item.identifier - 2100)]) + L" annotation color";
                style.swatch = true;
                style.content_color = RGB(static_cast<BYTE>(std::lround(item.color.r * 255)),
                    static_cast<BYTE>(std::lround(item.color.g * 255)), static_cast<BYTE>(std::lround(item.color.b * 255)));
                break;
            case ToolbarItem::Kind::Thickness:
                name = L"Line thickness " + std::to_wstring(static_cast<int>(item.thickness)) + L" pixels";
                style.sample = 2; style.sample_size = kBrushSizeOptions[static_cast<size_t>(item.identifier-2200)];
                break;
            case ToolbarItem::Kind::BrushSize:
                name = L"Mosaic brush " + std::to_wstring(static_cast<int>(item.brush_size)) + L" pixels";
                style.sample = 2; style.sample_size = item.brush_size;
                break;
            case ToolbarItem::Kind::Option:
                style.segmented=true;
                if(item.identifier==2500) { style.icon=Icon::Text; }
                if(item.identifier==2501) { style.icon=Icon::TextFill; }
                if(item.identifier==2502) { style.icon=Icon::TextOutline; }
                if(item.identifier==2510) { style.icon=Icon::Droplet; }
                if(item.identifier==2511) { style.icon=Icon::Mosaic; }
                if(item.identifier==2520) { style.icon=Icon::Hand; style.dropdown=true; }
                if(item.identifier==2521) { style.icon=Icon::Region; }
                if(item.identifier==2530) { style.icon=Icon::Rectangle; }
                if(item.identifier==2531) { style.icon=Icon::Ellipse; }
                if(item.identifier==2532) { style.icon=Icon::RoundedRectangle; }
                break;
            case ToolbarItem::Kind::Command:
                style.icon_only = true;
                if (item.command == kCommandUndo) { style.icon = Icon::Undo; name = L"Undo (Ctrl+Z)"; }
                if (item.command == kCommandRedo) { style.icon = Icon::Redo; name = L"Redo (Ctrl+Y)"; }
                if (item.command == kCommandClear) { style.icon = Icon::Clear; name = L"Clear annotations"; }
                if (item.command == kCommandCopy) { style.icon = Icon::Check; style.role = ButtonRole::Success; }
                if (item.command == kCommandSave) { style.icon = Icon::Save; name=L"Save (Ctrl+S)"; }
                if (item.command == kCommandCancel) { style.icon = Icon::Discard; name=L"Discard (Esc)"; }
                if (item.command == kCommandTheme) { style.icon = Icon::Appearance; name=L"Switch Light / Dark theme"; }
                break;
        }
        const DWORD type = item.kind == ToolbarItem::Kind::Command ? BS_PUSHBUTTON : BS_RADIOBUTTON | BS_PUSHLIKE;
        const bool first = item.identifier == 2000 || item.identifier == 2100 || item.identifier == 2200 ||
                           item.identifier == 2300 || item.identifier == 2401;
        const HWND parent=item.kind==ToolbarItem::Kind::Tool || item.kind==ToolbarItem::Kind::Command ? hwnd_ : item.kind==ToolbarItem::Kind::BrushSize ? brush_popup_:context_;
        HWND button = ::CreateWindowExW(0, L"BUTTON", name.c_str(), WS_CHILD | WS_TABSTOP | type | (first ? WS_GROUP : 0),
            0,0,1,1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(item.identifier)), ::GetModuleHandleW(nullptr), nullptr);
        if (!button || !StyleButton(button, style) ||
            !::SetWindowSubclass(button, ToolbarButtonProc, 1, reinterpret_cast<DWORD_PTR>(this))) { return false; }
        if (tooltip_) {
            TOOLINFOW info{sizeof(info)};
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = parent; info.uId = reinterpret_cast<UINT_PTR>(button);
            info.lpszText = name.data();
            ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
    }
    SyncToolbarControls();
    return true;
}

HWND ScreenshotEditor::Control(int id) const {
    if(id==2605) { return context_ ? ::GetDlgItem(::GetDlgItem(context_,2604),id):nullptr; }
    if(const auto child=::GetDlgItem(hwnd_,id)) { return child; }
    if(const auto child=context_ ? ::GetDlgItem(context_,id):nullptr) { return child; }
    return brush_popup_ ? ::GetDlgItem(brush_popup_,id):nullptr;
}
ToolId ScreenshotEditor::ContextTool() const {
    return selected_ && *selected_<document_.annotations().size() ? document_.annotations()[*selected_].tool : tool_;
}
AnnotationStyle ScreenshotEditor::ContextStyle() const {
    return selected_ && *selected_<document_.annotations().size() ? document_.annotations()[*selected_].style : style_;
}
void ScreenshotEditor::Select(std::optional<size_t> index) {
    selected_=index; if(selected_ && *selected_>=document_.annotations().size()) { selected_.reset(); }
    ReflowToolbar(); Invalidate();
}
void ScreenshotEditor::ChangeStyle(const AnnotationStyle& value) {
    const auto revision=document_.revision();
    if(selected_ && *selected_<document_.annotations().size()) {
        const bool shared_opacity=document_.annotations()[*selected_].tool==ToolId::Spotlight && document_.annotations()[*selected_].style.opacity!=value.opacity;
        const bool own_transaction=shared_opacity && !document_.in_transaction();
        if(own_transaction) { document_.BeginTransaction(); }
        if(shared_opacity) {
            for(size_t i=0;i<document_.annotations().size();++i) if(document_.annotations()[i].tool==ToolId::Spotlight) {
                auto region=document_.annotations()[i]; region.style.opacity=value.opacity; document_.Replace(i,std::move(region));
            }
        }
        auto annotation=document_.annotations()[*selected_]; annotation.style=value;
        document_.Replace(*selected_,std::move(annotation));
        if(own_transaction) { document_.CommitTransaction(); }
        if(editing_text_) { pending_=document_.annotations()[*selected_]; LayoutTextEditor(); }
    } else { style_=value; }
    SyncContext(false); Invalidate(); NotifyMutation(revision);
}
void ScreenshotEditor::CreateContextControls() {
    const auto make=[&](const wchar_t* cls,const wchar_t* name,DWORD style,int id) {
        HWND child=::CreateWindowExW(0,cls,name,WS_CHILD|WS_TABSTOP|style,0,0,1,1,context_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),::GetModuleHandleW(nullptr),nullptr);
        if(child) { ::SetWindowSubclass(child,PropertyProc,1,reinterpret_cast<DWORD_PTR>(this)); }
        if(child && ::lstrcmpW(cls,L"STATIC")==0 && !ui::StyleLabel(child)) { ::DestroyWindow(child); return static_cast<HWND>(nullptr); }
        return child;
    };
    HWND font=make(L"COMBOBOX",L"Font size in points",CBS_DROPDOWN|CBS_AUTOHSCROLL|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,2600);
    if(font) {
        for(const wchar_t* value:{L"12 pt",L"16 pt",L"24 pt",L"36 pt",L"48 pt",L"60 pt",L"72 pt",L"96 pt"}) { ::SendMessageW(font,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value)); }
        ::SendMessageW(font,CB_LIMITTEXT,10,0); static_cast<void>(ui::StyleInput(font));
        ui::SetAccessibleName(font,L"Font size in points");
    }
    for(int id:{2601,2602,2603}) {
        HWND slider=make(TRACKBAR_CLASSW,id==2601 ? L"Effect strength" : id==2602 ? L"Opacity percent" : L"Watermark size in points",TBS_HORZ|TBS_NOTICKS, id);
        ::SendMessageW(slider,TBM_SETRANGE,TRUE,MAKELPARAM(id==2603 ? 8:0,id==2603 ? 96:100));
        ::SendMessageW(slider,TBM_SETPAGESIZE,0,10); static_cast<void>(ui::StyleSlider(slider));
    }
    HWND text=make(L"EDIT",L"",ES_AUTOHSCROLL,2604);
    ::SendMessageW(text,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Max. 16 characters"));
    ::SendMessageW(text,EM_SETLIMITTEXT,16,0); static_cast<void>(ui::StyleInput(text));
    ui::SetAccessibleName(text,L"Watermark text");
    HWND clear=make(L"BUTTON",L"Clear watermark text",BS_PUSHBUTTON,2605);
    ui::ButtonStyle clear_style; clear_style.icon=ui::Icon::Discard; clear_style.icon_only=true; clear_style.segmented=true;
    clear_style.circular=true;
    static_cast<void>(ui::StyleButton(clear,clear_style));
    ::RemoveWindowSubclass(clear,PropertyProc,1);
    ::SetWindowSubclass(clear,PropertyProc,1,reinterpret_cast<DWORD_PTR>(this));
    ::SetParent(clear,text);
    ::SetWindowLongPtrW(text,GWL_STYLE,::GetWindowLongPtrW(text,GWL_STYLE)|WS_CLIPCHILDREN);
    for(int id:{2611,2612,2613}) { make(L"STATIC",L"",SS_CENTERIMAGE,id); }
}
void ScreenshotEditor::SyncWatermarkClear() {
    const HWND text=Control(2604),clear=Control(2605);
    if(!text || !clear) { return; }
    RECT field{}; ::GetWindowRect(text,&field);
    POINT cursor{}; ::GetCursorPos(&cursor);
    const bool over=::PtInRect(&field,cursor)!=FALSE;
    const bool show=!modal_active_ && ContextTool()==ToolId::Watermark && ::IsWindowVisible(context_) &&
        ::GetWindowTextLengthW(text)>0 && over;
    const bool shown=(::GetWindowLongPtrW(clear,GWL_STYLE)&WS_VISIBLE)!=0;
    if(shown!=show) {
        ::ShowWindow(clear,show ? SW_SHOWNOACTIVATE:SW_HIDE);
        if(show) { ::SetTimer(context_,0xEA63,40,nullptr); }
        else { ::KillTimer(context_,0xEA63); }
    }
}
LRESULT CALLBACK ScreenshotEditor::PropertyProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR reference) {
    auto* self=reinterpret_cast<ScreenshotEditor*>(reference);
    if(message==WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd,PropertyProc,id); return ::DefSubclassProc(hwnd,message,wparam,lparam); }
    const int control=::GetDlgCtrlID(hwnd);
    if(control==2604 && message==WM_COMMAND && LOWORD(wparam)==2605) { return self->HandleMessage(self->context_,message,wparam,lparam); }
    const bool clear_hover=(control==2604 || control==2605) &&
        (message==WM_MOUSEMOVE || message==WM_MOUSELEAVE || message==WM_SETFOCUS || message==WM_KILLFOCUS);
    if(clear_hover && message==WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; ::TrackMouseEvent(&track);
    }
    const bool slider=control>=2601 && control<=2603;
    if(!self->syncing_context_ && ((slider && (message==WM_LBUTTONDOWN || message==WM_KEYDOWN)) || (!slider && message==WM_SETFOCUS))) {
        self->document_.BeginTransaction();
    }
    if(message==WM_KEYDOWN && wparam==VK_ESCAPE) {
        self->document_.CancelTransaction(); self->Close(false); return 0;
    }
    const LRESULT result=::DefSubclassProc(hwnd,message,wparam,lparam);
    if(clear_hover) { self->SyncWatermarkClear(); }
    if((slider && (message==WM_LBUTTONUP || message==WM_KEYUP)) || (!slider && message==WM_KILLFOCUS)) { self->document_.CommitTransaction(); }
    if(slider && message==WM_CAPTURECHANGED && ::GetKeyState(VK_LBUTTON)<0) {
        self->document_.CancelTransaction(); self->SyncContext(); self->Invalidate();
    }
    return result;
}
void ScreenshotEditor::SyncContext(bool layout) {
    if(!context_ || syncing_context_) { return; }
    syncing_context_=true;
    const auto tool=ContextTool(); const auto style=ContextStyle();
    const bool text=tool==ToolId::Text || tool==ToolId::Tag,watermark=tool==ToolId::Watermark;
    const auto position=[&](int id,bool shown,float x,float y,float width,float height) {
        HWND child=Control(id); if(!child) { return; }
        if((::IsWindowVisible(child)!=FALSE)!=shown) { ::ShowWindow(child,shown ? SW_SHOWNA:SW_HIDE); }
        if(shown) {
            RECT old{}; ::GetWindowRect(child,&old); ::MapWindowPoints(nullptr,context_,reinterpret_cast<POINT*>(&old),2);
            const RECT next{static_cast<LONG>(x*scale_),static_cast<LONG>(y*scale_),static_cast<LONG>((x+width)*scale_),static_cast<LONG>((y+height)*scale_)};
            if(old.left!=next.left || old.top!=next.top || old.right!=next.right || (id!=2600 && old.bottom!=next.bottom)) {
                ::SetWindowPos(child,nullptr,next.left,next.top,next.right-next.left,next.bottom-next.top,SWP_NOZORDER|SWP_NOACTIVATE);
            }
        }
        if(reinterpret_cast<HFONT>(::SendMessageW(child,WM_GETFONT,0,0))!=toolbar_font_) { ::SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(toolbar_font_),TRUE); }
    };
    if(layout) for(int id:{2600,2601,2602,2603,2604,2611,2612,2613}) {
        const auto it=std::find_if(context_fields_.begin(),context_fields_.end(),[id](const auto& field){return field.first==id;});
        if(it==context_fields_.end()) { position(id,false,0,0,1,1); continue; }
        const auto r=it->second;
        position(id,true,r.left/scale_,r.top/scale_,(r.right-r.left)/scale_,id==2600 ? 280.0f:(r.bottom-r.top)/scale_);
    }
    const auto label=[&](int id,const wchar_t* value) {
        wchar_t old[256]{}; ::GetWindowTextW(Control(id),old,256);
        if(::lstrcmpW(old,value)!=0) { ::SetWindowTextW(Control(id),value); }
    };
    const auto slider=[&](int id,float value) {
        if(::SendMessageW(Control(id),TBM_GETPOS,0,0)!=static_cast<LPARAM>(value)) { ::SendMessageW(Control(id),TBM_SETPOS,TRUE,static_cast<LPARAM>(value)); }
    };
    if(text && ::GetFocus()!=Control(2600) && !::IsChild(Control(2600),::GetFocus())) {
        wchar_t value[32]{}; ::swprintf_s(value,L"%g pt",static_cast<double>(style.font_size)); label(2600,value);
    }
    if(watermark) {
        const auto& annotations=document_.annotations();
        const auto found=std::find_if(annotations.begin(),annotations.end(),[](const auto& a) { return a.tool==ToolId::Watermark; });
        // Undo may clear selection while the input still owns focus. Always
        // synchronize from the document; unchanged typing never resets EDIT.
        label(2604,found!=annotations.end() ? found->text.c_str() : L"");
    }
    if(watermark) {
        RECT field{}; ::GetClientRect(Control(2604),&field);
        const LONG size=ui::theme::Scaled(32,scale_),inset=ui::theme::Scaled(2,scale_);
        ::SetWindowPos(Control(2605),HWND_TOP,field.right-size-inset,field.top+(field.bottom-field.top-size)/2,size,size,SWP_NOACTIVATE);
        ::SendMessageW(Control(2604),EM_SETMARGINS,EC_RIGHTMARGIN,MAKELPARAM(0,size+inset*2));
    }
    SyncWatermarkClear();
    slider(2601,style.strength);
    slider(2602,style.opacity);
    slider(2603,style.font_size);
    const std::wstring strength=(style.effect==EffectMode::Blur ? L"Blur intensity ":L"Block size ")+std::to_wstring(static_cast<int>(style.effect==EffectMode::Blur ? style.strength : 2+style.strength*0.7f))+(style.effect==EffectMode::Blur ? L"%":L" px");
    label(2611,strength.c_str());
    label(2612,(L"Opacity "+std::to_wstring(static_cast<int>(style.opacity))+L"%").c_str());
    label(2613,(L"Size "+std::to_wstring(static_cast<int>(style.font_size))+L" pt").c_str());
    syncing_context_=false;
}
void ScreenshotEditor::PlaceContext(bool follow_frame) {
    if(!hwnd_ || !context_ || repositioning_ || modal_active_) { return; }
    repositioning_=true;
    RECT main{}; ::GetWindowRect(hwnd_,&main);
    const auto work=platform::MonitorForRect(follow_frame ? region_:main).work_area;
    const LONG gap=ui::theme::Scaled(8,scale_);
    const LONG width=main.right-main.left,height=main.bottom-main.top;
    const LONG popup_space=brush_popup_open_ ? gap+ui::theme::Scaled(kToolbarRowHeight+6,scale_):0;
    const LONG total=height+(context_visible_ ? gap+context_height_:0)+popup_space;
    LONG left=main.left,top=main.top;
    if(follow_frame) {
        left=region_.left+(platform::RectWidth(region_)-width)/2;
        top=region_.bottom+gap;
        if(top+total>work.bottom) { top=region_.top-gap-total; }
        const auto monitor=platform::MonitorForRect(region_);
        if(::EqualRect(&region_,&monitor.bounds)) { top=work.top+(platform::RectHeight(work)-total)/2; }
    }
    const RECT group=platform::ClampRectToBounds({left,top,left+width,top+total},work);
    ::SetWindowPos(hwnd_,nullptr,group.left,group.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    RECT context{group.left+(width-context_width_)/2,group.top+height+gap,0,0};
    context.right=context.left+context_width_; context.bottom=context.top+context_height_;
    context=platform::ClampRectToBounds(context,work);
    ::SetWindowPos(context_,HWND_TOPMOST,context.left,context.top,context_width_,std::max(1,context_height_),SWP_NOACTIVATE);
    ui::RoundWindow(context_); ui::RoundWindow(hwnd_);
    const bool show=visible_ && !frame_interacting_ && context_visible_;
    if(show && !::IsWindowVisible(context_) && ui::theme::AnimationsEnabled()) {
        context_fade_started_=::GetTickCount64(); ::SetLayeredWindowAttributes(context_,0,0,LWA_ALPHA); ::SetTimer(context_,0xEA61,15,nullptr);
    }
    if(!show) { ::KillTimer(context_,0xEA61); context_fade_started_=0; ::SetLayeredWindowAttributes(context_,0,255,LWA_ALPHA); }
    ::ShowWindow(context_,show ? SW_SHOWNOACTIVATE:SW_HIDE);
    if(brush_popup_open_) { ShowBrushPopup(true); }
    repositioning_=false;
}
void ScreenshotEditor::ShowBrushPopup(bool show) {
    if(!brush_popup_) { return; }
    if(show && visible_ && !modal_active_ && ContextTool()==ToolId::Mosaic) {
        brush_popup_open_=true;
        // Reserve all three islands as one group before showing the final one.
        // At the bottom edge, move the group upward instead of covering tools.
        if(!repositioning_) { PlaceContext(); return; }
        RECT anchor{},context{}; ::GetWindowRect(Control(2520),&anchor); ::GetWindowRect(context_,&context);
        const LONG width=ui::theme::Scaled(132,scale_),height=ui::theme::Scaled(kToolbarRowHeight+6,scale_),gap=ui::theme::Scaled(8,scale_);
        const auto work=platform::MonitorForRect(anchor).work_area;
        const LONG center=(anchor.left+anchor.right)/2,callout=ui::theme::Scaled(6,scale_);
        const LONG top=context.bottom+gap;
        RECT r{center-width/2,top,center+width-width/2,top+height}; r=platform::ClampRectToBounds(r,work);
        const LONG callout_x=std::clamp(center-r.left,callout*2,width-callout*2);
        HRGN shape=::CreateRoundRectRgn(0,callout,width+1,height+1,gap*2,gap*2);
        POINT triangle[]={{callout_x-callout,callout},{callout_x,0},{callout_x+callout,callout}};
        HRGN pointer=::CreatePolygonRgn(triangle,3,WINDING);
        if(shape && pointer) { ::CombineRgn(shape,shape,pointer,RGN_OR); }
        if(pointer) { ::DeleteObject(pointer); }
        if(shape && !::SetWindowRgn(brush_popup_,shape,FALSE)) { ::DeleteObject(shape); }
        for(int id=2300;id<2303;++id) {
            const auto item=std::find_if(toolbar_.begin(),toolbar_.end(),[id](const auto& value){ return value.identifier==id; });
            if(item!=toolbar_.end()) { ::SetWindowPos(Control(id),nullptr,static_cast<int>(item->rect.left),static_cast<int>(item->rect.top),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE); }
        }
        ::SetWindowPos(brush_popup_,HWND_TOPMOST,r.left,r.top,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        ::InvalidateRect(brush_popup_,nullptr,FALSE);
        brush_leave_started_=0; ::SetTimer(context_,0xEA62,30,nullptr);
    } else {
        brush_popup_open_=false;
        ::ShowWindow(brush_popup_,SW_HIDE); ::KillTimer(context_,0xEA62);
        brush_hover_started_=brush_leave_started_=0;
    }
}
void ScreenshotEditor::PaintContext(HDC dc,HWND window) const {
    RECT client{}; ::GetClientRect(window,&client); ui::PaintBuffer buffer(dc,client); dc=buffer.dc();
    ui::theme::DrawPanel(dc,client,scale_);
    if(window==context_) for(const auto& r:context_separators_) { ui::DrawSeparator(dc,r,scale_); }
}
void ScreenshotEditor::SetFrameInteracting(bool interacting) {
    if(frame_interacting_==interacting) { return; }
    frame_interacting_=interacting;
    if(interacting) { ShowBrushPopup(false); ::ShowWindow(hwnd_,SW_HIDE); if(context_) { ::ShowWindow(context_,SW_HIDE); } }
    else if(visible_) { PlaceContext(true); ::ShowWindow(hwnd_,SW_SHOWNOACTIVATE); }
}

LRESULT CALLBACK ScreenshotEditor::ToolbarButtonProc(HWND hwnd, UINT message, WPARAM wparam,
    LPARAM lparam, UINT_PTR identifier, DWORD_PTR reference) {
    auto* self = reinterpret_cast<ScreenshotEditor*>(reference);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, ToolbarButtonProc, identifier);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (ui::ConsumePopupInput(message, wparam)) { return 0; }
    if(message==WM_MOUSEMOVE && ::GetDlgCtrlID(hwnd)==2520) {
        if(!self->brush_hover_started_) { self->brush_hover_started_=::GetTickCount64(); }
        ::SetTimer(self->context_,0xEA62,30,nullptr);
    }
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) &&
        ::GetDlgCtrlID(hwnd) != 2400 + kCommandCancel && ::GetDlgCtrlID(hwnd) != 2400 + kCommandTheme && !self->FeatureUsed()) { return 0; }
    if (message == WM_KEYDOWN && (wparam == VK_ESCAPE || (::GetKeyState(VK_CONTROL) & 0x8000) != 0)) {
        return self->HandleMessage(self->hwnd_, message, wparam, lparam);
    }
    return ::DefSubclassProc(hwnd, message, wparam, lparam);
}

void ScreenshotEditor::SyncToolbarControls() const {
    if (!hwnd_) { return; }
    const auto style=ContextStyle();
    for (const auto& item : toolbar_) {
        HWND button = Control(item.identifier);
        if (!button) { continue; }
        if(((::GetWindowLongPtrW(button,GWL_STYLE)&WS_VISIBLE)!=0)!=item.shown) { ::ShowWindow(button, item.shown ? SW_SHOWNA : SW_HIDE); }
        if (item.shown) {
            const RECT rect{static_cast<LONG>(item.rect.left), static_cast<LONG>(item.rect.top),
                static_cast<LONG>(item.rect.right), static_cast<LONG>(item.rect.bottom)};
            RECT old{}; ::GetWindowRect(button, &old);
            ::MapWindowPoints(nullptr, ::GetParent(button), reinterpret_cast<POINT*>(&old), 2);
            if (!::EqualRect(&rect, &old)) { ::MoveWindow(button, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top, TRUE); }
        }
        if (reinterpret_cast<HFONT>(::SendMessageW(button, WM_GETFONT, 0, 0)) != toolbar_font_) {
            ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(toolbar_font_), TRUE);
        }
        bool selected = false;
        bool enabled = true;
        switch (item.kind) {
            case ToolbarItem::Kind::Tool: selected = item.tool == ContextTool(); break;
            case ToolbarItem::Kind::Color: selected = item.color.r == style.color.r && item.color.g == style.color.g && item.color.b == style.color.b; break;
            case ToolbarItem::Kind::Thickness: selected = item.thickness == style.thickness; break;
            case ToolbarItem::Kind::BrushSize: selected = item.brush_size == style.brush_size; break;
            case ToolbarItem::Kind::Option:
                selected = (item.identifier>=2500 && item.identifier<=2502 && static_cast<int>(style.text_mode)==item.identifier-2500) ||
                    (item.identifier>=2510 && item.identifier<=2511 && static_cast<int>(style.effect)==item.identifier-2510) ||
                    (item.identifier>=2520 && item.identifier<=2521 && static_cast<int>(style.method)==item.identifier-2520) ||
                    (item.identifier>=2530 && item.identifier<=2532 && static_cast<int>(style.shape)==item.identifier-2530);
                break;
            case ToolbarItem::Kind::Command:
                if (item.command == kCommandUndo || item.command == kCommandClear) { enabled = document_.CanUndo(); }
                if (item.command == kCommandRedo) { enabled = document_.CanRedo(); }
                break;
        }
        if ((::IsWindowEnabled(button) != FALSE) != enabled) { ::EnableWindow(button, enabled); }
        const WPARAM checked = selected ? BST_CHECKED : BST_UNCHECKED;
        if (item.kind != ToolbarItem::Kind::Command && ::SendMessageW(button, BM_GETCHECK, 0, 0) != static_cast<LRESULT>(checked)) {
            ::SendMessageW(button, BM_SETCHECK, checked, 0);
        }
    }
}

void ScreenshotEditor::ReflowToolbar() {
    if (!hwnd_) { return; }
    BuildToolbar(static_cast<float>(client_width_));
    const int height = toolbar_rect_.bottom;
    RECT current{}; ::GetWindowRect(hwnd_, &current);
    const auto monitor = platform::MonitorForRect(current);
    current.bottom = current.top + height;
    current = platform::ClampRectToBounds(current, monitor.work_area);
    ::SetWindowPos(hwnd_, nullptr, current.left, current.top, current.right-current.left, current.bottom-current.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    SyncToolbarControls();
    SyncContext();
    PlaceContext(!island_user_positioned_);
}

bool ScreenshotEditor::ProcessDialogMessage(MSG& message) const {
    if(!visible_) { return false; }
    for(HWND parent:{text_host_,brush_popup_,context_,hwnd_}) {
        if(parent && (message.hwnd==parent || ::IsChild(parent,message.hwnd))) {
            if(parent==text_host_) { return false; } // EDIT owns multiline Return, arrows and clipboard keys.
            if(message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE) {
                if(::SendMessageW(Control(2600),CB_GETDROPPEDSTATE,0,0)) { ::SendMessageW(Control(2600),CB_SHOWDROPDOWN,FALSE,0); }
                else { ::SendMessageW(hwnd_,WM_KEYDOWN,VK_ESCAPE,message.lParam); }
                return true;
            }
            return ui::ConsumePopupInput(message.message,message.wParam) || ui::ActivateFocusedButton(message,parent) || ::IsDialogMessageW(parent,&message)!=FALSE;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ScreenshotEditor::BuildToolbar(float available_width) {
    toolbar_.clear(); main_separators_.clear();
    const float s=scale_,pad=kEdgePadding*s,gap=kItemGap*s,row=kToolbarRowHeight*s;
    const float limit=std::max(available_width-pad,pad+kToolButton*s);
    float x=pad+kIslandGrip*s,y=0;
    const auto place=[&](float width) {
        if(x>pad && x+width>limit) { x=pad; y+=row; }
        const D2D1_RECT_F r{x,y+(row-kToolButton*s)/2,x+width,y+(row+kToolButton*s)/2}; x+=width+gap; return r;
    };
    struct Entry { ToolId tool; const wchar_t* name; };
    constexpr Entry tools[]={{ToolId::Pointer,L"Select / move"},{ToolId::Rectangle,L"Rectangle"},{ToolId::Ellipse,L"Ellipse"},
        {ToolId::Line,L"Line"},{ToolId::Arrow,L"Arrow"},{ToolId::Pen,L"Pen"},{ToolId::Text,L"Text"},{ToolId::Tag,L"Tag"},
        {ToolId::Mosaic,L"Blur / Pixelate"},{ToolId::Spotlight,L"Spotlight"},{ToolId::Watermark,L"Watermark"}};
    for(const auto& entry:tools) {
        if(entry.tool==ToolId::Mosaic) {
            const auto split=place(8*s); main_separators_.push_back({static_cast<LONG>((split.left+split.right)/2),static_cast<LONG>(split.top+8*s),static_cast<LONG>((split.left+split.right)/2+1),static_cast<LONG>(split.bottom-8*s)});
        }
        ToolbarItem item; item.tool=entry.tool; item.identifier=2000+static_cast<int>(entry.tool);
        item.label=item.tooltip=entry.name; item.rect=place(kToolButton*s); toolbar_.push_back(item);
    }
    { const auto split=place(kGroupGap*s); main_separators_.push_back({static_cast<LONG>((split.left+split.right)/2),static_cast<LONG>(split.top+8*s),static_cast<LONG>((split.left+split.right)/2+1),static_cast<LONG>(split.bottom-8*s)}); }
    struct Command { int id; const wchar_t* name; };
    constexpr Command commands[]={{kCommandUndo,L"Undo"},{kCommandRedo,L"Redo"},{kCommandClear,L"Clear annotations"},
        {kCommandTheme,L"Switch theme"},{kCommandSave,L"Save"},{kCommandCancel,L"Discard"},{kCommandCopy,L"Copy to clipboard (Ctrl+C)"}};
    for(const auto& entry:commands) {
        ToolbarItem item; item.kind=ToolbarItem::Kind::Command; item.command=entry.id; item.identifier=2400+entry.id;
        item.label=item.tooltip=entry.name; item.rect=place(kCommandButton*s); toolbar_.push_back(item);
    }
    toolbar_rect_={0,0,static_cast<LONG>(available_width),static_cast<LONG>(std::ceil(y+row))};
    x=pad; y=0; context_fields_.clear(); context_separators_.clear();
    const auto tool=ContextTool();
    const bool text=tool==ToolId::Text,tag=tool==ToolId::Tag,effect=tool==ToolId::Mosaic,spot=tool==ToolId::Spotlight,watermark=tool==ToolId::Watermark;
    float widest=0;
    const auto field=[&](int id,float width) {
        auto r=place(width*s); widest=std::max(widest,r.right+pad);
        context_fields_.push_back({id,{static_cast<LONG>(r.left),static_cast<LONG>(r.top),static_cast<LONG>(r.right),static_cast<LONG>(r.bottom)}});
    };
    const auto separator=[&] {
        const auto r=place(8*s);
        context_separators_.push_back({static_cast<LONG>((r.left+r.right)/2),static_cast<LONG>(r.top+8*s),static_cast<LONG>((r.left+r.right)/2+1),static_cast<LONG>(r.bottom-8*s)});
    };
    const auto option=[&](int id,const wchar_t* name,bool shown,float width=kToolButton) {
        ToolbarItem item; item.kind=ToolbarItem::Kind::Option; item.identifier=id; item.label=item.tooltip=name; item.shown=shown;
        if(shown) { item.rect=place(width*s); widest=std::max(widest,item.rect.right+pad); } toolbar_.push_back(item);
    };
    option(2500,L"Plain text",text); option(2501,L"Filled text",text); option(2502,L"Outlined text",text);
    option(2511,L"Pixelate",effect); option(2510,L"Smooth blur",effect);
    if(effect) { separator(); }
    option(2520,L"Brush and brush size",effect,48); option(2521,L"Rectangular region",effect);
    if(effect) { separator(); field(2611,125); field(2601,155); }
    option(2530,L"Rectangle spotlight",spot); option(2531,L"Ellipse spotlight",spot); option(2532,L"Rounded spotlight",spot);
    if(spot) { separator(); field(2612,110); field(2602,155); }
    if(text || tag) { field(2600,96); separator(); }
    if(watermark) { field(2604,140); separator(); }
    const bool stroke=tool!=ToolId::Pointer && !text && !tag && !effect && !spot && !watermark;
    for(size_t i=0;i<kThicknessOptions.size();++i) {
        ToolbarItem item; item.kind=ToolbarItem::Kind::Thickness; item.thickness=kThicknessOptions[i]; item.identifier=2200+static_cast<int>(i);
        item.tooltip=L"Stroke width"; item.shown=stroke;
        if(item.shown) { item.rect=place(kToolButton*s); widest=std::max(widest,item.rect.right+pad); } toolbar_.push_back(item);
    }
    if(stroke) { separator(); }
    for(size_t i=0;i<kPaletteColors.size();++i) {
        ToolbarItem item; item.kind=ToolbarItem::Kind::Color; item.color=kPaletteColors[i]; item.identifier=2100+static_cast<int>(i);
        item.tooltip=L"Annotation color"; item.shown=tool!=ToolId::Pointer && !effect && !spot;
        if(item.shown) { item.rect=place(kSwatch*s); widest=std::max(widest,item.rect.right+pad); } toolbar_.push_back(item);
    }
    if(watermark) { separator(); field(2612,85); field(2602,100); separator(); field(2613,65); field(2603,100); }
    for(size_t i=0;i<kBrushSizeOptions.size();++i) {
        ToolbarItem item; item.kind=ToolbarItem::Kind::BrushSize; item.brush_size=kBrushSizeOptions[i]; item.identifier=2300+static_cast<int>(i);
        item.tooltip=L"Brush size"; item.shown=true;
        const float left=pad+static_cast<float>(i)*(kToolButton+kItemGap)*s;
        const float callout=6*s;
        item.rect={left,callout+(row-kToolButton*s)/2,left+kToolButton*s,callout+(row+kToolButton*s)/2}; toolbar_.push_back(item);
    }
    context_visible_=tool!=ToolId::Pointer;
    context_width_=static_cast<int>(std::ceil(std::min(available_width,std::max(widest,200*s))));
    context_height_=context_visible_ ? static_cast<int>(std::ceil(y+row)):0;

}

void ScreenshotEditor::LayoutClient(int client_width, int client_height) {
    client_width_ = std::max(1, client_width);
    client_height_ = std::max(1, client_height);
    BuildToolbar(static_cast<float>(client_width_));
    toolbar_rect_.right = client_width_;
    zoom_ = 1.0f;
    image_rect_ = ToD2DRect(source_bounds_);
    SyncToolbarControls();
}

RECT ScreenshotEditor::InitialWindowRect(const RECT& screen_region) const {
    const auto monitor = platform::MonitorForRect(screen_region);
    RECT work = monitor.work_area;
    work.top = std::max(work.top, monitor.bounds.top + ui::theme::Scaled(
        ui::theme::kPaletteButtonHeight + 2 * ui::theme::kPalettePadding, monitor.scale()));
    const LONG width = std::min(platform::RectWidth(work), static_cast<LONG>(kIslandWidth * scale_));
    const LONG height = toolbar_rect_.bottom;
    const LONG margin = static_cast<LONG>(kCanvasGap * scale_);
    const LONG left = screen_region.left + (platform::RectWidth(screen_region) - width) / 2;
    const LONG top = std::min(work.bottom - height - margin, screen_region.bottom + margin);
    return platform::ClampRectToBounds(RECT{left, top, left + width, top + height}, work);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ScreenshotEditor::ResetSession() {
    brush_popup_open_=false;
    // Undo/redo is strictly per session: nothing from a previous capture may be
    // reachable once that capture is gone.
    document_.Reset();
    selected_.reset(); transforming_=false; frame_interacting_=false; island_user_positioned_=false;
    if(text_host_) { ::DestroyWindow(text_host_); text_host_=text_edit_=nullptr; }
    pending_ = Annotation{};
    has_pending_ = false;
    dragging_ = false;
    editing_text_ = false;
    hovered_index_ = -1;
    redaction_revision_ = UINT64_MAX;
    redacted_image_ = platform::BgraImage{};
    redaction_cache_.reset();
    image_bitmap_.Reset();
}

void ScreenshotEditor::CreateTextFormats() {
    HFONT font = ui::theme::CreateUiFont(13.0f, scale_, false);
    if (font) {
        if (toolbar_font_) { ::DeleteObject(toolbar_font_); }
        toolbar_font_=font; SyncToolbarControls();
    }

}

bool ScreenshotEditor::Open(const RECT& screen_region, app::AppSettings* settings, HWND owner) {
    if (platform::IsEmptyRect(screen_region)) {
        return false;
    }
    Destroy();

    source_bounds_ = screen_region;
    region_ = screen_region;
    settings_ = settings;
    scale_ = platform::MonitorForRect(screen_region).scale();
    ResetSession();
    closed_reported_ = false;
    tool_ = ToolId::Pointer;
    style_ = AnnotationStyle{};
    style_.thickness = kThicknessOptions[1];
    style_.brush_size = kBrushSizeOptions[1];
    style_.font_size = 16.0f;
    style_.image_scale = scale_;

    static bool class_registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &ScreenshotEditor::WindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = platform::LoadAppIcon(::GetSystemMetrics(SM_CXICON));
        wc.hIconSm = platform::LoadAppIcon(::GetSystemMetrics(SM_CXSMICON));
        wc.lpszClassName = kEditorClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    if (!class_registered) {
        return false;
    }

    BuildToolbar(std::min(kIslandWidth * scale_,
        static_cast<float>(platform::RectWidth(platform::MonitorForRect(screen_region).work_area))));
    const RECT window = InitialWindowRect(screen_region);
    hwnd_ = ::CreateWindowExW(kEditorExStyle, kEditorClassName, kEditorTitle, kEditorStyle,
                              static_cast<int>(window.left), static_cast<int>(window.top),
                              static_cast<int>(platform::RectWidth(window)),
                              static_cast<int>(platform::RectHeight(window)), owner, nullptr,
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        platform::LogMessage(L"CreateWindowEx(editor) failed (error %lu)", ::GetLastError());
        return false;
    }

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    LayoutClient(static_cast<int>(platform::RectWidth(client)), static_cast<int>(platform::RectHeight(client)));

    context_=::CreateWindowExW(kEditorExStyle|WS_EX_LAYERED,kEditorClassName,L"Annotation options",kEditorStyle,window.left,window.bottom+8,
        static_cast<int>(window.right-window.left),48,hwnd_,nullptr,::GetModuleHandleW(nullptr),this);
    brush_popup_=::CreateWindowExW(kEditorExStyle,kEditorClassName,L"Brush sizes",kEditorStyle,window.left,window.bottom,
        static_cast<int>(132*scale_),static_cast<int>((kToolbarRowHeight+6)*scale_),hwnd_,nullptr,::GetModuleHandleW(nullptr),this);
    if(brush_popup_) { ::SetWindowDisplayAffinity(brush_popup_,0x11); }
    if(!context_ || !brush_popup_ || !CreateToolbarControls()) { Destroy(); return false; }
    CreateContextControls();
    if(!Control(2600) || !Control(2601) || !Control(2604)) { Destroy(); return false; }
    ::SetWindowDisplayAffinity(context_,0x11);
    ::SetLayeredWindowAttributes(context_,0,255,LWA_ALPHA);
    static_cast<void>(::SetWindowDisplayAffinity(hwnd_, 0x11));
    CreateTextFormats();
    RebuildRedactedBase();

    Show();
    ReflowToolbar();
    return true;
}

bool ScreenshotEditor::SetImage(platform::BgraImage image) {
    if (has_image() || image.empty() || image.width != platform::RectWidth(region_) ||
        image.height != platform::RectHeight(region_)) { return false; }
    source_image_ = std::move(image);
    source_bounds_ = region_;
    image_rect_ = ToD2DRect(source_bounds_);
    redaction_revision_ = UINT64_MAX;
    Invalidate();
    return true;
}

void ScreenshotEditor::Show() {
    visible_ = true;
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    PlaceContext();
    Invalidate();
}

void ScreenshotEditor::Hide() {
    visible_ = false;
    ShowBrushPopup(false);
    ::ShowWindow(hwnd_, SW_HIDE);
    if(context_) { ::ShowWindow(context_,SW_HIDE); }
    if(text_host_) { ::ShowWindow(text_host_,SW_HIDE); }
}

void ScreenshotEditor::SetRegion(const RECT& region, bool interacting) {
    if (has_image() || platform::IsEmptyRect(region)) { return; }
    if(!::EqualRect(&region_,&region)) { island_user_positioned_=false; }
    region_ = region;
    SetFrameInteracting(interacting);
    if(!interacting) { PlaceContext(true); }

}

bool ScreenshotEditor::EnsureImage() {
    if (has_image()) { return true; }
    if (callbacks_.on_prepare_image && callbacks_.on_prepare_image() && has_image()) { return true; }
    ui::ShowCaptureStatusNotice(region_,L"Could not capture the image - try again");
    Invalidate();
    return false;
}

bool ScreenshotEditor::FeatureUsed() {
    if (!EnsureImage()) { return false; }
    if (callbacks_.on_feature_used) { callbacks_.on_feature_used(); }
    return true;
}

void ScreenshotEditor::NotifyMutation(uint64_t previous_revision) {
    if (document_.revision() != previous_revision && callbacks_.on_document_committed) {
        callbacks_.on_document_committed();
    }
}

void ScreenshotEditor::ImagePointer(UINT message, POINT point) {
    if (!visible_) { return; }
    switch (message) {
        case WM_LBUTTONDOWN: if (FeatureUsed()) { BeginStroke(point); } break;
        case WM_LBUTTONDBLCLK: {
            if(!FeatureUsed()) { break; }
            const auto hit=document_.HitTest(ToImagePoint(point));
            if(hit && (document_.annotations()[*hit].tool==ToolId::Text || document_.annotations()[*hit].tool==ToolId::Tag)) {
                CancelStroke(); StartTextEditing(hit,point);
                if(text_edit_) {
                    POINT local=point; ::ScreenToClient(text_edit_,&local);
                    ::SendMessageW(text_edit_,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(local.x,local.y));
                }
            } else if(editing_text_) {
                // A blank double-click is the continuation of the first press,
                // not another creation request. It can only finish an edit.
                CommitPendingText(); Select({});
            } else if(tool_!=ToolId::Text && tool_!=ToolId::Tag) { BeginStroke(point); }
            break;
        }
        case WM_MOUSEMOVE: UpdateStroke(point); break;
        case WM_LBUTTONUP: EndStroke(point); break;
        case WM_CAPTURECHANGED: CancelStroke(); break;
        default: break;
    }
}

void ScreenshotEditor::Keyboard(UINT message, WPARAM key, LPARAM flags) {
    if (visible_) { HandleMessage(hwnd_, message, key, flags); }
}

bool ScreenshotEditor::CompleteCopy() {
    if (!CopyToClipboard()) { return false; }
    ui::ShowCaptureCopiedNotice(region_);
    Close(true);
    return true;
}

void ScreenshotEditor::Close(bool exported) {
    Hide();
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
    // The editing session is over: drop the whole command history before anyone
    // can reopen the window, so undo/redo never leaks across captures.
    ResetSession();
    if (!closed_reported_) {
        closed_reported_ = true;
        if (callbacks_.on_closed) {
            callbacks_.on_closed(exported);
        }
    }
}

void ScreenshotEditor::Destroy() {
    if(brush_popup_) { ::DestroyWindow(brush_popup_); brush_popup_=nullptr; }
    if(text_host_) { ::DestroyWindow(text_host_); text_host_=text_edit_=nullptr; }
    if(context_) { ::DestroyWindow(context_); context_=nullptr; }
    if(text_font_) { ::DeleteObject(text_font_); text_font_=nullptr; }
    ResetSession();

    if (tooltip_) { ::DestroyWindow(tooltip_); tooltip_ = nullptr; }
    image_target_.Reset();
    image_bitmap_.Reset();
    image_brush_.Reset();
    if (hwnd_ != nullptr) {
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (toolbar_font_) { ::DeleteObject(toolbar_font_); toolbar_font_ = nullptr; }
    source_image_ = platform::BgraImage{};
    visible_ = false;
}

void ScreenshotEditor::Invalidate() const {
    SyncToolbarControls();
    if (has_image() && callbacks_.on_image_invalidated) { callbacks_.on_image_invalidated(region_); }
}

void ScreenshotEditor::InvalidateImage(const RECT& source_damage) const {
    if (!has_image() || !callbacks_.on_image_invalidated) { return; }
    RECT desktop_damage = source_damage;
    ::OffsetRect(&desktop_damage, source_bounds_.left, source_bounds_.top);
    callbacks_.on_image_invalidated(platform::IntersectRects(desktop_damage, region_));
}

RECT ScreenshotEditor::PendingDamage() const {
    if (pending_.tool == ToolId::Text) { return RECT{0, 0, source_image_.width, source_image_.height}; }
    const auto bounds = pending_.bounds();
    const float pad = std::max(pending_.style.brush_size, pending_.style.thickness * 6.0f) + 3.0f;
    return RECT{static_cast<LONG>(std::floor(bounds.left - pad)), static_cast<LONG>(std::floor(bounds.top - pad)),
        static_cast<LONG>(std::ceil(bounds.right + pad)), static_cast<LONG>(std::ceil(bounds.bottom + pad))};
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

LRESULT CALLBACK ScreenshotEditor::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<ScreenshotEditor*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT ScreenshotEditor::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ui::ConsumePopupInput(message, wparam)) { return 0; }
    if(message==WM_TIMER && wparam==0xEA63) { SyncWatermarkClear(); return 0; }
    if(message==WM_TIMER && wparam==0xEA62) {
        POINT point{}; ::GetCursorPos(&point);
        const auto over=[&](HWND window) { RECT r{}; ::GetWindowRect(window,&r); return ::IsWindowVisible(window) && ::PtInRect(&r,point); };
        const auto now=::GetTickCount64();
        const bool hand=over(Control(2520));
        if(hand || over(brush_popup_)) {
            brush_leave_started_=0;
            if(hand && brush_hover_started_ && now-brush_hover_started_>=350 && !::IsWindowVisible(brush_popup_)) { ShowBrushPopup(true); }
        } else if(!brush_leave_started_) { brush_leave_started_=now; }
        else if(now-brush_leave_started_>=250) { ShowBrushPopup(false); }
        return 0;
    }
    if(message==WM_TIMER && wparam==0xEA61 && hwnd==context_) {
        const float progress=std::clamp(static_cast<float>(::GetTickCount64()-context_fade_started_)/160.0f,0.0f,1.0f);
        ::SetLayeredWindowAttributes(context_,0,static_cast<BYTE>(255*progress),LWA_ALPHA);
        if(progress>=1 || !ui::theme::AnimationsEnabled()) { ::KillTimer(context_,0xEA61); ::SetLayeredWindowAttributes(context_,0,255,LWA_ALPHA); }
        return 0;
    }
    const bool auxiliary=hwnd_ && hwnd!=hwnd_;
    if(auxiliary) {
        if(message==WM_SIZE || message==WM_DPICHANGED || message==WM_GETMINMAXINFO) { return 0; }
        if(message==WM_NCHITTEST) { return HTCLIENT; }
        if(message==WM_PAINT || message==WM_PRINTCLIENT) {
            PAINTSTRUCT ps{}; HDC dc=message==WM_PAINT ? ::BeginPaint(hwnd,&ps):reinterpret_cast<HDC>(wparam);
            if(hwnd==text_host_) { RECT bounds{}; ::GetWindowRect(hwnd,&bounds); RenderImage(dc,bounds); }
            else { PaintContext(dc,hwnd); }
            if(message==WM_PAINT) { ::EndPaint(hwnd,&ps); } return 0;
        }
        if(message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_MOUSEMOVE || message==WM_CAPTURECHANGED) { return 0; }
    }
    if(message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX || message==WM_CTLCOLORSTATIC) {
        if(text_edit_ && reinterpret_cast<HWND>(lparam)==text_edit_) {
            // EDIT can paint synchronously during input while WS_VISIBLE is
            // retained for its native caret. Its GDI glyphs/backing are never
            // the annotation presentation: only PaintTextEditor owns those.
            // This DC belongs to the native draw and is reset on release; the
            // custom paint pass obtains its own unrestricted client DC.
            ::IntersectClipRect(reinterpret_cast<HDC>(wparam),0,0,0,0);
            return reinterpret_cast<LRESULT>(::GetStockObject(NULL_BRUSH));
        }
        return ui::ControlBackground(wparam,ui::theme::Current().text);
    }
    if(message==WM_HSCROLL && !syncing_context_) {
        const int id=::GetDlgCtrlID(reinterpret_cast<HWND>(lparam)); auto value=ContextStyle();
        const float position=static_cast<float>(::SendMessageW(reinterpret_cast<HWND>(lparam),TBM_GETPOS,0,0));
        if(id==2601) { value.strength=position; }
        if(id==2602) { value.opacity=position; }
        if(id==2603) { value.font_size=position; }
        ChangeStyle(value);
        if(LOWORD(wparam)==TB_ENDTRACK) { document_.CommitTransaction(); }
        return 0;
    }

    switch (message) {
        case ui::theme::kChangedMessage:
            ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0; // No document/history/fade/placement work.
        case WM_MEASUREITEM: {
            auto* measure=reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if(measure && measure->CtlType==ODT_COMBOBOX) { measure->itemHeight=ui::theme::Scaled(32,scale_); return TRUE; } break;
        }
        case WM_DRAWITEM: {
            const auto* draw=reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if(draw && ui::DrawComboItem(*draw)) { return TRUE; } break;
        }
        case WM_COMMAND: {
            const int id=LOWORD(wparam),notification=HIWORD(wparam);
            if(id==IDCANCEL) { Close(false); return 0; }
            if(id==2600 && notification==CBN_DROPDOWN) {
                COMBOBOXINFO info{sizeof(info)}; ::GetComboBoxInfo(Control(2600),&info);
                ::SetWindowLongPtrW(info.hwndList,GWL_STYLE,::GetWindowLongPtrW(info.hwndList,GWL_STYLE)&~WS_BORDER);
                ::SetWindowLongPtrW(info.hwndList,GWL_EXSTYLE,::GetWindowLongPtrW(info.hwndList,GWL_EXSTYLE)&~WS_EX_CLIENTEDGE);
                ui::RoundWindow(info.hwndList); ::SetWindowDisplayAffinity(info.hwndList,0x11); return 0;
            }
            if(id==2700 && notification==EN_CHANGE) { UpdateTextFromControl(); return 0; }
            if(syncing_context_) { return 0; }
            if(id==2600 && (notification==CBN_SELCHANGE || notification==CBN_KILLFOCUS)) {
                wchar_t text[32]{};
                if(notification==CBN_SELCHANGE) { ::SendMessageW(Control(2600),CB_GETLBTEXT,::SendMessageW(Control(2600),CB_GETCURSEL,0,0),reinterpret_cast<LPARAM>(text)); }
                else { ::GetWindowTextW(Control(2600),text,32); }
                wchar_t* end=nullptr; const double size=std::wcstod(text,&end);
                while(end && std::iswspace(*end)) { ++end; }
                if(end && ::lstrcmpiW(end,L"pt")==0) { end+=2; }
                if(end && *end==0 && std::isfinite(size) && size>=6 && size<=288) { auto value=ContextStyle(); value.font_size=static_cast<float>(size); ChangeStyle(value); }
                else { ui::ShowCaptureStatusNotice(region_,L"Font size must be 6–288 pt"); SyncContext(); Invalidate(); }
                if(notification==CBN_KILLFOCUS) { document_.CommitTransaction(); }
                return 0;
            }
            if(id==2605 && notification==BN_CLICKED) {
                document_.BeginTransaction();
                ::SetWindowTextW(Control(2604),L"");
                document_.CommitTransaction();
                ::SetFocus(Control(2604)); SyncWatermarkClear(); return 0;
            }
            if(id==2604 && notification==EN_CHANGE && ContextTool()==ToolId::Watermark) {
                if(!FeatureUsed()) { return 0; }
                const int length=::GetWindowTextLengthW(Control(2604));
                std::wstring text(static_cast<size_t>(length)+1,L'\0'); ::GetWindowTextW(Control(2604),text.data(),length+1); text.resize(std::min(length,16));
                if(!text.empty() && text.back()>=0xd800 && text.back()<=0xdbff) { text.pop_back(); }
                if(text.size()!=static_cast<size_t>(length)) {
                    syncing_context_=true; ::SetWindowTextW(Control(2604),text.c_str()); ::SendMessageW(Control(2604),EM_SETSEL,text.size(),text.size()); syncing_context_=false;
                }
                Annotation a;
                if(selected_) { a=document_.annotations()[*selected_]; }
                else { a.tool=ToolId::Watermark; a.style=style_; a.style.opacity=30; a.points={{0,0},{static_cast<float>(source_image_.width),static_cast<float>(source_image_.height)}}; }
                a.text=std::move(text);
                if(selected_) { document_.Replace(*selected_,std::move(a)); }
                else { document_.Apply(std::move(a)); selected_=document_.annotations().size()-1; }
                SyncContext(); Invalidate(); return 0;
            }
            if(notification==BN_CLICKED) {
                const auto found=std::find_if(toolbar_.begin(),toolbar_.end(),[id](const auto& item){ return item.identifier==id; });
                if(found!=toolbar_.end()) { const auto item=*found; OnToolbarActivated(item); }
            }
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintToolbar(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ScreenToClient(hwnd, &point);
            if(::ChildWindowFromPointEx(hwnd,point,CWP_SKIPINVISIBLE)==hwnd) { return HTCAPTION; }
            return HTCLIENT;
        }
        case WM_NCLBUTTONDBLCLK:
            if (wparam == HTCAPTION) { FeatureUsed(); }
            return 0; // Grip double-click never maximizes or completes.

        case WM_NCLBUTTONDOWN:
            // The grip is part of the floating toolbar. Fix the image before
            // Windows enters its native move loop; moving the island remains allowed.
            if (wparam == HTCAPTION && !FeatureUsed()) { return 0; }
            break;

        case WM_ENTERSIZEMOVE:
            if(!auxiliary) { island_user_positioned_=true; }
            return 0;
        case WM_WINDOWPOSCHANGED:
            if(!auxiliary) { PlaceContext(); }
            break;
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            PaintToolbar(dc);
            ::EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE: {
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            const int width = static_cast<int>(LOWORD(lparam));
            const int height = static_cast<int>(HIWORD(lparam));
            LayoutClient(width, height);
            Invalidate();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            RECT minimum{0, 0, static_cast<LONG>(kMinClientWidth * scale_),
                         static_cast<LONG>(kMinClientHeight * scale_)};
            AdjustForNonClient(minimum, static_cast<UINT>(std::lround(scale_ * 96.0f)));
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = platform::RectWidth(minimum);
            info->ptMinTrackSize.y = platform::RectHeight(minimum);
            return 0;
        }

        case WM_DPICHANGED: {
            scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            CreateTextFormats();
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) {
                const auto monitor = platform::MonitorForRect(*suggested);
                RECT available = monitor.work_area;
                available.top = std::max(available.top, monitor.bounds.top + ui::theme::Scaled(
                    ui::theme::kPaletteButtonHeight + 2 * ui::theme::kPalettePadding, scale_));
                const LONG width = std::min(platform::RectWidth(available), static_cast<LONG>(kIslandWidth * scale_));
                BuildToolbar(static_cast<float>(width));
                const LONG height = toolbar_rect_.bottom;
                const RECT placement = platform::ClampRectToBounds(
                    RECT{suggested->left, suggested->top, suggested->left + width, suggested->top + height}, available);
                ::SetWindowPos(hwnd, nullptr, placement.left, placement.top, platform::RectWidth(placement),
                               platform::RectHeight(placement), SWP_NOZORDER | SWP_NOACTIVATE);
            }
            Invalidate();
            return 0;
        }

        case WM_MOVING: {
            auto* proposed = reinterpret_cast<RECT*>(lparam);
            if (proposed) {
                const auto monitor = platform::MonitorForRect(*proposed);
                RECT available = monitor.work_area;
                available.top = std::max(available.top, monitor.bounds.top +
                    ui::theme::Scaled(ui::theme::kPaletteButtonHeight + 2 * ui::theme::kPalettePadding, monitor.scale()));
                *proposed = platform::ClampRectToBounds(*proposed, available);
            }
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const ToolbarItem* hovered = HitTestToolbar(pt);
            const int hovered_index = hovered != nullptr ? static_cast<int>(hovered - toolbar_.data()) : -1;
            if (hovered_index != hovered_index_) {
                hovered_index_ = hovered_index;
                Invalidate();
            }
            if ((dragging_ || transforming_) && ::GetCapture()==hwnd) {
                POINT desktop=pt; ::ClientToScreen(hwnd,&desktop); UpdateStroke(desktop);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            ::SetFocus(hwnd);
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (const ToolbarItem* item = HitTestToolbar(pt)) { OnToolbarActivated(*item); }
            else { FeatureUsed(); } // Padding, empty toolbar space and the info strip also fix the image.
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            FeatureUsed();
            return 0;

        case WM_LBUTTONUP: {
            // Finish the stroke *before* releasing capture: ReleaseCapture sends
            // WM_CAPTURECHANGED synchronously, which cancels an unfinished one.
            if (dragging_ || transforming_) {
                POINT desktop{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ::ClientToScreen(hwnd,&desktop); EndStroke(desktop);
            }
            if (::GetCapture() == hwnd) {
                ::ReleaseCapture();
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Losing capture (alt-tab, resize, ...) must not leave a half stroke.
            CancelStroke();
            return 0;

        case WM_CHAR:
            OnCharacter(static_cast<wchar_t>(wparam));
            return 0;

        case WM_KEYDOWN: {
            if(editing_text_ && text_edit_) { ::SendMessageW(text_edit_,message,wparam,lparam); return 0; }
            if(wparam==VK_ESCAPE && (transforming_ || dragging_ || document_.in_transaction())) { CancelStroke(); document_.CancelTransaction(); SyncContext(); Invalidate(); return 0; }
            if(wparam==VK_ESCAPE && selected_) { Select({}); return 0; }
            if(wparam==VK_DELETE && selected_) { document_.Remove(*selected_); Select({}); return 0; }
            if(selected_ && wparam>=VK_LEFT && wparam<=VK_DOWN) {
                auto a=document_.annotations()[*selected_]; const float step=(::GetKeyState(VK_SHIFT)&0x8000) ? 10.0f:1.0f;
                for(auto& p:a.points) { p.x+=wparam==VK_LEFT ? -step:wparam==VK_RIGHT ? step:0; p.y+=wparam==VK_UP ? -step:wparam==VK_DOWN ? step:0; }
                document_.Replace(*selected_,std::move(a)); Invalidate(); return 0;
            }
            if (wparam == VK_TAB) { ::SetFocus(::GetDlgItem(hwnd_, 2000)); return 0; }
            const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (control) {
                switch (wparam) {
                    case 'Z': {
                        selected_.reset();
                        if (!FeatureUsed()) { return 0; }
                        const auto revision = document_.revision();
                        if (shift) {
                            document_.Redo();
                        } else {
                            document_.Undo();
                        }
                        Invalidate();
                        NotifyMutation(revision);
                        return 0;
                    }
                    case 'Y': {
                        selected_.reset();
                        if (!FeatureUsed()) { return 0; }
                        const auto revision = document_.revision();
                        document_.Redo();
                        Invalidate();
                        NotifyMutation(revision);
                        return 0;
                    }
                    case 'C':
                        if (!FeatureUsed()) { return 0; }
                        CompleteCopy();
                        return 0;
                    case 'S':
                        if (!FeatureUsed()) { return 0; }
                        if (SaveToFile()) {
                            Close(true);
                        }
                        return 0;
                    default:
                        break;
                }
            }
            if (wparam == VK_ESCAPE) {
                if (editing_text_) {
                    editing_text_ = false;
                    has_pending_ = false;
                    Invalidate();
                    return 0;
                }
                Close(false);
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            Close(false);
            return 0;

        case WM_DESTROY:
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

D2D1_POINT_2F ScreenshotEditor::ToImagePoint(POINT client_pt) const {
    const float zoom = zoom_ > 0.0f ? zoom_ : 1.0f;
    return D2D1::Point2F((static_cast<float>(client_pt.x) - image_rect_.left) / zoom,
                         (static_cast<float>(client_pt.y) - image_rect_.top) / zoom);
}

const ScreenshotEditor::ToolbarItem* ScreenshotEditor::HitTestToolbar(POINT client_pt) const {
    if (::PtInRect(&toolbar_rect_, client_pt) == FALSE) {
        return nullptr;
    }
    const float x = static_cast<float>(client_pt.x - toolbar_rect_.left);
    const float y = static_cast<float>(client_pt.y - toolbar_rect_.top);
    for (const ToolbarItem& item : toolbar_) {
        if (item.shown && x >= item.rect.left && x <= item.rect.right && y >= item.rect.top && y <= item.rect.bottom) {
            return &item;
        }
    }
    return nullptr;
}

void ScreenshotEditor::OnToolbarActivated(const ToolbarItem& item) {
    if(item.kind==ToolbarItem::Kind::Command && item.command==kCommandCancel) { Close(false); return; }
    if(item.kind==ToolbarItem::Kind::Command && item.command==kCommandTheme) {
        const auto value=ui::theme::Preference()==app::AppearanceTheme::Light ? app::AppearanceTheme::Dark:app::AppearanceTheme::Light;
        if(callbacks_.on_appearance) { callbacks_.on_appearance(value); }
        else { ui::theme::ApplyPreference(value); ui::theme::NotifyWindows(); }
        return;
    }
    if(!FeatureUsed()) { return; }
    if(item.identifier==2520 && ContextStyle().method==EffectMethod::Brush) { ShowBrushPopup(!::IsWindowVisible(brush_popup_)); return; }
    if(item.kind==ToolbarItem::Kind::Command && item.command==kCommandCopy) { CompleteCopy(); return; }
    const auto revision=document_.revision();
    if(item.kind==ToolbarItem::Kind::Tool || item.kind==ToolbarItem::Kind::Command) {
        ShowBrushPopup(false); CommitPendingText(); document_.CommitTransaction(); CancelStroke();
    }
    if(!visible_) { return; }
    auto style=ContextStyle();
    switch(item.kind) {
        case ToolbarItem::Kind::Tool:
            tool_=item.tool; selected_.reset();
            if(tool_==ToolId::Watermark) {
                const auto& items=document_.annotations();
                for(size_t i=0;i<items.size();++i) if(items[i].tool==ToolId::Watermark) { selected_=i; break; }
            }
            break;
        case ToolbarItem::Kind::Color: style.color=item.color; ChangeStyle(style); break;
        case ToolbarItem::Kind::Thickness: style.thickness=item.thickness; ChangeStyle(style); break;
        case ToolbarItem::Kind::BrushSize: style.brush_size=item.brush_size; ChangeStyle(style); ShowBrushPopup(false); break;
        case ToolbarItem::Kind::Option:
            if(item.identifier>=2500 && item.identifier<=2502) { style.text_mode=static_cast<TextMode>(item.identifier-2500); }
            if(item.identifier>=2510 && item.identifier<=2511) { style.effect=static_cast<EffectMode>(item.identifier-2510); }
            if(item.identifier>=2520 && item.identifier<=2521) {
                // Application method is a new-tool default. Existing region or
                // brush geometry retains its transform contract.
                if(selected_) { selected_.reset(); style=style_; }
                tool_=ToolId::Mosaic;
                style.method=static_cast<EffectMethod>(item.identifier-2520);
            }
            if(item.identifier>=2530 && item.identifier<=2532) { style.shape=static_cast<SpotlightShape>(item.identifier-2530); }
            ChangeStyle(style); break;
        case ToolbarItem::Kind::Command:
            selected_.reset();
            if(item.command==kCommandUndo) { document_.Undo(); }
            if(item.command==kCommandRedo) { document_.Redo(); }
            if(item.command==kCommandClear) { document_.Clear(); }
            if(item.command==kCommandSave && SaveToFile()) { Close(true); return; }
            break;
    }
    ReflowToolbar(); Invalidate(); NotifyMutation(revision);
    if(item.identifier==2520) { ShowBrushPopup(true); }
}

bool ScreenshotEditor::WantsPointer(POINT point) const {
    if(editing_text_ || transforming_ || dragging_ || selected_) { return true; }
    const auto p=ToImagePoint(point);
    return tool_!=ToolId::Pointer || document_.HitTest(p).has_value();
}
HCURSOR ScreenshotEditor::ImageCursor(POINT point) const {
    const auto p=ToImagePoint(point);
    if(selected_ && *selected_<document_.annotations().size()) {
        const auto handles=document_.annotations()[*selected_].Handles();
        for(size_t i=0;i<handles.size();++i) if(std::hypot(p.x-handles[i].x,p.y-handles[i].y)<=9*scale_) {
            return ::LoadCursorW(nullptr,handles.size()==2 ? IDC_CROSS : i%2==0 ? IDC_SIZENWSE:IDC_SIZENESW);
        }
    }
    const bool movable=document_.HitTest(p).has_value();
    return ::LoadCursorW(nullptr,movable ? IDC_SIZEALL : tool_==ToolId::Pointer ? IDC_ARROW : tool_==ToolId::Text || tool_==ToolId::Tag ? IDC_IBEAM:IDC_CROSS);
}
void ScreenshotEditor::BeginStroke(POINT point) {
    ShowBrushPopup(false);
    if(editing_text_) {
        // Repeated clicks on the active object keep its editing session alive.
        // Only an actual outside click commits; that click never creates text.
        if(selected_ && document_.annotations()[*selected_].HitTest(ToImagePoint(point))) {
            if(text_edit_) {
                POINT local=point; ::ScreenToClient(text_edit_,&local);
                ::SendMessageW(text_edit_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(local.x,local.y));
                ::SendMessageW(text_edit_,WM_LBUTTONUP,0,MAKELPARAM(local.x,local.y));
            }
            return;
        }
        CommitPendingText(); Select({});
        // Commit can remove an empty annotation and shift later indices.
        const auto hit=document_.HitTest(ToImagePoint(point));
        if(hit && (document_.annotations()[*hit].tool==ToolId::Text || document_.annotations()[*hit].tool==ToolId::Tag)) {
            StartTextEditing(hit,point);
        }
        return;
    }
    if(!visible_) { return; }
    const auto p=ToImagePoint(point);
    transform_handle_=-1;
    if(selected_ && *selected_<document_.annotations().size()) {
        const auto handles=document_.annotations()[*selected_].Handles();
        for(size_t i=0;i<handles.size();++i) if(std::hypot(p.x-handles[i].x,p.y-handles[i].y)<=9*scale_) { transform_handle_=static_cast<int>(i); break; }
    }
    // Hover-hit objects are selectable and movable with every creation tool.
    // Handles appear only after an explicit selection click.
    std::optional<size_t> hit;
    if(transform_handle_>=0) { hit=selected_; }
    else { hit=document_.HitTest(p); }
    if(hit) {
        Select(hit); original_=document_.annotations()[*hit]; drag_origin_=p;
        transforming_=true; document_.BeginTransaction(); return;
    }
    if(tool_==ToolId::Watermark) { return; }
    const bool leave_text_selection=(tool_==ToolId::Text || tool_==ToolId::Tag) && selected_ &&
        (document_.annotations()[*selected_].tool==ToolId::Text || document_.annotations()[*selected_].tool==ToolId::Tag);
    Select({});
    // A selected text object is still the current interaction target after a
    // select/drag. Its first outside press only leaves that state; a later
    // blank press with no active selection may create the next text object.
    if(leave_text_selection) { return; }
    if(tool_==ToolId::Pointer) { return; }
    if(tool_==ToolId::Text || tool_==ToolId::Tag) { StartTextEditing({},point); return; }
    pending_={}; pending_.tool=tool_; pending_.style=style_; pending_.points={p,p};
    if(tool_==ToolId::Spotlight) for(const auto& a:document_.annotations()) if(a.tool==ToolId::Spotlight) { pending_.style.opacity=a.style.opacity; break; }
    has_pending_=dragging_=true; InvalidateImage(PendingDamage());
}
void ScreenshotEditor::UpdateStroke(POINT point) {
    const auto p=ToImagePoint(point);
    if(transforming_ && selected_) {
        auto updated=original_;
        if(transform_handle_<0) {
            const float dx=p.x-drag_origin_.x;
            const float dy=p.y-drag_origin_.y;
            for(auto& q:updated.points) { q.x+=dx; q.y+=dy; }
        } else {
            const auto handles=original_.Handles();
            const D2D1_POINT_2F q{std::clamp(p.x,0.0f,static_cast<float>(source_image_.width)),std::clamp(p.y,0.0f,static_cast<float>(source_image_.height))};
            if(handles.size()==2) { updated.points[transform_handle_==0 ? 0:updated.points.size()-1]=q; }
            else { updated.points={handles[(static_cast<size_t>(transform_handle_)+2)%4],q}; }
        }
        document_.Replace(*selected_,std::move(updated)); Invalidate(); return;
    }
    if(!has_pending_ || !dragging_) { return; }
    const auto last=pending_.points.back(); if(p.x==last.x && p.y==last.y) { return; }
    const bool freehand=pending_.tool==ToolId::Pen || (pending_.tool==ToolId::Mosaic && pending_.style.method==EffectMethod::Brush);
    RECT damage{};
    if(freehand) {
        const float pad=std::max(pending_.style.brush_size/2,pending_.style.thickness)+3;
        damage={static_cast<LONG>(std::floor(std::min(last.x,p.x)-pad)),static_cast<LONG>(std::floor(std::min(last.y,p.y)-pad)),
            static_cast<LONG>(std::ceil(std::max(last.x,p.x)+pad)),static_cast<LONG>(std::ceil(std::max(last.y,p.y)+pad))};
        pending_.points.push_back(p);
    } else {
        const RECT before=PendingDamage(); pending_.points.back()=p; const RECT after=PendingDamage(); ::UnionRect(&damage,&before,&after);
    }
    if(pending_.tool==ToolId::Spotlight) { Invalidate(); } else { InvalidateImage(damage); }
}
void ScreenshotEditor::EndStroke(POINT point) {
    if(transforming_) {
        UpdateStroke(point); transforming_=false; document_.CommitTransaction();
        if(callbacks_.on_document_committed && !original_.Same(document_.annotations()[*selected_])) { callbacks_.on_document_committed(); }
        Invalidate(); return;
    }
    if(!has_pending_ || !dragging_) { return; }
    const auto revision=document_.revision(); UpdateStroke(point); dragging_=false;
    const bool freehand=pending_.tool==ToolId::Pen || (pending_.tool==ToolId::Mosaic && pending_.style.method==EffectMethod::Brush);
    const auto box=NormalizedD2DRect(pending_.points.front(),pending_.points.back());
    if(freehand || box.right-box.left>=2 || box.bottom-box.top>=2) {
        document_.Apply(std::move(pending_));
        selected_.reset();
    }
    has_pending_=false; ReflowToolbar(); Invalidate(); NotifyMutation(revision);
}
void ScreenshotEditor::CancelStroke() {
    if(transforming_) { document_.CancelTransaction(); transforming_=false; Invalidate(); }
    if(!dragging_) { return; }
    const RECT damage=PendingDamage(); dragging_=has_pending_=false; pending_={}; InvalidateImage(damage);
}
void ScreenshotEditor::StartTextEditing(std::optional<size_t> index,POINT point) {
    if(editing_text_ && index && index==selected_) {
        ::SetFocus(text_edit_); return;
    }
    // Committing an empty active box erases it, shifting later hit indices.
    if(editing_text_ && index && selected_ && *selected_<*index &&
       document_.annotations()[*selected_].text.empty()) { --*index; }
    CommitPendingText();
    document_.BeginTransaction();
    if(!index) {
        Annotation a; a.tool=tool_==ToolId::Tag ? ToolId::Tag:ToolId::Text; a.style=style_; a.points={ToImagePoint(point)};
        document_.Apply(std::move(a)); index=document_.annotations().size()-1;
    }
    selected_=index; pending_=document_.annotations()[*index]; editing_text_=true; has_pending_=false;
    text_host_=::CreateWindowExW(kEditorExStyle,kEditorClassName,L"Edit annotation text",kEditorStyle,
        point.x,point.y,100,50,hwnd_,nullptr,::GetModuleHandleW(nullptr),this);
    if(text_host_) {
        text_edit_=::CreateWindowExW(0,L"EDIT",pending_.text.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_WANTRETURN|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_NOHIDESEL,
            0,0,100,50,text_host_,reinterpret_cast<HMENU>(2700),::GetModuleHandleW(nullptr),nullptr);
    }
    if(!text_edit_) {
        if(text_host_) { ::DestroyWindow(text_host_); text_host_=nullptr; }
        document_.CancelTransaction(); editing_text_=false; selected_.reset(); ui::ShowCaptureStatusNotice(region_,L"Could not open text editor"); Invalidate(); return;
    }
    ::SendMessageW(text_edit_,EM_SETLIMITTEXT,65535,0);
    last_text_double_=0;
    ui::SetAccessibleName(text_edit_,L"Annotation text");
    ::SetWindowSubclass(text_edit_,TextEditProc,1,reinterpret_cast<DWORD_PTR>(this));
    ::SetWindowDisplayAffinity(text_host_,0x11); LayoutTextEditor();
    ::ShowWindow(text_host_,SW_SHOWNORMAL); ::SetFocus(text_edit_);
    POINT local=point; ::ScreenToClient(text_edit_,&local);
    const auto caret=::SendMessageW(text_edit_,EM_CHARFROMPOS,0,MAKELPARAM(std::max(0L,local.x),std::max(0L,local.y)));
    ::SendMessageW(text_edit_,EM_SETSEL,LOWORD(caret),LOWORD(caret));
    ReflowToolbar(); Invalidate();
}
void ScreenshotEditor::LayoutTextEditor() {
    if(!text_host_ || !text_edit_ || !selected_) { return; }
    pending_=document_.annotations()[*selected_];
    const auto box=pending_.bounds(); const float em=pending_.FontPixels();
    if(!text_font_ || text_font_pixels_!=em) {
        HFONT next=ui::theme::CreateUiFont(em,1,false);
        if(next) { ::SendMessageW(text_edit_,WM_SETFONT,reinterpret_cast<WPARAM>(next),FALSE); if(text_font_) { ::DeleteObject(text_font_); } text_font_=next; text_font_pixels_=em; }
    }
    if(reinterpret_cast<HFONT>(::SendMessageW(text_edit_,WM_GETFONT,0,0))!=text_font_) {
        ::SendMessageW(text_edit_,WM_SETFONT,reinterpret_cast<WPARAM>(text_font_),FALSE);
    }
    const LONG pad=ui::theme::Scaled(5,scale_);
    const LONG x=source_bounds_.left+static_cast<LONG>(std::floor(box.left))-pad,y=source_bounds_.top+static_cast<LONG>(std::floor(box.top))-pad;
    const LONG w=std::max(2*pad+8L,static_cast<LONG>(std::ceil(box.right-box.left))+2*pad);
    const LONG h=std::max(2*pad+8L,static_cast<LONG>(std::ceil(box.bottom-box.top))+2*pad);
    ::SetWindowPos(text_host_,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE);
    ::MoveWindow(text_edit_,0,0,w,h,FALSE);
    // The input keeps its full layout/caret coordinates, but nothing (including
    // its selection frame) may paint beyond the capture canvas.
    RECT visible=platform::IntersectRects({x,y,x+w,y+h},source_bounds_);
    ::OffsetRect(&visible,-x,-y);
    HRGN clip=::CreateRectRgn(visible.left,visible.top,visible.right,visible.bottom);
    if(clip && !::SetWindowRgn(text_host_,clip,FALSE)) { ::DeleteObject(clip); }
    const float inset=pending_.tool==ToolId::Tag ? em*0.45f : pending_.style.text_mode==TextMode::Fill ? em*0.25f : pending_.style.text_mode==TextMode::Outline ? em*0.075f:0;
    RECT format{pad+static_cast<LONG>(inset+(pending_.tool==ToolId::Tag ? em*0.9f:0)),pad+static_cast<LONG>(inset),32760,32760};
    ::SendMessageW(text_edit_,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&format));
    ::InvalidateRect(text_edit_,nullptr,FALSE);

}
void ScreenshotEditor::UpdateTextFromControl() {
    if(!editing_text_ || !selected_ || !text_edit_) { return; }
    const int length=::GetWindowTextLengthW(text_edit_);
    std::wstring value(static_cast<size_t>(length)+1,L'\0'); ::GetWindowTextW(text_edit_,value.data(),length+1); value.resize(length);
    auto a=document_.annotations()[*selected_]; a.text=std::move(value); document_.Replace(*selected_,a); pending_=std::move(a);
    if(text_dispatch_depth_) { text_layout_pending_=true; }
    else { LayoutTextEditor(); }
    Invalidate();
}
LRESULT CALLBACK ScreenshotEditor::TextEditProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR reference) {
    auto* self=reinterpret_cast<ScreenshotEditor*>(reference);
    if(message==WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd,TextEditProc,id); return ::DefSubclassProc(hwnd,message,wparam,lparam); }
    if(message==WM_SHOWWINDOW) { ui::NativeRedrawGuard::VisibilityChanged(hwnd,wparam!=0); }
    if(message==WM_SETCURSOR || message==WM_LBUTTONDOWN) {
        POINT point{}; ::GetCursorPos(&point); ::ScreenToClient(hwnd,&point);
        if(message==WM_LBUTTONDOWN) { point={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; }
        RECT client{}; ::GetClientRect(hwnd,&client); const LONG edge=ui::theme::Scaled(4,self->scale_);
        const bool border=point.x<edge || point.y<edge || point.x>=client.right-edge || point.y>=client.bottom-edge;
        if(border) {
            if(message==WM_SETCURSOR) { ::SetCursor(::LoadCursorW(nullptr,IDC_SIZEALL)); return TRUE; }
            ::ClientToScreen(hwnd,&point); self->CommitPendingText();
            if(self->visible_ && self->selected_) {
                self->original_=self->document_.annotations()[*self->selected_]; self->drag_origin_=self->ToImagePoint(point);
                self->transform_handle_=-1; self->transforming_=true; self->document_.BeginTransaction();
                ::SetCapture(self->hwnd_); ::SetFocus(self->hwnd_);
            }
            return 0;
        }
    }
    if(message==WM_ERASEBKGND) { return 1; }
    if(message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT ps{}; HDC dc=message==WM_PAINT ? ::BeginPaint(hwnd,&ps):reinterpret_cast<HDC>(wparam);
        RECT client{}; ::GetClientRect(hwnd,&client);
        ::HideCaret(hwnd);
        { ui::PaintBuffer buffer(dc,client); self->PaintTextEditor(buffer.dc()); }
        ::ShowCaret(hwnd);
        if(message==WM_PAINT) { ::EndPaint(hwnd,&ps); } return 0;
    }
    if(message==WM_KEYDOWN && wparam==VK_ESCAPE) {
        self->editing_text_=false; self->document_.CancelTransaction();
        HWND host=self->text_host_; self->text_host_=self->text_edit_=nullptr; ::DestroyWindow(host);
        self->selected_.reset(); self->ReflowToolbar(); self->Invalidate(); return 0;
    }
    if(message==WM_KEYDOWN && wparam==VK_RETURN && (::GetKeyState(VK_CONTROL)&0x8000)) { self->CommitPendingText(); return 0; }
    if(message==WM_KEYDOWN && wparam=='A' && (::GetKeyState(VK_CONTROL)&0x8000)) { ::SendMessageW(hwnd,EM_SETSEL,0,-1); return 0; }
    if(message==WM_LBUTTONDBLCLK) { self->last_text_double_=::GetTickCount(); self->last_text_point_={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; }
    if(message==WM_LBUTTONDOWN && self->last_text_double_ && ::GetTickCount()-self->last_text_double_<=::GetDoubleClickTime() &&
        std::abs(GET_X_LPARAM(lparam)-self->last_text_point_.x)<=::GetSystemMetrics(SM_CXDOUBLECLK) &&
        std::abs(GET_Y_LPARAM(lparam)-self->last_text_point_.y)<=::GetSystemMetrics(SM_CYDOUBLECLK)) {
        const size_t index=LOWORD(::SendMessageW(hwnd,EM_CHARFROMPOS,0,lparam));
        const auto& value=self->document_.annotations()[*self->selected_].text;
        size_t start=std::min(index,value.size()),end=start;
        while(start>0 && value[start-1]!=L'\n') { --start; }
        while(end<value.size() && value[end]!=L'\n') { ++end; }
        if(end<value.size()) { ++end; }
        ::SendMessageW(hwnd,EM_SETSEL,start,end); self->last_text_double_=0; return 0;
    }
    const bool changes_pixels=(message>=WM_KEYFIRST && message<=WM_KEYLAST) ||
        (message>=WM_MOUSEFIRST && message<=WM_MOUSELAST) || message==EM_SETSEL || message==EM_REPLACESEL ||
        message==WM_SETTEXT || message==WM_SETFOCUS || message==WM_KILLFOCUS || message==WM_SIZE ||
        message==WM_CUT || message==WM_PASTE || message==WM_CLEAR || message==WM_UNDO || message==EM_UNDO ||
        message==WM_IME_COMPOSITION || message==WM_IME_ENDCOMPOSITION || message==WM_TIMER || message==WM_CAPTURECHANGED;
    const auto result=[&] {
        ++self->text_dispatch_depth_;
        ui::NativeRedrawGuard redraw(hwnd,changes_pixels);
        // EDIT needs visibility for native caret/selection updates, even while
        // its internal redraw flag suppresses synchronous GDI text painting.
        if(changes_pixels) { ::SetWindowLongPtrW(hwnd,GWL_STYLE,::GetWindowLongPtrW(hwnd,GWL_STYLE)|WS_VISIBLE); }
        const auto result=::DefSubclassProc(hwnd,message,wparam,lparam);
        --self->text_dispatch_depth_;
        return result;
    }();
    if(!self->text_dispatch_depth_ && self->text_layout_pending_) {
        self->text_layout_pending_=false;
        self->LayoutTextEditor();
    }
    if(changes_pixels && !self->text_dispatch_depth_ && ::IsWindow(hwnd) && ::GetFocus()==hwnd && self->selected_) {
        DWORD start{},end{};
        ::SendMessageW(hwnd,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
        // The native caret can be parked outside the old client bounds while
        // typing grows the host. Position it against the rendered text after
        // layout; native EDIT still owns selection, navigation and caret life.
        if(start==end) {
            const auto& a=self->document_.annotations()[*self->selected_];
            auto layout=TextLayout(a);
            float x{},y{}; DWRITE_HIT_TEST_METRICS metrics{};
            if(layout && SUCCEEDED(layout->HitTestTextPosition(start,FALSE,&x,&y,&metrics))) {
                RECT format{}; ::SendMessageW(hwnd,EM_GETRECT,0,reinterpret_cast<LPARAM>(&format));
                ::SetCaretPos(format.left+static_cast<int>(std::lround(x)),format.top+static_cast<int>(std::lround(y)));
            }
        }
    }
    return result;
}
void ScreenshotEditor::PaintTextEditor(HDC dc) {
    if(!text_edit_ || !selected_) { return; }
    RECT bounds{}; ::GetWindowRect(text_edit_,&bounds); RenderImage(dc,bounds);
    DWORD start{},end{}; ::SendMessageW(text_edit_,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
    if(start==end) { return; }
    const auto& a=document_.annotations()[*selected_]; auto layout=TextLayout(a); if(!layout) { return; }
    const float em=a.FontPixels(); const float inset=a.tool==ToolId::Tag ? em*0.45f : a.style.text_mode==TextMode::Fill ? em*0.25f : a.style.text_mode==TextMode::Outline ? em*0.075f:0;
    const float x=source_bounds_.left-bounds.left+a.points.front().x+inset+(a.tool==ToolId::Tag ? em*0.9f:0), y=source_bounds_.top-bounds.top+a.points.front().y+inset;
    UINT32 count{}; layout->HitTestTextRange(start,end-start,x,y,nullptr,0,&count);
    std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
    if(SUCCEEDED(layout->HitTestTextRange(start,end-start,x,y,ranges.data(),count,&count))) {
        for(const auto& r:ranges) { RECT area{static_cast<LONG>(r.left),static_cast<LONG>(r.top),static_cast<LONG>(std::ceil(r.left+r.width)),static_cast<LONG>(std::ceil(r.top+r.height))}; ::InvertRect(dc,&area); }
    }
}
void ScreenshotEditor::CommitPendingText() {
    if(!editing_text_) { return; }
    UpdateTextFromControl(); editing_text_=false;
    if(selected_ && document_.annotations()[*selected_].text.empty()) { document_.Remove(*selected_); selected_.reset(); }
    document_.CommitTransaction();
    if(text_host_) { HWND host=text_host_; text_host_=text_edit_=nullptr; ::DestroyWindow(host); }
    has_pending_=false;
    if(callbacks_.on_document_committed) { callbacks_.on_document_committed(); }
    Invalidate();
}
void ScreenshotEditor::OnCharacter(wchar_t character) {
    if(editing_text_ && text_edit_) { ::SendMessageW(text_edit_,WM_CHAR,character,0); }
}
void ScreenshotEditor::DrawSelection(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush) const {
    if(!selected_ || *selected_>=document_.annotations().size()) { return; }
    const auto& a=document_.annotations()[*selected_]; if(a.tool==ToolId::Watermark || a.points.empty()) { return; }
    brush->SetColor(ui::theme::D2DColor(ui::theme::Current().focus)); const float width=1.2f*scale_;
    const auto box=NormalizedD2DRect(a.points.front(),a.points.back());
    if(a.tool==ToolId::Text || a.tool==ToolId::Tag) {
        auto r=a.bounds(); const float pad=3*scale_; r.left-=pad; r.top-=pad; r.right+=pad; r.bottom+=pad;
        target->DrawRoundedRectangle(D2D1::RoundedRect(r,5*scale_,5*scale_),brush,width);
    } else if(a.tool==ToolId::Ellipse || (a.tool==ToolId::Spotlight && a.style.shape==SpotlightShape::Ellipse)) {
        target->DrawEllipse(D2D1::Ellipse({(box.left+box.right)/2,(box.top+box.bottom)/2},(box.right-box.left)/2,(box.bottom-box.top)/2),brush,width);
    } else if(a.tool==ToolId::Pen || a.tool==ToolId::Line || a.tool==ToolId::Arrow) {
        for(size_t i=1;i<a.points.size();++i) { target->DrawLine(a.points[i-1],a.points[i],brush,width,RoundStrokeStyle()); }
    } else if(a.tool==ToolId::Spotlight && a.style.shape==SpotlightShape::Rounded) {
        const float radius=std::min({(box.right-box.left)/2,(box.bottom-box.top)/2,16*a.style.image_scale});
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush,width);
    } else {
        const auto r=a.tool==ToolId::Mosaic && a.style.method==EffectMethod::Brush ? a.bounds():box;
        target->DrawRectangle(r,brush,width);
    }
    for(auto point:a.Handles()) {
        brush->SetColor(D2D1::ColorF(D2D1::ColorF::White)); target->FillEllipse(D2D1::Ellipse(point,4.5f*scale_,4.5f*scale_),brush);
        brush->SetColor(ui::theme::D2DColor(ui::theme::Current().focus)); target->DrawEllipse(D2D1::Ellipse(point,4.5f*scale_,4.5f*scale_),brush,1.5f*scale_);
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

namespace {
// Three separable box passes approximate a Gaussian. Sliding sums keep cost
// linear in pixels, independent of strength, and all input is derived afresh.
void SmoothBlur(platform::BgraImage& image,int radius) {
    std::vector<uint32_t> scratch(image.pixels.size());
    const int w=image.width,h=image.height;
    const auto pass=[&](bool horizontal) {
        const int major=horizontal ? h:w, minor=horizontal ? w:h;
        const int count=radius*2+1;
        for(int row=0;row<major;++row) {
            const auto index=[&](int col) { return horizontal ? static_cast<size_t>(row)*w+std::clamp(col,0,w-1) : static_cast<size_t>(std::clamp(col,0,h-1))*w+row; };
            int red=0,green=0,blue=0;
            const auto accumulate=[&](uint32_t pixel,int sign) { red+=static_cast<int>((pixel>>16)&255)*sign; green+=static_cast<int>((pixel>>8)&255)*sign; blue+=static_cast<int>(pixel&255)*sign; };
            for(int k=-radius;k<=radius;++k) { accumulate(image.pixels[index(k)],1); }
            for(int col=0;col<minor;++col) {
                scratch[index(col)]=0xff000000u|(static_cast<uint32_t>(red/count)<<16)|(static_cast<uint32_t>(green/count)<<8)|static_cast<uint32_t>(blue/count);
                accumulate(image.pixels[index(col-radius)],-1); accumulate(image.pixels[index(col+radius+1)],1);
            }
        }
        image.pixels.swap(scratch);
    };
    for(int i=0;i<3;++i) { pass(true); pass(false); }
}
}

void ScreenshotEditor::RebuildRedactedBase() {
    const bool effect_preview=has_pending_ && (pending_.tool==ToolId::Mosaic || pending_.tool==ToolId::Spotlight);
    if((redaction_revision_==document_.revision() && !effect_preview) || !has_image()) { return; }
    std::vector<Annotation> effects;
    for(const auto& a:document_.annotations()) if(a.tool==ToolId::Mosaic || a.tool==ToolId::Spotlight) { effects.push_back(a); }
    if(effect_preview) { effects.push_back(pending_); }
    redaction_revision_=effect_preview ? UINT64_MAX:document_.revision();
    if(redaction_cache_ && SameList(redaction_cache_->effects,effects)) { return; }
    redacted_image_=source_image_;
    const int w=source_image_.width,h=source_image_.height;
    CoverageMap coverage; coverage.Reset(w,h);
    std::vector<uint8_t> mask(source_image_.pixels.size());
    std::vector<float> spots;
    float dim=0;
    for(const auto& a:effects) {
        if(a.points.empty()) { continue; }
        const auto box=a.bounds();
        const RECT r{std::clamp(static_cast<LONG>(std::floor(box.left)),0L,static_cast<LONG>(w)),
            std::clamp(static_cast<LONG>(std::floor(box.top)),0L,static_cast<LONG>(h)),
            std::clamp(static_cast<LONG>(std::ceil(box.right)),0L,static_cast<LONG>(w)),
            std::clamp(static_cast<LONG>(std::ceil(box.bottom)),0L,static_cast<LONG>(h))};
        if(::IsRectEmpty(&r)) { continue; }
        if(a.tool==ToolId::Spotlight) {
            if(spots.empty()) { spots.resize(source_image_.pixels.size()); }
            dim=std::max(dim,a.style.opacity/100);
            const float rx=(box.right-box.left)/2,ry=(box.bottom-box.top)/2;
            for(LONG y=r.top;y<r.bottom;++y) for(LONG x=r.left;x<r.right;++x) {
                float inside=1;
                const float px=x+0.5f-(box.left+rx),py=y+0.5f-(box.top+ry);
                if(a.style.shape==SpotlightShape::Ellipse && rx>0 && ry>0) {
                    inside=std::clamp((1-std::hypot(px/rx,py/ry))*std::min(rx,ry)+0.5f,0.0f,1.0f);
                } else if(a.style.shape==SpotlightShape::Rounded) {
                    const float radius=std::min({rx,ry,16*a.style.image_scale});
                    const float qx=std::abs(px)-(rx-radius),qy=std::abs(py)-(ry-radius);
                    const float d=std::hypot(std::max(qx,0.0f),std::max(qy,0.0f))+std::min(std::max(qx,qy),0.0f)-radius;
                    inside=std::clamp(0.5f-d,0.0f,1.0f);
                }
                auto& value=spots[static_cast<size_t>(y)*w+x]; value=std::max(value,inside);
            }
            continue;
        }
        if(a.style.method==EffectMethod::Brush) {
            coverage.Stroke(a.points,a.style.brush_size/2); coverage.CurrentMask(mask);
            for(auto& value:mask) { value=value ? 1:0; }
        } else {
            std::fill(mask.begin(),mask.end(),uint8_t{0});
            for(LONG y=r.top;y<r.bottom;++y) { std::fill(mask.begin()+static_cast<ptrdiff_t>(y)*w+r.left,mask.begin()+static_cast<ptrdiff_t>(y)*w+r.right,uint8_t{1}); }
        }
        if(a.style.effect==EffectMode::Pixelate) {
            platform::ApplyMosaicMasked(redacted_image_,mask,1,r,std::clamp(static_cast<int>(2+a.style.strength*0.7f),2,72));
        } else {
            const int radius=std::clamp(static_cast<int>(1+a.style.strength*0.35f),1,36);
            const RECT expanded{std::max(0L,r.left-radius*3),std::max(0L,r.top-radius*3),
                std::min(static_cast<LONG>(w),r.right+radius*3),std::min(static_cast<LONG>(h),r.bottom+radius*3)};
            platform::BgraImage blurred;
            if(platform::CropImage(redacted_image_,expanded,blurred)) {
                SmoothBlur(blurred,radius);
                for(LONG y=r.top;y<r.bottom;++y) for(LONG x=r.left;x<r.right;++x) if(mask[static_cast<size_t>(y)*w+x]) {
                    redacted_image_.row(y)[x]=blurred.row(y-expanded.top)[x-expanded.left];
                }
            }
        }
    }
    if(!spots.empty()) for(size_t i=0;i<spots.size();++i) {
        const float factor=1-dim*(1-spots[i]); const auto pixel=redacted_image_.pixels[i];
        redacted_image_.pixels[i]=0xff000000u|(static_cast<uint32_t>(((pixel>>16)&255)*factor)<<16)|
            (static_cast<uint32_t>(((pixel>>8)&255)*factor)<<8)|static_cast<uint32_t>((pixel&255)*factor);
    }
    redaction_cache_=std::make_unique<RedactionCache>(); redaction_cache_->effects=std::move(effects);
    image_bitmap_.Reset();
}

void ScreenshotEditor::RenderImage(HDC dc, const RECT& bounds, const RECT* repaint) {
    auto& graphics = platform::GraphicsEnvironment::Get();
    if (!has_image() || !graphics.EnsureInitialized() || !dc) { return; }
    RebuildRedactedBase();
    if (!image_target_) {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(graphics.d2d_factory()->CreateDCRenderTarget(&properties, &image_target_))) { return; }
        image_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &image_brush_);
    }
    RECT client{0, 0, platform::RectWidth(bounds), platform::RectHeight(bounds)};
    if (repaint) { client = platform::IntersectRects(client, *repaint); }
    if (platform::IsEmptyRect(client) || FAILED(image_target_->BindDC(dc, &client))) { return; }
    if (!image_bitmap_) { image_bitmap_ = platform::CreateD2DBitmapForTarget(image_target_.Get(), redacted_image_); }
    if (!image_bitmap_ || !image_brush_) { return; }
    image_target_->BeginDraw();
    image_target_->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(source_bounds_.left - bounds.left - client.left), static_cast<float>(source_bounds_.top - bounds.top - client.top)));
    image_target_->PushAxisAlignedClip(D2D1::RectF(0,0,static_cast<float>(source_image_.width),static_cast<float>(source_image_.height)),D2D1_ANTIALIAS_MODE_ALIASED);
    image_target_->DrawBitmap(image_bitmap_.Get());
    for (const auto& annotation : document_.annotations()) {
        DrawAnnotation(image_target_.Get(), annotation, image_brush_.Get(), false);
    }
    if (has_pending_ && pending_.tool!=ToolId::Mosaic && pending_.tool!=ToolId::Spotlight) {
        DrawAnnotation(image_target_.Get(), pending_, image_brush_.Get(), true);
    }
    DrawSelection(image_target_.Get(),image_brush_.Get());
    image_target_->PopAxisAlignedClip();
    if (FAILED(image_target_->EndDraw())) {
        image_bitmap_.Reset(); image_brush_.Reset(); image_target_.Reset();
    }
}

void ScreenshotEditor::PaintToolbar(HDC dc) const {
    using namespace ui;
    RECT client{0, 0, client_width_, client_height_};
    PaintBuffer buffer(dc,client); dc=buffer.dc();
    theme::DrawPanel(dc, client, scale_);
    theme::DrawGrip(dc, {0, 0, static_cast<LONG>(kIslandGrip * scale_), toolbar_rect_.bottom}, scale_);
    for(const auto& split:main_separators_) { DrawSeparator(dc,split,scale_); }
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

platform::BgraImage ScreenshotEditor::Composite() {
    RebuildRedactedBase();

    platform::BgraImage output;
    auto& graphics = platform::GraphicsEnvironment::Get();
    if (!graphics.EnsureInitialized() || redacted_image_.empty()) {
        return output;
    }
    platform::BgraImage cropped;
    const RECT crop{region_.left - source_bounds_.left, region_.top - source_bounds_.top,
                    region_.right - source_bounds_.left, region_.bottom - source_bounds_.top};
    if (!platform::CropImage(redacted_image_, crop, cropped)) { return {}; }
    if (document_.annotations().empty()) {
        return cropped;  // Nothing to composite.
    }

    // Always the captured resolution - never the window or preview size.
    const UINT width = static_cast<UINT>(cropped.width);
    const UINT height = static_cast<UINT>(cropped.height);
    ComPtr<IWICBitmap> wic_bitmap;
    HRESULT hr = graphics.wic_factory()->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                                      WICBitmapCacheOnLoad, &wic_bitmap);
    if (FAILED(hr)) {
        platform::LogHresult(L"WIC CreateBitmap(composite)", hr);
        return {};
    }

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> render_target;
    hr = graphics.d2d_factory()->CreateWicBitmapRenderTarget(wic_bitmap.Get(), properties, &render_target);
    if (FAILED(hr)) {
        platform::LogHresult(L"CreateWicBitmapRenderTarget", hr);
        return {};
    }

    ComPtr<ID2D1Bitmap> base = platform::CreateD2DBitmapForTarget(render_target.Get(), cropped);
    ComPtr<ID2D1SolidColorBrush> brush;
    render_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);

    if (!base || !brush) { return {}; }
    render_target->BeginDraw();
    render_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
    if (base != nullptr) {
        render_target->DrawBitmap(base.Get(),
                                  D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)));
    }
    render_target->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(crop.left), -static_cast<float>(crop.top)));
    for (const Annotation& annotation : document_.annotations()) {
        DrawAnnotation(render_target.Get(), annotation, brush.Get(), false);
    }
    hr = render_target->EndDraw();
    if (FAILED(hr)) {
        platform::LogHresult(L"Composite EndDraw", hr);
        return {};
    }

    WICRect lock_rect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    ComPtr<IWICBitmapLock> lock;
    hr = wic_bitmap->Lock(&lock_rect, WICBitmapLockRead, &lock);
    if (FAILED(hr)) {
        platform::LogHresult(L"IWICBitmap::Lock", hr);
        return {};
    }
    UINT stride = 0;
    UINT buffer_size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&buffer_size, &data)) || data == nullptr) {
        return {};
    }

    output.Reset(static_cast<int>(width), static_cast<int>(height));
    for (int y = 0; y < output.height; ++y) {
        std::memcpy(output.row(y), data + static_cast<size_t>(y) * stride, output.stride_bytes());
    }
    output.MakeOpaque();
    return output;
}

bool ScreenshotEditor::CopyToClipboard() {
    if (!EnsureImage()) { return false; }
    completing_ = true;
    CommitPendingText();
    completing_ = false;
    const platform::BgraImage image = Composite();
    if (image.empty()) {
        ui::ShowCaptureStatusNotice(region_,L"Nothing to copy");
        Invalidate();
        return false;
    }
    if (!platform::CopyImageToClipboard(hwnd_, image)) {
        // Failure safety: the session stays open so no capture is lost.
        ui::ShowCaptureStatusNotice(region_,L"Clipboard is unavailable - try again or use Save");
        Invalidate();
        return false;
    }
    platform::LogMessage(L"Screenshot copied to the clipboard");
    return true;
}

bool ScreenshotEditor::SaveToFile() {
    if (!EnsureImage()) { return false; }
    CommitPendingText();
    if (!visible_) { return false; }
    const platform::BgraImage image = Composite();
    if (image.empty()) {
        ui::ShowCaptureStatusNotice(region_,L"Nothing to save");
        Invalidate();
        return false;
    }

    const std::wstring suggested = platform::TimestampedFileName(L"Easnap", L"png");
    const std::wstring initial = settings_ != nullptr ? settings_->last_save_directory : std::wstring();
    // Topmost owned islands and combo lists must not cover a native file dialog.
    // Freeze their timers/placement during its nested message loop.
    modal_active_=true;
    ShowBrushPopup(false);
    ::KillTimer(context_,0xEA61); context_fade_started_=0;
    ::KillTimer(context_,0xEA63);
    ::SendMessageW(Control(2600),CB_SHOWDROPDOWN,FALSE,0);
    if(tooltip_) { ::SendMessageW(tooltip_,TTM_POP,0,0); ::SendMessageW(tooltip_,TTM_ACTIVATE,FALSE,0); }
    ::ShowWindow(context_,SW_HIDE);
    const auto destination = platform::ShowSaveImageDialog(hwnd_, suggested, initial);
    modal_active_=false;
    if(tooltip_) { ::SendMessageW(tooltip_,TTM_ACTIVATE,TRUE,0); }
    if(visible_) { PlaceContext(); }
    if (!destination) {
        return false;  // Cancelled: keep editing.
    }
    if (!platform::SaveImage(image, *destination)) {
        ui::ShowCaptureStatusNotice(region_,L"Save failed - the capture is still here, try another location");
        Invalidate();
        return false;
    }
    if (settings_ != nullptr) {
        settings_->last_save_directory = platform::DirectoryOf(*destination);
    }
    platform::LogMessage(L"Screenshot exported successfully");
    return true;
}

}  // namespace easnap::editor
