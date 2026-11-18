// Easnap - the native text field of the screenshot editor's text annotations.
#include "editor/text_annotation_editor.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "editor/annotation_tools.h"
#include "platform/paint_buffer.h"
#include "platform/screen_geometry.h"
#include "strings/strings.h"
#include "ui/capture_workspace.h"
#include "ui/control_ids.h"
#include "ui/ui_theme.h"

namespace easnap::editor {

void TextAnnotationEditor::Start(std::optional<size_t> index, POINT point) {
    auto& document = owner_.text_document();
    const auto selected = owner_.text_selection();
    if (editing_ && index && index == selected) {
        ::SetFocus(edit_);
        return;
    }
    // Committing an empty active box erases it, shifting later hit indices.
    if (editing_ && index && selected && *selected < *index &&
        document.annotations()[*selected].text.empty()) { --*index; }
    Commit();
    owner_.BeginTextTransaction();
    if (!index) {
        Annotation a;
        a.tool = owner_.current_tool() == ToolId::Tag ? ToolId::Tag : ToolId::Text;
        a.style = owner_.new_annotation_style();
        a.points = {owner_.ToImagePoint(point)};
        document.Apply(std::move(a));
        index = document.annotations().size() - 1;
    }
    owner_.SetTextSelection(index);
    // The field opens with the annotation's text as it is now.
    const std::wstring text = document.annotations()[*index].text;
    editing_ = true;
    owner_.ForgetPendingStroke();
    host_ = owner_.CreateTextHost(point);
    if (host_) {
        edit_ = ::CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                  0, 0, 100, 50, host_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInlineTextEdit)), ::GetModuleHandleW(nullptr), nullptr);
    }
    if (!edit_) {
        if (host_) {
            ::DestroyWindow(host_);
            host_ = nullptr;
        }
        document.CancelTransaction();
        editing_ = false;
        owner_.SetTextSelection({});
        ui::ShowCaptureStatusNotice(owner_.text_view().frame, strings::Get(IDS_EDITOR_NOTICE_TEXT_EDITOR_FAILED));
        owner_.Invalidate();
        return;
    }
    ::SendMessageW(edit_, EM_SETLIMITTEXT, 65535, 0);
    last_double_ = 0;
    ui::SetAccessibleName(edit_, strings::Get(IDS_EDITOR_TEXT_NAME));
    ::SetWindowSubclass(edit_, TextEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    static_cast<void>(ui::ExcludeFromCapture(host_));
    Layout();
    ::ShowWindow(host_, SW_SHOWNORMAL);
    ::SetFocus(edit_);
    POINT local = point;
    ::ScreenToClient(edit_, &local);
    const auto caret = ::SendMessageW(edit_, EM_CHARFROMPOS, 0, MAKELPARAM(std::max(0L, local.x), std::max(0L, local.y)));
    ::SendMessageW(edit_, EM_SETSEL, LOWORD(caret), LOWORD(caret));
    owner_.ReflowToolbar();
    owner_.Invalidate();
}

void TextAnnotationEditor::Layout() {
    const auto selected = owner_.text_selection();
    if (!host_ || !edit_ || !selected) { return; }
    const Annotation edited = owner_.text_document().annotations()[*selected];
    const TextEditorView view = owner_.text_view();
    const auto box = edited.bounds();
    const float em = edited.FontPixels();
    // The native input follows the scale the text is shown at.
    const float shown_em = em * view.zoom;
    if (!font_ || font_pixels_ != shown_em) {
        HFONT next = ui::theme::CreateUiFont(shown_em, 1, false);
        if (next) {
            ::SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(next), FALSE);
            if (font_) { ::DeleteObject(font_); }
            font_ = next;
            font_pixels_ = shown_em;
        }
    }
    if (reinterpret_cast<HFONT>(::SendMessageW(edit_, WM_GETFONT, 0, 0)) != font_) {
        ::SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    }
    const LONG pad = ui::theme::Scaled(5, view.scale);
    const LONG x = static_cast<LONG>(view.origin.x) + static_cast<LONG>(std::floor(box.left * view.zoom)) - pad;
    const LONG y = static_cast<LONG>(view.origin.y) + static_cast<LONG>(std::floor(box.top * view.zoom)) - pad;
    const LONG w = std::max(2 * pad + 8L, static_cast<LONG>(std::ceil((box.right - box.left) * view.zoom)) + 2 * pad);
    const LONG h = std::max(2 * pad + 8L, static_cast<LONG>(std::ceil((box.bottom - box.top) * view.zoom)) + 2 * pad);
    // Only what changed is changed: Windows takes every move, resize, z-order
    // or region change of a window for pointer movement, and shows the
    // pointer it hides while the user types. This runs on every key.
    RECT placed{};
    ::GetWindowRect(host_, &placed);
    const bool shown = ::IsWindowVisible(host_) != FALSE;
    if (!shown || placed.left != x || placed.top != y || platform::RectWidth(placed) != w || platform::RectHeight(placed) != h) {
        // Topmost when it appears; afterwards it keeps its place.
        ::SetWindowPos(host_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | (shown ? SWP_NOZORDER : 0));
    }
    RECT input{};
    ::GetClientRect(edit_, &input);
    if (input.right != w || input.bottom != h) { ::MoveWindow(edit_, 0, 0, w, h, FALSE); }
    // The input keeps its full layout/caret coordinates, but nothing (including
    // its selection frame) may paint beyond the capture canvas.
    RECT visible = platform::IntersectRects({x, y, x + w, y + h}, view.canvas);
    ::OffsetRect(&visible, -x, -y);
    HRGN current = ::CreateRectRgn(0, 0, 0, 0);
    const bool has_region = current && ::GetWindowRgn(host_, current) != ERROR;
    if (visible.left == 0 && visible.top == 0 && visible.right == w && visible.bottom == h) {
        // Wholly on the canvas: no region at all.
        if (has_region) { ::SetWindowRgn(host_, nullptr, FALSE); }
    } else if (HRGN clip = ::CreateRectRgn(visible.left, visible.top, visible.right, visible.bottom)) {
        if ((has_region && ::EqualRgn(current, clip)) || !::SetWindowRgn(host_, clip, FALSE)) { ::DeleteObject(clip); }
    }
    if (current) { ::DeleteObject(current); }
    const float inset = TextInset(edited);
    RECT format{pad + static_cast<LONG>((inset + TagMarkerWidth(edited)) * view.zoom), pad + static_cast<LONG>(inset * view.zoom), 32760, 32760};
    ::SendMessageW(edit_, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&format));
    ::InvalidateRect(edit_, nullptr, FALSE);
}

void TextAnnotationEditor::UpdateFromControl() {
    const auto selected = owner_.text_selection();
    if (!editing_ || !selected || !edit_) { return; }
    const int length = ::GetWindowTextLengthW(edit_);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(edit_, value.data(), length + 1);
    value.resize(length);
    auto& document = owner_.text_document();
    auto a = document.annotations()[*selected];
    a.text = std::move(value);
    document.Replace(*selected, std::move(a));
    if (dispatch_depth_) {
        layout_pending_ = true;
    } else {
        Layout();
    }
    owner_.Invalidate();
}

void TextAnnotationEditor::Commit() {
    typing_pointer_.Restore();
    if (!editing_) { return; }
    UpdateFromControl();
    editing_ = false;
    auto& document = owner_.text_document();
    if (const auto selected = owner_.text_selection(); selected && document.annotations()[*selected].text.empty()) {
        document.Remove(*selected);
        owner_.SetTextSelection({});
    }
    document.CommitTransaction();
    CloseField();
    owner_.TextCommitted();
}

void TextAnnotationEditor::Cancel() {
    editing_ = false;
    owner_.text_document().CancelTransaction();
    CloseField();
    owner_.SetTextSelection({});
    owner_.ReflowToolbar();
    owner_.Invalidate();
}

void TextAnnotationEditor::ForwardClick(POINT point) {
    if (!edit_) { return; }
    POINT local = point;
    ::ScreenToClient(edit_, &local);
    ::SendMessageW(edit_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(local.x, local.y));
    ::SendMessageW(edit_, WM_LBUTTONUP, 0, MAKELPARAM(local.x, local.y));
}

void TextAnnotationEditor::ForwardDoubleClick(POINT point) {
    if (!edit_) { return; }
    POINT local = point;
    ::ScreenToClient(edit_, &local);
    ::SendMessageW(edit_, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(local.x, local.y));
}

bool TextAnnotationEditor::ForwardKeyDown(WPARAM key, LPARAM flags) {
    if (!editing_ || !edit_) { return false; }
    ::SendMessageW(edit_, WM_KEYDOWN, key, flags);
    return true;
}

void TextAnnotationEditor::OnCharacter(wchar_t character) {
    if (!owner_.export_running() && editing_ && edit_) { ::SendMessageW(edit_, WM_CHAR, character, 0); }
}

bool TextAnnotationEditor::HandleCommand(int id, int notification) {
    if (id != kInlineTextEdit || notification != EN_CHANGE) { return false; }
    UpdateFromControl();
    return true;
}

std::optional<LRESULT> TextAnnotationEditor::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER && wparam == ui::kTypingPointerTimer) {
        typing_pointer_.Poll();
        return 0;
    }
    if ((message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORSTATIC) && edit_ &&
        reinterpret_cast<HWND>(lparam) == edit_) {
        // EDIT can paint synchronously during input while WS_VISIBLE is
        // retained for its native caret. Its GDI glyphs/backing are never
        // the annotation presentation: only Paint owns those.
        // This DC belongs to the native draw and is reset on release; the
        // custom paint pass obtains its own unrestricted client DC.
        ::IntersectClipRect(reinterpret_cast<HDC>(wparam), 0, 0, 0, 0);
        return reinterpret_cast<LRESULT>(::GetStockObject(NULL_BRUSH));
    }
    return std::nullopt;
}

void TextAnnotationEditor::HideHost() const {
    if (host_) { ::ShowWindow(host_, SW_HIDE); }
}

void TextAnnotationEditor::DestroyHost() {
    if (host_) {
        ::DestroyWindow(host_);
        host_ = edit_ = nullptr;
    }
}

void TextAnnotationEditor::CloseField() {
    if (host_) {
        HWND host = host_;
        host_ = edit_ = nullptr;
        ::DestroyWindow(host);
    }
}

void TextAnnotationEditor::Reset() {
    DestroyHost();
    editing_ = false;
}

void TextAnnotationEditor::ReleaseFont() {
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
}

LRESULT CALLBACK TextAnnotationEditor::TextEditProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR reference) {
    auto* self = reinterpret_cast<TextAnnotationEditor*>(reference);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, TextEditProc, id);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    self->ObserveFieldMessage(hwnd, message, wparam);
    if (message == WM_SETCURSOR || message == WM_LBUTTONDOWN) {
        if (const auto handled = self->OnBorderPointer(hwnd, message, lparam)) { return *handled; }
    }
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_PAINT || message == WM_PRINTCLIENT) { return self->OnFieldPaint(hwnd, message, wparam); }
    if (message == WM_KEYDOWN) {
        if (const auto handled = self->OnFieldShortcut(hwnd, wparam)) { return *handled; }
    }
    if (message == WM_LBUTTONDBLCLK) {
        self->last_double_ = ::GetTickCount();
        self->last_point_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    }
    if (message == WM_LBUTTONDOWN) {
        if (const auto handled = self->OnTripleClick(hwnd, lparam)) { return *handled; }
    }
    return self->DispatchToField(hwnd, message, wparam, lparam);
}

void TextAnnotationEditor::ObserveFieldMessage(HWND field, UINT message, WPARAM wparam) {
    if (message == WM_SHOWWINDOW) { ui::NativeRedrawGuard::VisibilityChanged(field, wparam != 0); }
    // The box grows with its text, and Windows shows the pointer it hides
    // for typing at each such change: the editor keeps it hidden instead.
    if (message == WM_CHAR) { typing_pointer_.Typed(owner_.editor_window()); }
    if (message == WM_KILLFOCUS) { typing_pointer_.Restore(); }
}

std::optional<LRESULT> TextAnnotationEditor::OnBorderPointer(HWND field, UINT message, LPARAM lparam) {
    POINT point{};
    ::GetCursorPos(&point);
    ::ScreenToClient(field, &point);
    if (message == WM_LBUTTONDOWN) { point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; }
    RECT client{};
    ::GetClientRect(field, &client);
    const LONG edge = ui::theme::Scaled(4, owner_.text_view().scale);
    const bool border = point.x < edge || point.y < edge || point.x >= client.right - edge || point.y >= client.bottom - edge;
    if (!border) { return std::nullopt; }
    if (message == WM_SETCURSOR) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
        return TRUE;
    }
    ::ClientToScreen(field, &point);
    Commit();
    owner_.BeginMoveFromText(point);
    return 0;
}

LRESULT TextAnnotationEditor::OnFieldPaint(HWND field, UINT message, WPARAM wparam) {
    PAINTSTRUCT ps{};
    HDC dc = message == WM_PAINT ? ::BeginPaint(field, &ps) : reinterpret_cast<HDC>(wparam);
    RECT client{};
    ::GetClientRect(field, &client);
    ::HideCaret(field);
    {
        platform::PaintBuffer buffer(dc, client);
        Paint(buffer.dc());
    }
    ::ShowCaret(field);
    if (message == WM_PAINT) { ::EndPaint(field, &ps); }
    return 0;
}

std::optional<LRESULT> TextAnnotationEditor::OnFieldShortcut(HWND field, WPARAM key) {
    if (key == VK_ESCAPE) {
        Cancel();
        return 0;
    }
    if (key == VK_RETURN && (::GetKeyState(VK_CONTROL) & 0x8000)) {
        Commit();
        return 0;
    }
    if (key == 'A' && (::GetKeyState(VK_CONTROL) & 0x8000)) {
        ::SendMessageW(field, EM_SETSEL, 0, -1);
        return 0;
    }
    return std::nullopt;
}

std::optional<LRESULT> TextAnnotationEditor::OnTripleClick(HWND field, LPARAM lparam) {
    if (!(last_double_ && ::GetTickCount() - last_double_ <= ::GetDoubleClickTime() &&
          std::abs(GET_X_LPARAM(lparam) - last_point_.x) <= ::GetSystemMetrics(SM_CXDOUBLECLK) &&
          std::abs(GET_Y_LPARAM(lparam) - last_point_.y) <= ::GetSystemMetrics(SM_CYDOUBLECLK))) {
        return std::nullopt;
    }
    const size_t index = LOWORD(::SendMessageW(field, EM_CHARFROMPOS, 0, lparam));
    const auto& value = owner_.text_document().annotations()[*owner_.text_selection()].text;
    size_t start = std::min(index, value.size()), end = start;
    while (start > 0 && value[start - 1] != L'\n') { --start; }
    while (end < value.size() && value[end] != L'\n') { ++end; }
    if (end < value.size()) { ++end; }
    ::SendMessageW(field, EM_SETSEL, start, end);
    last_double_ = 0;
    return 0;
}

LRESULT TextAnnotationEditor::DispatchToField(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const bool changes_pixels = (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
                                (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == EM_SETSEL || message == EM_REPLACESEL ||
                                message == WM_SETTEXT || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SIZE ||
                                message == WM_CUT || message == WM_PASTE || message == WM_CLEAR || message == WM_UNDO || message == EM_UNDO ||
                                message == WM_IME_COMPOSITION || message == WM_IME_ENDCOMPOSITION || message == WM_TIMER || message == WM_CAPTURECHANGED;
    // The native caret stays hidden for the whole dispatch. EDIT would show it
    // at its own GDI position first; it is placed against the rendered text
    // below, by which time the control is invalid, and Windows does not erase
    // a caret inside an update region. The first column would stay inverted
    // until the next paint.
    const bool caret_hidden = changes_pixels && ::HideCaret(hwnd) != FALSE;
    const auto result = [&] {
        ++dispatch_depth_;
        // Typing is not suspended. Suspending redraw takes WS_VISIBLE away and
        // gives it back, which Windows takes for pointer movement: the pointer
        // it hides while the user types came back at every key. The control
        // colour handler already keeps EDIT's own drawing off the screen.
        const bool typing = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        ui::NativeRedrawGuard redraw(hwnd, changes_pixels && !typing);
        // EDIT needs visibility for native caret/selection updates, even while
        // its internal redraw flag suppresses synchronous GDI text painting.
        const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (changes_pixels && (style & WS_VISIBLE) == 0) { ::SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_VISIBLE); }
        const auto result = ::DefSubclassProc(hwnd, message, wparam, lparam);
        --dispatch_depth_;
        return result;
    }();
    if (!dispatch_depth_ && layout_pending_) {
        layout_pending_ = false;
        Layout();
    }
    if (changes_pixels && !dispatch_depth_ && ::IsWindow(hwnd) && ::GetFocus() == hwnd && owner_.text_selection()) { PlaceCaret(hwnd); }
    // Fails harmlessly when the dispatch gave the caret away.
    if (caret_hidden && ::IsWindow(hwnd)) { ::ShowCaret(hwnd); }
    return result;
}

void TextAnnotationEditor::PlaceCaret(HWND field) const {
    DWORD start{}, end{};
    ::SendMessageW(field, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    // The native caret can be parked outside the old client bounds while
    // typing grows the host. Position it against the rendered text after
    // layout; native EDIT still owns selection, navigation and caret life.
    if (start != end) { return; }
    const auto& a = owner_.text_document().annotations()[*owner_.text_selection()];
    auto layout = TextLayout(a);
    float x{}, y{};
    DWRITE_HIT_TEST_METRICS metrics{};
    if (layout && SUCCEEDED(layout->HitTestTextPosition(start, FALSE, &x, &y, &metrics))) {
        RECT format{};
        ::SendMessageW(field, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
        const float zoom = owner_.text_view().zoom;
        ::SetCaretPos(format.left + static_cast<int>(std::lround(x * zoom)), format.top + static_cast<int>(std::lround(y * zoom)));
    }
}

void TextAnnotationEditor::Paint(HDC dc) {
    const auto selected = owner_.text_selection();
    if (!edit_ || !selected) { return; }
    RECT bounds{};
    ::GetWindowRect(edit_, &bounds);
    owner_.RenderTextBackground(dc, bounds);
    DWORD start{}, end{};
    ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (start == end) { return; }
    const auto& a = owner_.text_document().annotations()[*selected];
    auto layout = TextLayout(a);
    if (!layout) { return; }
    const float inset = TextInset(a);
    const TextEditorView view = owner_.text_view();
    // Layout units are image pixels; the host's pixels are scaled by the view.
    const float x = (view.origin.x - bounds.left) / view.zoom + a.points.front().x + inset + TagMarkerWidth(a), y = (view.origin.y - bounds.top) / view.zoom + a.points.front().y + inset;
    UINT32 count{};
    layout->HitTestTextRange(start, end - start, x, y, nullptr, 0, &count);
    std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
    if (SUCCEEDED(layout->HitTestTextRange(start, end - start, x, y, ranges.data(), count, &count))) {
        for (const auto& r : ranges) {
            RECT area{static_cast<LONG>(r.left * view.zoom), static_cast<LONG>(r.top * view.zoom), static_cast<LONG>(std::ceil((r.left + r.width) * view.zoom)), static_cast<LONG>(std::ceil((r.top + r.height) * view.zoom))};
            ::InvertRect(dc, &area);
        }
    }
}

}  // namespace easnap::editor
