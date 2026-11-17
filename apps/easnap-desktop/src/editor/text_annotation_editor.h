// Easnap - the native text field of the screenshot editor's text annotations.
//
// A Text or Tag annotation is typed into a native EDIT inside a small host
// window laid over the annotation. The EDIT keeps native selection, keyboard,
// clipboard, undo and caret ownership; its own drawing is kept off the screen
// and the host shows the annotation as it will be exported, drawn by the
// editor's preview renderer (ADR-022 and its 2026-11-17 amendment). This class
// creates and lays out the field, mirrors its text into the document, paints
// it, places its caret against the rendered text, and keeps the pointer hidden
// while the user types. The document, the selection, the transaction a text
// edit commits or cancels and the host's window class stay with
// ScreenshotEditor, reached through TextEditorOwner.
#pragma once

#include <windows.h>

#include <cstddef>
#include <optional>

#include "editor/annotation_document.h"
#include "ui/native_controls.h"

namespace easnap::editor {

/// How the image is shown, as the text field needs it to follow the annotation.
struct TextEditorView {
    D2D1_POINT_2F origin{};  ///< Desktop position of image pixel (0, 0).
    float zoom{1.0f};        ///< Desktop pixels per image pixel.
    float scale{1.0f};       ///< DPI scale of the hosting monitor.
    RECT canvas{};           ///< Desktop pixels the field may paint on: the shown image.
    RECT frame{};            ///< The capture frame, where notices appear.
};

/// What the text field needs from the editor that owns it.
class TextEditorOwner {
public:
    /// The document that holds the edited annotation.
    [[nodiscard]] virtual AnnotationDocument& text_document() = 0;
    /// The selected annotation; while text is edited, the edited one.
    [[nodiscard]] virtual std::optional<size_t> text_selection() const = 0;
    /// Sets the selection without reflowing or repainting the islands.
    virtual void SetTextSelection(std::optional<size_t> index) = 0;
    /// The chosen tool; a new text annotation is a Tag only when it is Tag.
    [[nodiscard]] virtual ToolId current_tool() const = 0;
    /// The style a new annotation is created with.
    [[nodiscard]] virtual AnnotationStyle new_annotation_style() const = 0;
    /// The current mapping between the desktop and the image.
    [[nodiscard]] virtual TextEditorView text_view() const = 0;
    /// Desktop point to image pixels through the current view.
    [[nodiscard]] virtual D2D1_POINT_2F ToImagePoint(POINT desktop_point) const = 0;
    /// A copy or save is running; typed characters are dropped.
    [[nodiscard]] virtual bool export_running() const = 0;
    /// The tools island, which receives the typing pointer's timer.
    [[nodiscard]] virtual HWND editor_window() const = 0;
    /// Creates the hidden host window of the field at `desktop_point`, with
    /// the editor's window class; null when it could not be made.
    virtual HWND CreateTextHost(POINT desktop_point) = 0;
    /// Opens the transaction a text edit commits or cancels; the property
    /// island no longer claims the open transaction.
    virtual void BeginTextTransaction() = 0;
    /// Drops the stroke being drawn from the preview.
    virtual void ForgetPendingStroke() = 0;
    /// Starts moving the edited annotation from `desktop_point`, a press on
    /// the field's border, as a drag of the annotation itself.
    virtual void BeginMoveFromText(POINT desktop_point) = 0;
    /// A text edit was committed and its field closed.
    virtual void TextCommitted() = 0;
    /// Draws the image under `desktop_bounds` into `dc`, as the preview shows it.
    virtual void RenderTextBackground(HDC dc, const RECT& desktop_bounds) = 0;
    /// Lays the islands out again for the current selection.
    virtual void ReflowToolbar() = 0;
    /// Repaints the islands and the image.
    virtual void Invalidate() const = 0;

protected:
    ~TextEditorOwner() = default;
};

/// The native text field of a Text or Tag annotation while it is typed.
class TextAnnotationEditor {
public:
    explicit TextAnnotationEditor(TextEditorOwner& owner) noexcept : owner_(owner) {}
    TextAnnotationEditor(const TextAnnotationEditor&) = delete;
    TextAnnotationEditor& operator=(const TextAnnotationEditor&) = delete;

    /// Text is being typed into an annotation.
    [[nodiscard]] bool editing() const noexcept { return editing_; }
    /// The field's host window; null while there is none.
    [[nodiscard]] HWND host() const noexcept { return host_; }

    /// Edits the text annotation `index`, or a new one created at
    /// `desktop_point` when there is none; an edit already open is committed
    /// first. The caret goes to the character under `desktop_point`.
    void Start(std::optional<size_t> index, POINT desktop_point);
    /// Moves and sizes the field over the edited annotation as it is now shown.
    void Layout();
    /// Commits the text being edited and closes the field. An annotation left
    /// empty is removed. Shows the pointer again in every case.
    void Commit();

    /// Forwards a press and release at `desktop_point` to the field.
    void ForwardClick(POINT desktop_point);
    /// Forwards a double-click at `desktop_point` to the field.
    void ForwardDoubleClick(POINT desktop_point);
    /// Forwards a key pressed on the islands to the field. False when no
    /// text is being edited.
    bool ForwardKeyDown(WPARAM key, LPARAM flags);
    /// A character typed on the islands while text is edited and nothing is exported.
    void OnCharacter(wchar_t character);

    /// The field's EN_CHANGE. False for any other command.
    bool HandleCommand(int id, int notification);
    /// The typing pointer's timer and the field's control colour; nothing
    /// for any other message.
    [[nodiscard]] std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    /// Shows the pointer again if typing hid it.
    void RestorePointer() { typing_pointer_.Restore(); }
    /// Hides the host with the editor.
    void HideHost() const;
    /// Destroys the host and its field; while they are destroyed, they are
    /// still the field's.
    void DestroyHost();
    /// Ends the session: destroys the host and forgets the edit.
    void Reset();
    /// Releases the field's font.
    void ReleaseFont();

private:
    static LRESULT CALLBACK TextEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    /// Reads the field's text into the edited annotation.
    void UpdateFromControl();
    /// Notes what a field message means for the pointer and the redraw guard.
    void ObserveFieldMessage(HWND field, UINT message, WPARAM wparam);
    /// A pointer on the field's border: the move cursor, or the start of a
    /// move of the annotation. Nothing when the pointer is inside.
    std::optional<LRESULT> OnBorderPointer(HWND field, UINT message, LPARAM lparam);
    /// WM_PAINT and WM_PRINTCLIENT: the annotation, buffered, under a hidden caret.
    LRESULT OnFieldPaint(HWND field, UINT message, WPARAM wparam);
    /// Esc cancels the edit, Ctrl+Enter commits it, Ctrl+A selects all.
    std::optional<LRESULT> OnFieldShortcut(HWND field, WPARAM key);
    /// Esc: the edit and its transaction are dropped and the field closes.
    void Cancel();
    /// Forgets the host and its field, then destroys them.
    void CloseField();
    /// A press that completes a triple-click selects its paragraph.
    std::optional<LRESULT> OnTripleClick(HWND field, LPARAM lparam);
    /// Lets EDIT handle `message`, with its redraw suspended and its caret
    /// hidden where that changes pixels, then lays the field out and places
    /// the caret against the rendered text.
    LRESULT DispatchToField(HWND field, UINT message, WPARAM wparam, LPARAM lparam);
    /// Places a collapsed caret against the rendered DirectWrite layout.
    void PlaceCaret(HWND field) const;
    /// The selected text inverted over the annotation the host shows.
    void Paint(HDC dc);

    TextEditorOwner& owner_;
    HWND host_{nullptr};
    HWND edit_{nullptr};
    HFONT font_{nullptr};
    float font_pixels_{0};
    /// Nesting of EDIT dispatches: layout waits until the outermost one ends.
    unsigned dispatch_depth_{0};
    bool layout_pending_{false};
    bool editing_{false};
    DWORD last_double_{0};  ///< When the last double-click in the field was.
    POINT last_point_{};    ///< Where it was, in field pixels.
    /// Hidden while text is typed into the annotation, until it moves.
    ui::TypingPointer typing_pointer_;
};

}  // namespace easnap::editor
