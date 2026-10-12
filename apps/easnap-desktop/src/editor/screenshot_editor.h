// Easnap - screenshot annotation editor.
//
// The native editing island is movable independently of the selected region.
// The workspace owns its inline image planes; this class owns the single
// annotation document, gesture/command handling, redaction and export logic.
// The frame is provisional until first feature use supplies immutable pixels.
// Source/document coordinates then stay in that fixed, full-resolution image.
//
// Annotations are stored as vector commands and are only composited onto pixels
// when the user copies or saves, which keeps undo/redo lossless. The command
// history belongs to a single editing session and is wiped when that session
// ends (see ScreenshotEditor::ResetSession).
//
// A stitched Scrollshot is edited by the same document, tools and export path.
// Only the mapping between the desktop and the image differs: the image is
// seen through a viewport that scrolls and zooms (editor/image_viewport.h),
// where a screenshot is shown one to one at the place it was captured.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/session_state.h"
#include "editor/image_viewport.h"
#include "editor/redaction_renderer.h"
#include "platform/platform_utils.h"

namespace easnap::editor {

using platform::ComPtr;

enum class ToolId {
    Pointer,    ///< Inspect only; does not draw.
    Pen,        ///< Freehand polyline.
    Arrow,      ///< Line with a filled head.
    Rectangle,  ///< Outlined rectangle.
    Ellipse,    ///< Outlined ellipse.
    Text,       ///< Click, then type.
    Mosaic,     ///< Smooth blur or block pixelation, brush or rectangle.
    Line, Tag, Spotlight, Watermark,
};

enum class TextMode { Plain, Fill, Outline };
enum class EffectMode { Blur, Pixelate };
enum class EffectMethod { Brush, Region };
enum class SpotlightShape { Rectangle, Ellipse, Rounded };

struct AnnotationStyle {
    D2D1_COLOR_F color{1.0f, 0.259f, 0.286f, 1.0f};  // Easnap default: signal red
    float thickness{3.0f};
    float font_size{16.0f}; // Points, applied to the whole object.
    float image_scale{1.0f}; // Capture DPI/96, frozen with content.
    TextMode text_mode{TextMode::Plain};
    EffectMode effect{EffectMode::Blur};
    EffectMethod method{EffectMethod::Brush};
    SpotlightShape shape{SpotlightShape::Rectangle};
    float strength{50.0f};
    float opacity{50.0f};
    /// Diameter of the mosaic brush in captured pixels.
    float brush_size{32.0f};
};

/// One editing operation in image-space coordinates (origin = top-left of the
/// captured region, 1 unit = 1 captured pixel).
struct Annotation {
    ToolId tool{ToolId::Pen};
    AnnotationStyle style{};
    std::vector<D2D1_POINT_2F> points;
    std::wstring text;

    [[nodiscard]] D2D1_RECT_F bounds() const;
    [[nodiscard]] bool HitTest(D2D1_POINT_2F point, float tolerance = 5.0f) const;
    [[nodiscard]] std::vector<D2D1_POINT_2F> Handles() const;
    [[nodiscard]] bool Same(const Annotation& other) const;
    [[nodiscard]] float FontPixels() const { return style.font_size * (96.0f/72.0f) * style.image_scale; }
};

/// Vector command list with undo/redo. Compositing happens only on export.
class IAnnotationDocument {
public:
    virtual ~IAnnotationDocument() = default;

    virtual void Apply(Annotation annotation) = 0;
    [[nodiscard]] virtual bool CanUndo() const = 0;
    [[nodiscard]] virtual bool CanRedo() const = 0;
    virtual bool Undo() = 0;
    virtual bool Redo() = 0;
    /// Undoable "remove everything" as one document operation.
    virtual void Clear() = 0;
    /// Non-undoable session reset: drops both stacks and the revision counter.
    virtual void Reset() = 0;
    [[nodiscard]] virtual const std::vector<Annotation>& annotations() const = 0;
    /// Increments on every mutation; used as a cheap cache key.
    [[nodiscard]] virtual uint64_t revision() const = 0;
};

class AnnotationDocument final : public IAnnotationDocument {
public:
    void Apply(Annotation annotation) override;
    [[nodiscard]] bool CanUndo() const override { return !undo_stack_.empty(); }
    [[nodiscard]] bool CanRedo() const override { return !redo_stack_.empty(); }
    bool Undo() override;
    bool Redo() override;
    void Clear() override;
    void Reset() override;
    [[nodiscard]] const std::vector<Annotation>& annotations() const override { return annotations_; }
    [[nodiscard]] uint64_t revision() const override { return revision_; }
    [[nodiscard]] bool HasRedactions() const;
    void Replace(size_t index, Annotation annotation);
    void Remove(size_t index);
    void BeginTransaction();
    void CommitTransaction();
    void CancelTransaction();
    [[nodiscard]] bool in_transaction() const { return transaction_.has_value(); }
    [[nodiscard]] std::optional<size_t> HitTest(D2D1_POINT_2F point, float tolerance = 5.0f) const;

private:
    std::vector<Annotation> annotations_;
    void Checkpoint();
    std::vector<std::vector<Annotation>> undo_stack_, redo_stack_;
    std::optional<std::vector<Annotation>> transaction_;
    uint64_t revision_{0};
};

/// Screenshot document/commands and movable native editing island.
class ScreenshotEditor {
public:
    struct Callbacks {
        /// `exported` is true when the user copied or saved successfully.
        std::function<void(bool exported)> on_closed;
        /// Esc, Discard or a close request would lose annotations or a
        /// stitched image. Return false to keep the editor open.
        std::function<bool()> on_confirm_discard;
        std::function<void(app::AppearanceTheme)> on_appearance;
        std::function<void()> on_feature_used;
        // Acquires the final pixels only when an editor feature/completion needs them.
        std::function<bool()> on_prepare_image;
        std::function<void()> on_document_committed;
        std::function<void(const RECT&)> on_image_invalidated;
        /// The tools island's Scrollshot button, pressed while the frame is
        /// still provisional. It never acquires or fixes the screenshot.
        std::function<void()> on_scrollshot;
        /// The viewport of a document moved, scrolled or zoomed.
        std::function<void(const ImageView&)> on_view_changed;
    };

    explicit ScreenshotEditor(Callbacks callbacks);
    ~ScreenshotEditor();

    ScreenshotEditor(const ScreenshotEditor&) = delete;
    ScreenshotEditor& operator=(const ScreenshotEditor&) = delete;

    /// Shows tools for a provisional frame; no image is acquired at this point.
    bool Open(const RECT& screen_region, app::AppSettings* settings, HWND owner);
    /// Installs the final selected pixels once; subsequent geometry is immutable.
    bool SetImage(platform::BgraImage image);
    [[nodiscard]] bool has_image() const noexcept { return !source_image_.empty(); }
    /// Opens the tools for an image that already exists and may be larger than
    /// the screen. It is shown through a viewport inside `view_area`, fitted to
    /// the width at first, and scrolled and zoomed with Wheel and the keyboard.
    /// `image` is taken only when this succeeds.
    /// `preferred_left` is where the viewport's left edge goes when the area allows.
    bool OpenDocument(platform::BgraImage& image, const RECT& view_area, LONG preferred_left,
                      app::AppSettings* settings, HWND owner);
    [[nodiscard]] bool document_mode() const noexcept { return document_mode_; }
    [[nodiscard]] bool has_annotations() const noexcept { return !document_.annotations().empty(); }
    [[nodiscard]] ImageView view() const noexcept { return view_; }
    /// A wheel turn over the image. Scrolls a document vertically, pans it
    /// with Shift and zooms it around `desktop_point` with Ctrl; a screenshot
    /// shown one to one ignores it.
    void Wheel(int delta, bool control, bool shift, POINT desktop_point);
    /// Gives the unannotated image back, for a caller that must close the
    /// editor without losing it.
    [[nodiscard]] platform::BgraImage TakeImage();
    void Show();
    void Hide();
    /// During a frame gesture, update geometry immediately and defer the island's
    /// duplicate size readout until the gesture ends. The overlay readout stays live.
    void SetRegion(const RECT& region, bool interacting = false);
    [[nodiscard]] RECT region() const noexcept { return region_; }
    [[nodiscard]] bool uses_annotation_tool() const noexcept { return tool_ != ToolId::Pointer; }
    void ImagePointer(UINT message, POINT desktop_point);
    [[nodiscard]] bool WantsPointer(POINT desktop_point) const;
    [[nodiscard]] HCURSOR ImageCursor(POINT desktop_point) const;
    [[nodiscard]] HWND context_hwnd() const noexcept { return context_; }
    void SetFrameInteracting(bool interacting);
    void Keyboard(UINT message, WPARAM key, LPARAM flags);
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const;
    void RenderImage(HDC dc, const RECT& desktop_bounds, const RECT* repaint = nullptr);
    [[nodiscard]] platform::BgraImage Composite();
    /// Composite after finishing a text edit that is still open, without
    /// treating that commit as a reason to complete the session.
    [[nodiscard]] platform::BgraImage CompositeCommitted();
    bool CompleteCopy();
    [[nodiscard]] bool completing() const noexcept { return completing_; }
    [[nodiscard]] uint64_t revision() const noexcept { return document_.revision(); }
    [[nodiscard]] const AnnotationDocument& document() const noexcept { return document_; }
    /// Hides the window, wipes the session history and reports the outcome once.
    void Close(bool exported);
    /// Destroys the window and releases device resources.
    void Destroy();

    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

private:
    struct ToolbarItem;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    /// Lays the toolbar out as a left-to-right flow that wraps into as many rows
    /// as the current window width requires.
    void BuildToolbar(float available_width);
    /// Lays out the island; image coordinates remain physical desktop pixels.
    void LayoutClient(int client_width, int client_height);
    /// Places the island beside/below the region, contained by its monitor.
    [[nodiscard]] RECT InitialWindowRect(const RECT& screen_region) const;
    /// Forgets every trace of the previous editing session.
    void ResetSession();
    /// Resets tools and styles for a new session on `frame`.
    void BeginSession(const RECT& frame, app::AppSettings* settings);
    /// Creates the island, its context island and their controls beside `screen_region`.
    bool CreateWindows(const RECT& screen_region, HWND owner);
    void CreateTextFormats();
    bool FeatureUsed();
    bool EnsureImage();
    void NotifyMutation(uint64_t previous_revision);

    bool CreateToolbarControls();
    void SyncToolbarControls() const;
    void ReflowToolbar();
    void PaintToolbar(HDC dc) const;
    static LRESULT CALLBACK ToolbarButtonProc(HWND hwnd, UINT message, WPARAM wparam,
        LPARAM lparam, UINT_PTR identifier, DWORD_PTR reference);
    void Invalidate() const;
    void InvalidateImage(const RECT& source_bounds) const;
    [[nodiscard]] RECT PendingDamage() const;

    /// Desktop point to image pixels through the current view.
    [[nodiscard]] D2D1_POINT_2F ToImagePoint(POINT desktop_pt) const;
    /// A screenshot is shown one to one where it was captured.
    void ResetFixedView();
    /// Adopts a document view: viewport, scale and scroll position.
    void ApplyView(const ImageView& view, bool notify = true);
    /// Esc, the Discard button and a close request. Asks through
    /// `on_confirm_discard` when annotations or a stitched image would be lost.
    void RequestDiscard();
    void ScrollDocument(float delta_x, float delta_y);
    void ZoomDocument(float zoom, POINT anchor);
    /// Makes the base bitmap hold image rows [first, end) and returns the rows
    /// it actually covers from `first` on, or 0 on failure.
    [[nodiscard]] int EnsureBaseRows(int first, int end);
    [[nodiscard]] const ToolbarItem* HitTestToolbar(POINT client_pt) const;

    void OnToolbarActivated(const ToolbarItem& item);
    void BeginStroke(POINT client_pt);
    void UpdateStroke(POINT client_pt);
    void EndStroke(POINT client_pt);
    void CancelStroke();
    void CommitPendingText();
    void OnCharacter(wchar_t character);
    void Select(std::optional<size_t> index);
    [[nodiscard]] ToolId ContextTool() const;
    [[nodiscard]] AnnotationStyle ContextStyle() const;
    void ChangeStyle(const AnnotationStyle& style);
    void CreateContextControls();
    [[nodiscard]] HWND Control(int identifier) const;
    void SyncContext(bool layout = true);
    void CommitContextTransaction();
    void PlaceContext(bool follow_frame = false);
    void StartTextEditing(std::optional<size_t> index, POINT point);
    void UpdateTextFromControl();
    void LayoutTextEditor();
    static LRESULT CALLBACK TextEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK PropertyProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void DrawSelection(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush) const;
    void PaintTextEditor(HDC dc);
    void ShowBrushPopup(bool show);
    void SyncWatermarkClear();
    void PaintContext(HDC dc, HWND window) const;

    /// Brings the derived blur/pixelation and spotlight pixels up to date with
    /// the effect list. Only the final composite reaches the clipboard or file.
    void RebuildRedactedBase();
    bool CopyToClipboard();
    bool SaveToFile();

    Callbacks callbacks_;
    app::AppSettings* settings_{nullptr};

    HWND hwnd_{nullptr};
    HWND context_{nullptr}, text_host_{nullptr}, text_edit_{nullptr}, brush_popup_{nullptr};
    HFONT text_font_{nullptr};
    float text_font_pixels_{0};
    unsigned text_dispatch_depth_{0};
    bool text_layout_pending_{false};
    ULONGLONG brush_hover_started_{0}, brush_leave_started_{0};
    bool syncing_context_{false}, frame_interacting_{false}, repositioning_{false};
    bool modal_active_{false}, brush_popup_open_{false};
    int context_height_{0}, context_width_{0};
    bool context_visible_{false}, island_user_positioned_{false};
    ULONGLONG context_fade_started_{0};
    std::optional<size_t> selected_;
    std::vector<RECT> main_separators_, context_separators_;
    std::vector<std::pair<int,RECT>> context_fields_;
    Annotation original_;
    D2D1_POINT_2F drag_origin_{};
    int transform_handle_{-1};
    bool transforming_{false};
    DWORD last_text_double_{0};
    POINT last_text_point_{};
    HFONT toolbar_font_{nullptr};
    HWND tooltip_{nullptr};
    platform::BgraImage source_image_;    ///< Untouched capture.
    RECT source_bounds_{};               ///< Immutable snapshot placement.
    RECT region_{};                      ///< Provisional frame, fixed when pixels arrive.
    bool completing_{false};
    ComPtr<ID2D1DCRenderTarget> image_target_;
    ComPtr<ID2D1Bitmap> image_bitmap_;
    ComPtr<ID2D1SolidColorBrush> image_brush_;
    RedactionRenderer redaction_;         ///< Capture with pixel effects applied.

    AnnotationDocument document_;
    std::vector<ToolbarItem> toolbar_;
    Annotation pending_;      ///< Stroke currently being drawn.
    bool has_pending_{false};
    bool dragging_{false};
    bool context_owns_transaction_{false};  ///< A property control opened the open transaction.
    bool editing_text_{false};
    ToolId tool_{ToolId::Pen};
    AnnotationStyle style_{};

    int client_width_{0};      ///< Client area width in physical pixels.
    int client_height_{0};     ///< Client area height in physical pixels.
    RECT toolbar_rect_{};      ///< Toolbar strip, client pixels (docked to top).
    /// Desktop-to-image mapping. A screenshot keeps scale 1 at its captured
    /// place; a document scrolls and zooms inside `view_area_`.
    ImageView view_{};
    D2D1_POINT_2F view_origin_{};  ///< Desktop position of image pixel (0, 0).
    float zoom_{1.0f};
    RECT view_area_{};
    LONG view_preferred_left_{0};
    bool document_mode_{false};
    /// Image rows the base bitmap currently holds. A document taller than one
    /// bitmap may be is uploaded in the band that is being shown.
    int band_top_{0};
    int band_rows_{0};
    float scale_{1.0f};        ///< DPI scale of the hosting monitor.
    uint64_t redaction_revision_{UINT64_MAX};
    bool visible_{false};
    bool closed_reported_{false};
};

}  // namespace easnap::editor
