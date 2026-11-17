// Easnap - the annotation document of the screenshot editor.
//
// An annotation is one vector command in image space; the document holds the
// list with its undo and redo history. Compositing happens only on export.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>

#include "editor/annotation_palette.h"
#include "platform/platform_utils.h"

namespace easnap::editor {

using platform::ComPtr;

/// The editor's tools. The order is a test contract: tool button identifiers are
/// kToolButtons.Id(int(tool)).
enum class ToolId {
    Pointer,    ///< Inspect only; does not draw.
    Pen,        ///< Freehand polyline.
    Arrow,      ///< Line with a filled head.
    Rectangle,  ///< Outlined rectangle.
    Ellipse,    ///< Outlined ellipse.
    Text,       ///< Click, then type.
    Mosaic,     ///< Smooth blur or block pixelation, brush or rectangle.
    Line,       ///< Straight line between two points.
    Tag,        ///< Marker dot with a text label beside it.
    Spotlight,  ///< Dims the image outside a rectangle, ellipse or rounded rectangle.
    Watermark,  ///< Text repeated diagonally over the whole image.
};

/// How a text annotation is drawn: plain, on a filled box, or outlined.
enum class TextMode { Plain,
                      Fill,
                      Outline };
/// The kind of mosaic: smooth blur or block pixelation.
enum class EffectMode { Blur,
                        Pixelate };
/// How a mosaic is applied: along a brush stroke or over a dragged region.
enum class EffectMethod { Brush,
                          Region };
/// The outline of the area a spotlight leaves undimmed.
enum class SpotlightShape { Rectangle,
                            Ellipse,
                            Rounded };

/// The properties an annotation is drawn with; the editor keeps one for new annotations.
struct AnnotationStyle {
    D2D1_COLOR_F color{kDefaultAnnotationColor};  ///< Stroke, text or marker colour.
    float thickness{3.0f};  ///< Stroke width in captured pixels.
    float font_size{16.0f}; // Points, applied to the whole object.
    float image_scale{1.0f}; // Capture DPI/96, frozen with content.
    TextMode text_mode{TextMode::Plain};  ///< How text is drawn.
    EffectMode effect{EffectMode::Blur};  ///< Blur or pixelate, for a mosaic.
    EffectMethod method{EffectMethod::Brush};  ///< Brush stroke or region, for a mosaic.
    SpotlightShape shape{SpotlightShape::Rectangle};  ///< The undimmed area's outline, for a spotlight.
    float strength{50.0f};  ///< Mosaic strength, 0-100: blur radius or block size.
    float opacity{50.0f};  ///< Percent: the watermark's ink, or how much a spotlight dims.
    /// Diameter of the mosaic brush in captured pixels.
    float brush_size{32.0f};
};

/// One editing operation in image-space coordinates (origin = top-left of the
/// captured region, 1 unit = 1 captured pixel).
struct Annotation {
    ToolId tool{ToolId::Pen};  ///< The tool that made it.
    AnnotationStyle style{};  ///< The properties it is drawn with.
    /// The stroke, the two corners or ends of a shape, or the text origin.
    std::vector<D2D1_POINT_2F> points;
    std::wstring text;  ///< Text, tag and watermark only.

    /// The image pixels it covers, including its stroke or text box.
    [[nodiscard]] D2D1_RECT_F bounds() const;
    /// True when `point` grabs it: text anywhere in its box, shapes at their outline.
    [[nodiscard]] bool HitTest(D2D1_POINT_2F point, float tolerance = 5.0f) const;
    /// The resize handles: two ends or four corners, or none.
    [[nodiscard]] std::vector<D2D1_POINT_2F> Handles() const;
    /// True when both have the same tool, style, points and text.
    [[nodiscard]] bool Same(const Annotation& other) const;
    /// The font size in image pixels.
    [[nodiscard]] float FontPixels() const { return style.font_size * (96.0f / 72.0f) * style.image_scale; }
};

/// Vector command list with undo/redo. Compositing happens only on export.
class IAnnotationDocument {
public:
    virtual ~IAnnotationDocument() = default;

    /// Adds an annotation on top as one undo step.
    virtual void Apply(Annotation annotation) = 0;
    /// True when there is a step to undo.
    [[nodiscard]] virtual bool CanUndo() const = 0;
    /// True when there is an undone step to redo.
    [[nodiscard]] virtual bool CanRedo() const = 0;
    /// Goes back one step; false when there is none.
    virtual bool Undo() = 0;
    /// Repeats the last undone step; false when there is none.
    virtual bool Redo() = 0;
    /// Undoable "remove everything" as one document operation.
    virtual void Clear() = 0;
    /// Non-undoable session reset: drops both stacks and the revision counter.
    virtual void Reset() = 0;
    /// The annotations, bottom first.
    [[nodiscard]] virtual const std::vector<Annotation>& annotations() const = 0;
    /// Increments on every mutation; used as a cheap cache key.
    [[nodiscard]] virtual uint64_t revision() const = 0;
};

/// The editor's document: the annotation list, its bounded undo history and
/// the one open transaction that groups a gesture into one undo step.
class AnnotationDocument final : public IAnnotationDocument {
public:
    /// Undo keeps this many steps; older ones are dropped. Each step holds a
    /// copy of the whole annotation list, so an unbounded history grew with
    /// every nudge of an arrow key.
    static constexpr size_t kUndoLimit = 200;

    void Apply(Annotation annotation) override;
    [[nodiscard]] bool CanUndo() const override { return !undo_stack_.empty(); }
    [[nodiscard]] bool CanRedo() const override { return !redo_stack_.empty(); }
    bool Undo() override;
    bool Redo() override;
    void Clear() override;
    void Reset() override;
    [[nodiscard]] const std::vector<Annotation>& annotations() const override { return annotations_; }
    [[nodiscard]] uint64_t revision() const override { return revision_; }
    /// Replaces the annotation at `index`; nothing happens when it is the same.
    void Replace(size_t index, Annotation annotation);
    /// Removes the annotation at `index`.
    void Remove(size_t index);
    /// Opens a transaction: the changes until it ends are one undo step.
    void BeginTransaction();
    /// Ends the open transaction, keeping its changes as one undo step.
    void CommitTransaction();
    /// Ends the open transaction and takes its changes back.
    void CancelTransaction();
    /// True while a transaction is open.
    [[nodiscard]] bool in_transaction() const { return transaction_.has_value(); }
    /// The topmost annotation `point` grabs.
    [[nodiscard]] std::optional<size_t> HitTest(D2D1_POINT_2F point, float tolerance = 5.0f) const;

private:
    std::vector<Annotation> annotations_;
    void Checkpoint();
    void PushUndo(std::vector<Annotation> snapshot);
    std::deque<std::vector<Annotation>> undo_stack_;
    std::vector<std::vector<Annotation>> redo_stack_;
    std::optional<std::vector<Annotation>> transaction_;
    uint64_t revision_{0};
};

/// The rectangle spanned by two corner points, whatever their order.
[[nodiscard]] D2D1_RECT_F NormalizedD2DRect(D2D1_POINT_2F a, D2D1_POINT_2F b);
/// The DirectWrite layout of a text, tag or watermark annotation's text at its
/// font size, from a per-thread cache of recent layouts. Empty on failure.
[[nodiscard]] ComPtr<IDWriteTextLayout> TextLayout(const Annotation& a);

}  // namespace easnap::editor
