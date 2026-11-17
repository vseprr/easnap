// Easnap - how annotations are drawn with Direct2D.
#include "editor/annotation_renderer.h"

#include "editor/annotation_palette.h"
#include "editor/annotation_tools.h"
#include "platform/graphics_device.h"
#include "ui/ui_theme.h"

#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace easnap::editor {

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

namespace {

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

/// The bounding box of `area` mapped through `transform`.
D2D1_RECT_F TransformedBounds(const D2D1_RECT_F& area, const D2D1::Matrix3x2F& transform) {
    const std::array<D2D1_POINT_2F, 4> corners{{{area.left, area.top}, {area.right, area.top}, {area.right, area.bottom}, {area.left, area.bottom}}};
    const D2D1_POINT_2F first = transform.TransformPoint(corners[0]);
    D2D1_RECT_F bounds{first.x, first.y, first.x, first.y};
    for (const auto& corner : corners) {
        const D2D1_POINT_2F point = transform.TransformPoint(corner);
        bounds.left = std::min(bounds.left, point.x);
        bounds.top = std::min(bounds.top, point.y);
        bounds.right = std::max(bounds.right, point.x);
        bounds.bottom = std::max(bounds.bottom, point.y);
    }
    return bounds;
}

/// The part of the space `transform` maps onto `target` that the target
/// shows, grown by `margin` target pixels for antialiasing. Empty when the
/// transform cannot be inverted.
std::optional<D2D1_RECT_F> ShownArea(ID2D1RenderTarget* target, const D2D1_MATRIX_3X2_F& m, float margin) {
    const float determinant = m._11 * m._22 - m._12 * m._21;
    if (determinant == 0.0f || !std::isfinite(determinant)) { return std::nullopt; }
    // The inverse of the 2x3 matrix, written out.
    const D2D1::Matrix3x2F inverse(m._22 / determinant, -m._12 / determinant, -m._21 / determinant, m._11 / determinant,
                                   (m._21 * m._32 - m._22 * m._31) / determinant,
                                   (m._12 * m._31 - m._11 * m._32) / determinant);
    const D2D1_SIZE_F size = target->GetSize();
    const D2D1_RECT_F shown = TransformedBounds({-margin, -margin, size.width + margin, size.height + margin}, inverse);
    if (!std::isfinite(shown.left) || !std::isfinite(shown.top) || !std::isfinite(shown.right) || !std::isfinite(shown.bottom)) {
        return std::nullopt;
    }
    return shown;
}

// DirectWrite supplies glyph outlines, so Outline is a glyph stroke, never a
// selection rectangle or a bitmap dilation of the whole label.
class OutlineRenderer final : public IDWriteTextRenderer {
public:
    OutlineRenderer(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_COLOR_F outline, D2D1_COLOR_F fill, float width)
        : target_(target), brush_(brush), outline_(outline), fill_(fill), width_(width) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) { return E_POINTER; }
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) || iid == __uuidof(IDWriteTextRenderer)) {
            *result = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        D2D1_MATRIX_3X2_F matrix;
        target_->GetTransform(&matrix);
        std::memcpy(transform, &matrix, sizeof(matrix));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override {
        *pixels = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE,
                                           const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        ComPtr<ID2D1PathGeometry> geometry;
        ComPtr<ID2D1GeometrySink> sink;
        auto* factory = platform::GraphicsEnvironment::Get().d2d_factory();
        HRESULT hr = factory->CreatePathGeometry(&geometry);
        if (FAILED(hr)) { return hr; }
        hr = geometry->Open(&sink);
        if (FAILED(hr)) { return hr; }
        hr = run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                                               run->glyphCount, run->isSideways, run->bidiLevel % 2, sink.Get());
        if (FAILED(hr)) { return hr; }
        hr = sink->Close();
        if (FAILED(hr)) { return hr; }
        D2D1_MATRIX_3X2_F original;
        target_->GetTransform(&original);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * original);
        brush_->SetColor(outline_);
        target_->DrawGeometry(geometry.Get(), brush_, width_, RoundStrokeStyle());
        brush_->SetColor(fill_);
        target_->FillGeometry(geometry.Get(), brush_);
        target_->SetTransform(original);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* line, IUnknown*) override {
        brush_->SetColor(fill_);
        target_->FillRectangle({x, y + line->offset, x + line->width, y + line->offset + line->thickness}, brush_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* line, IUnknown*) override {
        brush_->SetColor(fill_);
        target_->FillRectangle({x, y + line->offset, x + line->width, y + line->offset + line->thickness}, brush_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object, BOOL sideways, BOOL rtl, IUnknown* effect) override {
        return object->Draw(context, this, x, y, sideways, rtl, effect);
    }

private:
    ID2D1RenderTarget* target_;
    ID2D1SolidColorBrush* brush_;
    D2D1_COLOR_F outline_, fill_;
    float width_;
};

}  // namespace

/// Draws one annotation in image space. Mosaic strokes are baked into the base
/// bitmap beforehand, so they only render a translucent trail while being drawn.
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
            if (annotation.points.size() >= 2) { target->DrawLine(annotation.points.front(), annotation.points.back(), brush, thickness, RoundStrokeStyle()); }
            break;
        case ToolId::Text:
        case ToolId::Tag: {
            auto layout = TextLayout(annotation);
            if (!layout || annotation.text.empty()) { break; }
            const auto box = annotation.bounds();
            auto origin = annotation.points.front();
            const float em = annotation.FontPixels();
            const bool white = annotation.style.color.r > kWhiteTextThreshold && annotation.style.color.g > kWhiteTextThreshold && annotation.style.color.b > kWhiteTextThreshold;
            const auto ink = white ? kTextInkOnWhite : kTextInk;
            if (annotation.tool == ToolId::Tag) {
                const float dot = em * 0.16f, body = origin.x + em * 0.9f;
                brush->SetColor(annotation.style.color);
                target->FillEllipse(D2D1::Ellipse({origin.x + dot, (box.top + box.bottom) / 2}, dot, dot), brush);
                brush->SetColor(kTagBody);
                target->FillRoundedRectangle(D2D1::RoundedRect({body, box.top, box.right, box.bottom}, em * 0.2f, em * 0.2f), brush);
                ComPtr<ID2D1PathGeometry> notch;
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(platform::GraphicsEnvironment::Get().d2d_factory()->CreatePathGeometry(&notch)) && SUCCEEDED(notch->Open(&sink))) {
                    const float mid = (box.top + box.bottom) / 2;
                    sink->BeginFigure({body - em * 0.25f, mid}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({body + 1, mid - em * 0.25f});
                    sink->AddLine({body + 1, mid + em * 0.25f});
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    target->FillGeometry(notch.Get(), brush);
                }
                origin = {body + em * 0.45f, origin.y + em * 0.45f};
                brush->SetColor(kTagText);
                target->DrawTextLayout(origin, layout.Get(), brush);
                break;
            }
            if (annotation.style.text_mode == TextMode::Fill) {
                target->FillRoundedRectangle(D2D1::RoundedRect(box, em * 0.22f, em * 0.22f), brush);
                origin.x += em * 0.25f;
                origin.y += em * 0.25f;
                brush->SetColor(ink);
            }
            if (annotation.style.text_mode == TextMode::Outline) {
                origin.x += em * 0.075f;
                origin.y += em * 0.075f;
                OutlineRenderer renderer(target, brush, annotation.style.color, ink, em * 0.12f);
                layout->Draw(nullptr, &renderer, origin.x, origin.y);
            } else {
                target->DrawTextLayout(origin, layout.Get(), brush);
            }
            break;
        }
        case ToolId::Watermark: {
            auto layout = TextLayout(annotation);
            if (!layout || annotation.text.empty() || annotation.points.size() < 2) { break; }
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            const float stepX = std::max(annotation.FontPixels() * 5, metrics.width + annotation.FontPixels() * 3);
            const float stepY = std::max(annotation.FontPixels() * 4, metrics.height * 3);
            D2D1_MATRIX_3X2_F original;
            target->GetTransform(&original);
            auto color = annotation.style.color;
            color.a = annotation.style.opacity / 100;
            brush->SetColor(color);
            target->PushAxisAlignedClip({0, 0, annotation.points.back().x, annotation.points.back().y}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const D2D1_POINT_2F center{annotation.points.back().x / 2, annotation.points.back().y / 2};
            const float reach = std::hypot(center.x, center.y) + std::max(stepX, stepY);
            target->SetTransform(D2D1::Matrix3x2F::Rotation(-18, center) * original);
            const int columns = static_cast<int>(std::ceil(reach / stepX)), rows = static_cast<int>(std::ceil(reach / stepY));
            // A tall picture has thousands of tiles. Only those that can reach
            // what the target shows are drawn, in the order of the full grid.
            int first_row = -rows, last_row = rows, first_column = -columns, last_column = columns;
            if (const auto shown = ShownArea(target, original, 2.0f)) {
                // Tiles are laid out in image space turned back by their rotation.
                const D2D1_RECT_F area = TransformedBounds(*shown, D2D1::Matrix3x2F::Rotation(18, center));
                // Glyphs may overhang their layout box: an em on every side.
                const float em = annotation.FontPixels();
                const float width = metrics.widthIncludingTrailingWhitespace, height = metrics.height;
                const auto index = [](float position, float step, int limit, bool upward) {
                    const float value = upward ? std::ceil(position / step) : std::floor(position / step);
                    return static_cast<int>(std::clamp(value, static_cast<float>(-limit), static_cast<float>(limit)));
                };
                first_column = index(area.left - em - center.x - width / 2, stepX, columns, false);
                last_column = index(area.right + em - center.x + width / 2, stepX, columns, true);
                first_row = index(area.top - em - center.y - height / 2, stepY, rows, false);
                last_row = index(area.bottom + em - center.y + height / 2, stepY, rows, true);
            }
            for (int row = first_row; row <= last_row; ++row)
                for (int column = first_column; column <= last_column; ++column) {
                    const D2D1_POINT_2F origin{center.x + column * stepX - metrics.widthIncludingTrailingWhitespace / 2,
                                               center.y + row * stepY - metrics.height / 2};
                    target->DrawTextLayout(origin, layout.Get(), brush);
                }
            target->SetTransform(original);
            target->PopAxisAlignedClip();
            break;
        }
        case ToolId::Spotlight: break; // Union dim layer is derived with the base pixels.
        case ToolId::Mosaic: {
            if (!preview) {
                break;  // Already destructive on the pixels.
            }
            brush->SetColor(kMosaicBrushTrail);
            DrawFreehand(target, annotation.points, brush, std::max(2.0f, annotation.style.brush_size));
            break;
        }
        case ToolId::Pointer:
        default:
            break;
    }
}

void DrawSelectionChrome(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, const Annotation& a, float unit) {
    if (!IsSelectable(a.tool) || a.points.empty()) { return; }
    brush->SetColor(ui::theme::D2DColor(ui::theme::Current().focus));
    const float width = kSelectionStroke * unit;
    const auto box = NormalizedD2DRect(a.points.front(), a.points.back());
    if (IsTextTool(a.tool)) {
        auto r = a.bounds();
        const float pad = kSelectionTextGap * unit;
        r.left -= pad;
        r.top -= pad;
        r.right += pad;
        r.bottom += pad;
        target->DrawRoundedRectangle(D2D1::RoundedRect(r, 5 * unit, 5 * unit), brush, width);
    } else if (a.tool == ToolId::Ellipse || (a.tool == ToolId::Spotlight && a.style.shape == SpotlightShape::Ellipse)) {
        target->DrawEllipse(D2D1::Ellipse({(box.left + box.right) / 2, (box.top + box.bottom) / 2}, (box.right - box.left) / 2, (box.bottom - box.top) / 2), brush, width);
    } else if (a.tool == ToolId::Pen || a.tool == ToolId::Line || a.tool == ToolId::Arrow) {
        for (size_t i = 1; i < a.points.size(); ++i) { target->DrawLine(a.points[i - 1], a.points[i], brush, width, RoundStrokeStyle()); }
    } else if (a.tool == ToolId::Spotlight && a.style.shape == SpotlightShape::Rounded) {
        const float radius = std::min({(box.right - box.left) / 2, (box.bottom - box.top) / 2, 16 * a.style.image_scale});
        target->DrawRoundedRectangle(D2D1::RoundedRect(box, radius, radius), brush, width);
    } else {
        const auto r = a.tool == ToolId::Mosaic && a.style.method == EffectMethod::Brush ? a.bounds() : box;
        target->DrawRectangle(r, brush, width);
    }
    for (auto point : a.Handles()) {
        brush->SetColor(ui::theme::D2DColor(ui::theme::kSelectionHandleFill));
        target->FillEllipse(D2D1::Ellipse(point, kHandleRadius * unit, kHandleRadius * unit), brush);
        brush->SetColor(ui::theme::D2DColor(ui::theme::Current().focus));
        target->DrawEllipse(D2D1::Ellipse(point, kHandleRadius * unit, kHandleRadius * unit), brush, kHandleStroke * unit);
    }
}

}  // namespace easnap::editor
