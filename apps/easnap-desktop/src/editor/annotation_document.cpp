// Easnap - the annotation document of the screenshot editor.
#include "editor/annotation_document.h"

#include "editor/annotation_tools.h"
#include "platform/graphics_device.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace easnap::editor {

D2D1_RECT_F NormalizedD2DRect(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    return D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
}

// ---------------------------------------------------------------------------
// Annotation / AnnotationDocument
// ---------------------------------------------------------------------------

ComPtr<IDWriteTextLayout> TextLayout(const Annotation& a) {
    struct Entry {
        std::wstring text;
        float size;
        ComPtr<IDWriteTextLayout> layout;
    };
    static thread_local std::deque<Entry> cache;
    const float size = a.FontPixels();
    for (const auto& e : cache)
        if (e.text == a.text && e.size == size) { return e.layout; }
    auto& graphics = platform::GraphicsEnvironment::Get();
    if (!graphics.EnsureInitialized()) { return {}; }
    auto format = graphics.CreateTextFormat(size, DWRITE_FONT_WEIGHT_NORMAL);
    if (!format) { return {}; }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(graphics.dwrite_factory()->CreateTextLayout(a.text.c_str(), static_cast<UINT32>(a.text.size()), format.Get(),
                                                           65536, 65536, &layout))) { return {}; }
    if (cache.size() == 64) { cache.pop_front(); }
    cache.push_back({a.text, size, layout});
    return layout;
}

namespace {
float SegmentDistance(D2D1_POINT_2F p, D2D1_POINT_2F a, D2D1_POINT_2F b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float t = dx * dx + dy * dy > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f) : 0;
    return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}
bool SameList(const std::vector<Annotation>& a, const std::vector<Annotation>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) { return x.Same(y); });
}
}  // namespace

D2D1_RECT_F Annotation::bounds() const {
    if (points.empty()) { return {}; }
    auto box = NormalizedD2DRect(points.front(), points.front());
    for (const auto& point : points) {
        box.left = std::min(box.left, point.x);
        box.top = std::min(box.top, point.y);
        box.right = std::max(box.right, point.x);
        box.bottom = std::max(box.bottom, point.y);
    }
    if (IsTextTool(tool)) {
        auto layout = TextLayout(*this);
        DWRITE_TEXT_METRICS metrics{};
        if (layout) { layout->GetMetrics(&metrics); }
        const float padding = tool == ToolId::Tag ? FontPixels() * 0.45f : style.text_mode == TextMode::Fill  ? FontPixels() * 0.25f
                                                                       : style.text_mode == TextMode::Outline ? FontPixels() * 0.075f
                                                                                                              : 0;
        box.right = box.left + std::max(1.0f, metrics.widthIncludingTrailingWhitespace) + padding * 2;
        box.bottom = box.top + std::max(FontPixels(), metrics.height) + padding * 2;
        if (tool == ToolId::Tag) { box.right += FontPixels() * 0.9f; }
        return box;
    }
    const float pad = tool == ToolId::Mosaic ? (style.method == EffectMethod::Brush ? style.brush_size / 2 : 0) : tool == ToolId::Spotlight ? 0
                                                                                                              : tool == ToolId::Arrow       ? style.thickness * 3
                                                                                                                                            : style.thickness / 2;
    box.left -= pad;
    box.top -= pad;
    box.right += pad;
    box.bottom += pad;
    return box;
}

bool Annotation::Same(const Annotation& b) const {
    const auto& x = style;
    const auto& y = b.style;
    return tool == b.tool && text == b.text && points.size() == b.points.size() &&
           x.color.r == y.color.r && x.color.g == y.color.g && x.color.b == y.color.b && x.color.a == y.color.a &&
           x.thickness == y.thickness && x.font_size == y.font_size && x.image_scale == y.image_scale &&
           x.brush_size == y.brush_size && x.text_mode == y.text_mode && x.effect == y.effect && x.method == y.method &&
           x.shape == y.shape && x.strength == y.strength && x.opacity == y.opacity &&
           std::equal(points.begin(), points.end(), b.points.begin(), [](auto a, auto c) { return a.x == c.x && a.y == c.y; });
}

bool Annotation::HitTest(D2D1_POINT_2F p, float tolerance) const {
    if (points.empty() || tool == ToolId::Watermark) { return false; }
    const auto r = bounds();
    if (p.x < r.left - tolerance || p.x > r.right + tolerance || p.y < r.top - tolerance || p.y > r.bottom + tolerance) { return false; }
    // Text remains directly editable. Closed geometry only responds at its
    // outline, so another annotation can start inside an existing shape.
    if (tool == ToolId::Text || tool == ToolId::Tag) { return true; }
    if (tool == ToolId::Arrow && points.size() >= 2) {
        const auto a = points.front(), b = points.back();
        const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
        if (length < 1) { return false; }
        const float ux = dx / length, uy = dy / length;
        const float head = std::min(length, std::max(14.0f, style.thickness * 5));
        const D2D1_POINT_2F base{b.x - ux * head, b.y - uy * head};
        const D2D1_POINT_2F left{base.x - uy * head * .31f, base.y + ux * head * .31f};
        const D2D1_POINT_2F right{base.x + uy * head * .31f, base.y - ux * head * .31f};
        return SegmentDistance(p, a, base) <= style.thickness / 2 + tolerance ||
               SegmentDistance(p, b, left) <= tolerance || SegmentDistance(p, left, right) <= tolerance || SegmentDistance(p, right, b) <= tolerance;
    }
    if (tool == ToolId::Line || tool == ToolId::Pen || (tool == ToolId::Mosaic && style.method == EffectMethod::Brush)) {
        float distance = std::hypot(p.x - points[0].x, p.y - points[0].y);
        for (size_t i = 1; i < points.size(); ++i) { distance = std::min(distance, SegmentDistance(p, points[i - 1], points[i])); }
        if (tool == ToolId::Mosaic) { return std::abs(distance - style.brush_size / 2) <= tolerance; }
        return distance <= style.thickness / 2 + tolerance;
    }
    const auto box = NormalizedD2DRect(points.front(), points.back());
    const float margin = tolerance + ((tool == ToolId::Rectangle || tool == ToolId::Ellipse) ? style.thickness / 2 : 0);
    if (tool == ToolId::Ellipse || (tool == ToolId::Spotlight && style.shape == SpotlightShape::Ellipse)) {
        const float rx = (box.right - box.left) / 2, ry = (box.bottom - box.top) / 2;
        const float dx = p.x - (box.left + box.right) / 2, dy = p.y - (box.top + box.bottom) / 2;
        const auto ellipse = [&](float x, float y) { return x > 0 && y > 0 && dx * dx / (x * x) + dy * dy / (y * y) < 1; };
        return ellipse(rx + margin, ry + margin) && !ellipse(rx - margin, ry - margin);
    }
    if (tool == ToolId::Spotlight && style.shape == SpotlightShape::Rounded) {
        const float rx = (box.right - box.left) / 2, ry = (box.bottom - box.top) / 2;
        const float radius = std::min({rx, ry, 16 * style.image_scale});
        const float dx = std::abs(p.x - (box.left + rx)) - (rx - radius), dy = std::abs(p.y - (box.top + ry)) - (ry - radius);
        const float distance = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f)) + std::min(std::max(dx, dy), 0.0f) - radius;
        return std::abs(distance) <= margin;
    }
    const D2D1_POINT_2F tl{box.left, box.top}, tr{box.right, box.top}, br{box.right, box.bottom}, bl{box.left, box.bottom};
    return std::min({SegmentDistance(p, tl, tr), SegmentDistance(p, tr, br), SegmentDistance(p, br, bl), SegmentDistance(p, bl, tl)}) <= margin;
}

std::vector<D2D1_POINT_2F> Annotation::Handles() const {
    if (points.size() < 2) { return {}; }
    if (tool == ToolId::Line || tool == ToolId::Arrow) { return {points.front(), points.back()}; }
    if (tool == ToolId::Rectangle || tool == ToolId::Ellipse || tool == ToolId::Spotlight ||
        (tool == ToolId::Mosaic && style.method == EffectMethod::Region)) {
        const auto r = NormalizedD2DRect(points.front(), points.back());
        return {{r.left, r.top}, {r.right, r.top}, {r.right, r.bottom}, {r.left, r.bottom}};
    }
    return {};
}

void AnnotationDocument::PushUndo(std::vector<Annotation> snapshot) {
    undo_stack_.push_back(std::move(snapshot));
    if (undo_stack_.size() > kUndoLimit) { undo_stack_.pop_front(); }
}
void AnnotationDocument::Checkpoint() {
    if (!transaction_) {
        PushUndo(annotations_);
        redo_stack_.clear();
    }
}
void AnnotationDocument::Apply(Annotation annotation) {
    Checkpoint();
    annotations_.push_back(std::move(annotation));
    ++revision_;
}
void AnnotationDocument::Replace(size_t index, Annotation annotation) {
    if (index >= annotations_.size() || annotations_[index].Same(annotation)) { return; }
    Checkpoint();
    annotations_[index] = std::move(annotation);
    ++revision_;
}
void AnnotationDocument::Remove(size_t index) {
    if (index >= annotations_.size()) { return; }
    Checkpoint();
    annotations_.erase(annotations_.begin() + static_cast<ptrdiff_t>(index));
    ++revision_;
}
void AnnotationDocument::BeginTransaction() {
    if (!transaction_) { transaction_ = annotations_; }
}
void AnnotationDocument::CommitTransaction() {
    if (!transaction_) { return; }
    if (!SameList(*transaction_, annotations_)) {
        PushUndo(std::move(*transaction_));
        redo_stack_.clear();
    }
    transaction_.reset();
}
void AnnotationDocument::CancelTransaction() {
    if (!transaction_) { return; }
    if (!SameList(*transaction_, annotations_)) {
        annotations_ = std::move(*transaction_);
        ++revision_;
    }
    transaction_.reset();
}
bool AnnotationDocument::Undo() {
    CancelTransaction();
    if (undo_stack_.empty()) { return false; }
    redo_stack_.push_back(std::move(annotations_));
    annotations_ = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    ++revision_;
    return true;
}
bool AnnotationDocument::Redo() {
    CancelTransaction();
    if (redo_stack_.empty()) { return false; }
    PushUndo(std::move(annotations_));
    annotations_ = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    ++revision_;
    return true;
}
void AnnotationDocument::Clear() {
    if (annotations_.empty()) { return; }
    Checkpoint();
    annotations_.clear();
    ++revision_;
}
void AnnotationDocument::Reset() {
    annotations_ = {};
    undo_stack_ = {};
    redo_stack_ = {};
    transaction_.reset();
    revision_ = 0;
}
std::optional<size_t> AnnotationDocument::HitTest(D2D1_POINT_2F point, float tolerance) const {
    for (size_t i = annotations_.size(); i > 0; --i)
        if (annotations_[i - 1].HitTest(point, tolerance)) { return i - 1; }
    return {};
}

}  // namespace easnap::editor
