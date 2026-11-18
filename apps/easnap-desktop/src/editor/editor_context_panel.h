// Easnap - the screenshot editor's property island and brush size pop-up.
//
// Below the tools island, the property island shows what the chosen tool or
// the selected annotation can change: text style, effect and method, spotlight
// shape, strength and opacity sliders, font size, watermark text, thickness and
// colour. Its brush method button opens a small pop-up of brush sizes. This
// class lays both out, creates and refreshes their controls, places the three
// islands as one group, runs their fade and hover timers and turns their input
// into style changes. The document, the selection and their undo transactions
// stay with ScreenshotEditor, reached through ContextPanelOwner.
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "editor/annotation_document.h"
#include "editor/toolbar_item.h"

namespace easnap::editor {

/// What the property island needs from the editor that owns it.
class ContextPanelOwner {
public:
    /// The tools island; the property island is placed below it and the
    /// group is moved together.
    [[nodiscard]] virtual HWND tools_island() const = 0;
    /// DPI scale of the islands' monitor.
    [[nodiscard]] virtual float island_scale() const = 0;
    /// The font every island control uses.
    [[nodiscard]] virtual HFONT island_font() const = 0;
    /// The frame the islands are placed around, in desktop pixels.
    [[nodiscard]] virtual RECT island_frame() const = 0;
    /// The area the islands never cover; empty when there is none.
    [[nodiscard]] virtual RECT IslandKeepClear() const = 0;
    /// The editor is shown, so its islands may be.
    [[nodiscard]] virtual bool islands_shown() const = 0;
    /// A question or a file dialog is open over the islands.
    [[nodiscard]] virtual bool modal_active() const = 0;
    /// The frame is being dragged: the islands are hidden meanwhile.
    [[nodiscard]] virtual bool frame_interacting() const = 0;

    /// The tool whose properties the island shows: the selection's, or else the chosen one.
    [[nodiscard]] virtual ToolId ContextTool() const = 0;
    /// The style the island shows and edits.
    [[nodiscard]] virtual AnnotationStyle ContextStyle() const = 0;
    /// The document's annotations, for the watermark's text.
    [[nodiscard]] virtual const std::vector<Annotation>& island_annotations() const = 0;
    /// Applies `style` to the edited annotation or to the next one drawn.
    virtual void ChangeStyle(const AnnotationStyle& style) = 0;
    /// Gives the image's watermark `text`, creating the watermark when there is none.
    virtual void SetWatermarkText(std::wstring text) = 0;
    /// Empties the watermark `field` as one undo step that also keeps what
    /// was typed into it before.
    virtual void ClearWatermarkField(HWND field) = 0;

    /// Opens a transaction the island owns, unless one is open already: a
    /// text edit or a transform keeps its own undo boundary.
    virtual void OpenContextTransaction() = 0;
    /// Commits the open transaction when the island opened it.
    virtual void CommitContextTransaction() = 0;
    /// Cancels the open transaction and drops the island's claim to it.
    virtual void CancelContextTransaction() = 0;
    /// The island opened the transaction that is open.
    [[nodiscard]] virtual bool context_owns_transaction() const = 0;

    /// Fixes the screenshot on the first use of a feature; false when it could not.
    virtual bool FeatureUsed() = 0;
    /// Esc on an island control: asks before discarding the session.
    virtual void RequestDiscard() = 0;
    /// Repaints the image and refreshes the islands' buttons.
    virtual void Invalidate() const = 0;
    /// The editor's handler of the islands' window messages.
    virtual LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) = 0;

protected:
    ~ContextPanelOwner() = default;
};

/// The property island and the brush size pop-up: their windows, items,
/// controls, placement, timers and input.
class EditorContextPanel {
public:
    explicit EditorContextPanel(ContextPanelOwner& owner) noexcept : owner_(owner) {}
    EditorContextPanel(const EditorContextPanel&) = delete;
    EditorContextPanel& operator=(const EditorContextPanel&) = delete;

    /// Takes the property island's window, created by the editor with its window class.
    void AdoptIsland(HWND island) noexcept { context_ = island; }
    /// Takes the brush size pop-up's window, created by the editor with its window class.
    void AdoptBrushPopup(HWND popup) noexcept { brush_popup_ = popup; }
    /// Destroys the brush size pop-up and its buttons.
    void DestroyBrushPopup();
    /// Destroys the property island and its controls.
    void DestroyIsland();
    /// The property island's window; null while there is none.
    [[nodiscard]] HWND island() const noexcept { return context_; }
    /// The brush size pop-up's window; null while there is none.
    [[nodiscard]] HWND brush_popup() const noexcept { return brush_popup_; }
    /// The island's or the pop-up's control `identifier`; null when there is none.
    [[nodiscard]] HWND Control(int identifier) const;

    /// Lays out the property island of the context tool in rows that fit
    /// `available_width` pixels, and measures the tallest one any tool shows.
    void Build(float available_width);
    /// The room the tools island takes with, below it, the tallest property
    /// island any tool shows (ui/island_layout.h).
    [[nodiscard]] SIZE GroupSize() const noexcept;
    /// The option, thickness, colour or brush size item `identifier`, as a copy.
    [[nodiscard]] std::optional<ToolbarItem> Find(int identifier) const;

    /// Creates the island's and the pop-up's item buttons, registered with
    /// the shared `tooltip`. False when a button could not be made.
    bool CreateButtons(HWND tooltip, ToolbarButtonOwner& owner);
    /// Creates the font size box, the sliders, their value labels and the
    /// watermark field with its clear button.
    void CreateControls();
    /// Places the item buttons and checks the ones `style` chooses.
    void SyncButtons(const AnnotationStyle& style, HFONT font) const;
    /// Shows the controls of the context tool with the context style; with
    /// `layout`, places them as Build laid them out.
    void Sync(bool layout = true);
    /// Shows the watermark clear button while the pointer is over a filled field.
    void SyncWatermarkClear();
    /// Places the islands as one group: around the frame with `follow_frame`
    /// or when they would cover the keep-clear area, else where they are.
    void Place(bool follow_frame = false);
    /// Opens the brush size pop-up below the property island, or closes it.
    void ShowBrushPopup(bool show);
    /// The pointer is over the brush method button: opens the pop-up once it rests.
    void OnBrushMethodHover();
    /// Stops the fade and hover timers and closes the font size list while a
    /// file dialog covers the islands.
    void StopForDialog();
    /// Forgets the pop-up of the previous session.
    void ResetSession() noexcept { brush_popup_open_ = false; }
    /// The island is writing the context style into its controls: the
    /// notifications that causes are not edits.
    [[nodiscard]] bool syncing() const noexcept { return syncing_context_; }

    /// The island's messages that reach the editor's window procedure: its
    /// timers, slider scrolls and font size list items. Empty when not handled.
    [[nodiscard]] std::optional<LRESULT> HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    /// Handles a WM_COMMAND of the font size box or the watermark field and
    /// its clear button. False when it is not the island's.
    bool HandleCommand(int identifier, int notification);
    /// Paints the island's or the pop-up's panel and separators.
    void Paint(HDC dc, HWND window) const;

private:
    static LRESULT CALLBACK PropertyProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    /// Moves, shows and hides the property controls as Build laid them out.
    void PositionControls(float scale, HFONT font);
    /// Keeps the clear button at the right end of the watermark field.
    void PlaceWatermarkClear(float scale);
    void OnBrushHoverTimer();
    void OnFadeTimer();
    void OnSliderScroll(WPARAM wparam, LPARAM lparam);
    void OnFontSizeEntered(int notification);
    void OnWatermarkClear();
    void OnWatermarkEdited();

    ContextPanelOwner& owner_;
    HWND context_{nullptr}, brush_popup_{nullptr};
    /// Options, thicknesses and colours of the island, then the pop-up's
    /// brush sizes, in tab order.
    std::vector<ToolbarItem> items_;
    std::vector<RECT> context_separators_;
    std::vector<std::pair<int, RECT>> context_fields_;
    ULONGLONG brush_hover_started_{0}, brush_leave_started_{0};
    bool syncing_context_{false}, repositioning_{false}, brush_popup_open_{false};
    int context_height_{0}, context_width_{0};
    /// Height of the tallest property island any tool shows; the islands keep
    /// room for it around the frame.
    int reserved_context_height_{0};
    bool context_visible_{false};
    ULONGLONG context_fade_started_{0};
};

}  // namespace easnap::editor
