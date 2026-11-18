// Easnap - the screenshot editor's property island and brush size pop-up.
#include "editor/editor_context_panel.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

#include "editor/annotation_tools.h"
#include "editor/context_panel_layout.h"
#include "platform/paint_buffer.h"
#include "platform/screen_geometry.h"
#include "strings/strings.h"
#include "ui/capture_workspace.h"
#include "ui/control_ids.h"
#include "ui/island_layout.h"
#include "ui/native_controls.h"
#include "ui/ui_theme.h"

namespace easnap::editor {

// ---------------------------------------------------------------------------
// Windows and layout
// ---------------------------------------------------------------------------

void EditorContextPanel::DestroyBrushPopup() {
    if (brush_popup_) {
        ::DestroyWindow(brush_popup_);
        brush_popup_ = nullptr;
    }
}

void EditorContextPanel::DestroyIsland() {
    if (context_) {
        ::DestroyWindow(context_);
        context_ = nullptr;
    }
}

HWND EditorContextPanel::Control(int id) const {
    if (id == kWatermarkClearButton) { return context_ ? ::GetDlgItem(::GetDlgItem(context_, kWatermarkTextEdit), id) : nullptr; }
    if (const auto child = context_ ? ::GetDlgItem(context_, id) : nullptr) { return child; }
    return brush_popup_ ? ::GetDlgItem(brush_popup_, id) : nullptr;
}

void EditorContextPanel::Build(float available_width) {
    items_.clear();
    context_fields_.clear();
    context_separators_.clear();
    const float s = owner_.island_scale();
    const HFONT font = owner_.island_font();
    const auto tool = owner_.ContextTool();
    const auto [context_height, widest] = LayOutContextPanel(tool, available_width, s, font, items_, context_fields_, context_separators_);
    context_visible_ = PropertiesOf(tool).island;
    context_width_ = static_cast<int>(std::ceil(std::min(available_width, std::max(widest, 200 * s))));
    context_height_ = context_visible_ ? static_cast<int>(std::ceil(context_height)) : 0;
    // The islands are placed around the frame with room for the tallest
    // property island any tool shows, so choosing a tool never moves them.
    float tallest = context_height;
    std::vector<ToolbarItem> scratch_items;
    std::vector<std::pair<int, RECT>> scratch_fields;
    std::vector<RECT> scratch_separators;
    for (const ToolId entry : kToolbarOrder) {
        scratch_items.clear();
        scratch_fields.clear();
        scratch_separators.clear();
        tallest = std::max(tallest, LayOutContextPanel(entry, available_width, s, font, scratch_items, scratch_fields, scratch_separators).first);
    }
    reserved_context_height_ = static_cast<int>(std::ceil(tallest));
}

SIZE EditorContextPanel::GroupSize() const noexcept {
    RECT main{};
    const HWND tools = owner_.tools_island();
    if (!tools || !::GetWindowRect(tools, &main)) { return {}; }
    const LONG context = std::max(context_visible_ ? context_height_ : 0, reserved_context_height_);
    return SIZE{main.right - main.left, main.bottom - main.top + (context > 0 ? ui::theme::Scaled(ui::theme::kIslandGap, owner_.island_scale()) + context : 0)};
}

std::optional<ToolbarItem> EditorContextPanel::Find(int identifier) const {
    const auto found = std::find_if(items_.begin(), items_.end(), [identifier](const auto& item) { return item.identifier == identifier; });
    return found != items_.end() ? std::optional<ToolbarItem>(*found) : std::nullopt;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

bool EditorContextPanel::CreateButtons(HWND tooltip, ToolbarButtonOwner& owner) {
    for (const auto& item : items_) {
        const HWND parent = item.kind == ToolbarItem::Kind::BrushSize ? brush_popup_ : context_;
        if (!CreateToolbarButton(item, parent, tooltip, owner)) { return false; }
    }
    return true;
}

void EditorContextPanel::CreateControls() {
    const auto make = [&](const wchar_t* cls, const wchar_t* name, DWORD style, int id) {
        HWND child = ::CreateWindowExW(0, cls, name, WS_CHILD | WS_TABSTOP | style, 0, 0, 1, 1, context_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
        if (child) { ::SetWindowSubclass(child, PropertyProc, 1, reinterpret_cast<DWORD_PTR>(this)); }
        if (child && ::lstrcmpW(cls, L"STATIC") == 0 && !ui::StyleLabel(child)) {
            ::DestroyWindow(child);
            return static_cast<HWND>(nullptr);
        }
        return child;
    };
    HWND font = make(L"COMBOBOX", strings::Get(IDS_EDITOR_FONT_SIZE_NAME), CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, kFontSizeCombo);
    if (font) {
        for (const wchar_t* value : {L"12 pt", L"16 pt", L"24 pt", L"36 pt", L"48 pt", L"60 pt", L"72 pt", L"96 pt"}) { ::SendMessageW(font, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value)); }
        ::SendMessageW(font, CB_LIMITTEXT, 10, 0);
        static_cast<void>(ui::StyleInput(font));
        ui::SetAccessibleName(font, strings::Get(IDS_EDITOR_FONT_SIZE_NAME));
    }
    for (int id : {kEffectStrengthSlider, kOpacitySlider, kFontSizeSlider}) {
        HWND slider = make(TRACKBAR_CLASSW, strings::Get(id == kEffectStrengthSlider ? IDS_EDITOR_EFFECT_STRENGTH_NAME : id == kOpacitySlider ? IDS_EDITOR_OPACITY_NAME
                                                                                                                                              : IDS_EDITOR_WATERMARK_SIZE_NAME),
                           TBS_HORZ | TBS_NOTICKS, id);
        ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(id == kFontSizeSlider ? 8 : 0, id == kFontSizeSlider ? 96 : 100));
        ::SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
        static_cast<void>(ui::StyleSlider(slider));
    }
    HWND text = make(L"EDIT", L"", ES_AUTOHSCROLL, kWatermarkTextEdit);
    ::SendMessageW(text, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(strings::Get(IDS_EDITOR_WATERMARK_TEXT_CUE)));
    ::SendMessageW(text, EM_SETLIMITTEXT, 16, 0);
    static_cast<void>(ui::StyleInput(text));
    ui::SetAccessibleName(text, strings::Get(IDS_EDITOR_WATERMARK_TEXT_NAME));
    HWND clear = make(L"BUTTON", strings::Get(IDS_EDITOR_WATERMARK_CLEAR_BUTTON), BS_PUSHBUTTON, kWatermarkClearButton);
    ui::ButtonStyle clear_style;
    clear_style.icon = ui::Icon::Discard;
    clear_style.icon_only = true;
    clear_style.segmented = true;
    clear_style.circular = true;
    static_cast<void>(ui::StyleButton(clear, clear_style));
    ::RemoveWindowSubclass(clear, PropertyProc, 1);
    ::SetWindowSubclass(clear, PropertyProc, 1, reinterpret_cast<DWORD_PTR>(this));
    ::SetParent(clear, text);
    ::SetWindowLongPtrW(text, GWL_STYLE, ::GetWindowLongPtrW(text, GWL_STYLE) | WS_CLIPCHILDREN);
    for (int id : {kEffectStrengthValue, kOpacityValue, kFontSizeValue}) { make(L"STATIC", L"", SS_CENTERIMAGE, id); }
}

void EditorContextPanel::SyncButtons(const AnnotationStyle& style, HFONT font) const {
    for (const auto& item : items_) {
        HWND button = Control(item.identifier);
        if (!button) { continue; }
        PlaceToolbarButton(button, item, font);
        bool selected = false;
        switch (item.kind) {
            case ToolbarItem::Kind::Color: selected = item.color.r == style.color.r && item.color.g == style.color.g && item.color.b == style.color.b; break;
            case ToolbarItem::Kind::Thickness: selected = item.thickness == style.thickness; break;
            case ToolbarItem::Kind::BrushSize: selected = item.brush_size == style.brush_size; break;
            case ToolbarItem::Kind::Option:
                selected = (kTextModeButtons.Contains(item.identifier) && static_cast<int>(style.text_mode) == kTextModeButtons.Index(item.identifier)) ||
                           (kEffectButtons.Contains(item.identifier) && static_cast<int>(style.effect) == kEffectButtons.Index(item.identifier)) ||
                           (kEffectMethodButtons.Contains(item.identifier) && static_cast<int>(style.method) == kEffectMethodButtons.Index(item.identifier)) ||
                           (kSpotlightShapeButtons.Contains(item.identifier) && static_cast<int>(style.shape) == kSpotlightShapeButtons.Index(item.identifier));
                break;
            case ToolbarItem::Kind::Tool:
            case ToolbarItem::Kind::Command: break;
        }
        MarkToolbarButton(button, item, selected, true);
    }
}

void EditorContextPanel::SyncWatermarkClear() {
    const HWND text = Control(kWatermarkTextEdit), clear = Control(kWatermarkClearButton);
    if (!text || !clear) { return; }
    RECT field{};
    ::GetWindowRect(text, &field);
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const bool over = ::PtInRect(&field, cursor) != FALSE;
    const bool show = !owner_.modal_active() && PropertiesOf(owner_.ContextTool()).watermark && ::IsWindowVisible(context_) &&
                      ::GetWindowTextLengthW(text) > 0 && over;
    const bool shown = (::GetWindowLongPtrW(clear, GWL_STYLE) & WS_VISIBLE) != 0;
    if (shown != show) {
        ::ShowWindow(clear, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show) {
            ::SetTimer(context_, kWatermarkClearHoverTimer, 40, nullptr);
        } else {
            ::KillTimer(context_, kWatermarkClearHoverTimer);
        }
    }
}

LRESULT CALLBACK EditorContextPanel::PropertyProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR reference) {
    auto* self = reinterpret_cast<EditorContextPanel*>(reference);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, PropertyProc, id);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    const int control = ::GetDlgCtrlID(hwnd);
    if (control == kWatermarkTextEdit && message == WM_COMMAND && LOWORD(wparam) == kWatermarkClearButton) { return self->owner_.HandleMessage(self->context_, message, wparam, lparam); }
    const bool clear_hover = (control == kWatermarkTextEdit || control == kWatermarkClearButton) &&
                             (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE || message == WM_SETFOCUS || message == WM_KILLFOCUS);
    if (clear_hover && message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
    }
    const bool slider = kPropertySliders.Contains(control);
    if (!self->syncing_context_ && ((slider && (message == WM_LBUTTONDOWN || message == WM_KEYDOWN)) || (!slider && message == WM_SETFOCUS))) {
        // Only a transaction opened here is committed here: a text edit or a
        // transform that is already open keeps its own undo boundary.
        self->owner_.OpenContextTransaction();
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        self->owner_.CancelContextTransaction();
        self->owner_.RequestDiscard();
        return 0;
    }
    const LRESULT result = ::DefSubclassProc(hwnd, message, wparam, lparam);
    if (clear_hover) { self->SyncWatermarkClear(); }
    if ((slider && (message == WM_LBUTTONUP || message == WM_KEYUP)) || (!slider && message == WM_KILLFOCUS)) { self->owner_.CommitContextTransaction(); }
    if (slider && message == WM_CAPTURECHANGED && ::GetKeyState(VK_LBUTTON) < 0 && self->owner_.context_owns_transaction()) {
        self->owner_.CancelContextTransaction();
        self->Sync();
        self->owner_.Invalidate();
    }
    return result;
}

void EditorContextPanel::Sync(bool layout) {
    if (!context_ || syncing_context_) { return; }
    syncing_context_ = true;
    const auto tool = owner_.ContextTool();
    const auto style = owner_.ContextStyle();
    const ToolProperties& shows = PropertiesOf(tool);
    const float scale = owner_.island_scale();
    if (layout) { PositionControls(scale, owner_.island_font()); }
    const auto label = [&](int id, const wchar_t* value) {
        wchar_t old[256]{};
        ::GetWindowTextW(Control(id), old, 256);
        if (::lstrcmpW(old, value) != 0) { ::SetWindowTextW(Control(id), value); }
    };
    const auto slider = [&](int id, float value) {
        if (::SendMessageW(Control(id), TBM_GETPOS, 0, 0) != static_cast<LPARAM>(value)) { ::SendMessageW(Control(id), TBM_SETPOS, TRUE, static_cast<LPARAM>(value)); }
    };
    if (shows.font_size && ::GetFocus() != Control(kFontSizeCombo) && !::IsChild(Control(kFontSizeCombo), ::GetFocus())) {
        wchar_t value[32]{};
        ::swprintf_s(value, L"%g pt", static_cast<double>(style.font_size));
        label(kFontSizeCombo, value);
    }
    if (shows.watermark) {
        const auto& annotations = owner_.island_annotations();
        const auto found = std::find_if(annotations.begin(), annotations.end(), [](const auto& a) { return a.tool == ToolId::Watermark; });
        // Undo may clear selection while the input still owns focus. Always
        // synchronize from the document; unchanged typing never resets EDIT.
        label(kWatermarkTextEdit, found != annotations.end() ? found->text.c_str() : L"");
    }
    if (shows.watermark) { PlaceWatermarkClear(scale); }
    SyncWatermarkClear();
    slider(kEffectStrengthSlider, style.strength);
    slider(kOpacitySlider, style.opacity);
    slider(kFontSizeSlider, style.font_size);
    const std::wstring strength = strings::Format(style.effect == EffectMode::Blur ? IDS_EDITOR_BLUR_INTENSITY_VALUE : IDS_EDITOR_BLOCK_SIZE_VALUE, {std::to_wstring(static_cast<int>(style.effect == EffectMode::Blur ? style.strength : 2 + style.strength * 0.7f))});
    label(kEffectStrengthValue, strength.c_str());
    label(kOpacityValue, strings::Format(IDS_EDITOR_OPACITY_VALUE, {std::to_wstring(static_cast<int>(style.opacity))}).c_str());
    label(kFontSizeValue, strings::Format(IDS_EDITOR_SIZE_VALUE, {std::to_wstring(static_cast<int>(style.font_size))}).c_str());
    syncing_context_ = false;
}

void EditorContextPanel::PositionControls(float scale, HFONT font) {
    const auto position = [&](int id, bool shown, float x, float y, float width, float height) {
        HWND child = Control(id);
        if (!child) { return; }
        if ((::IsWindowVisible(child) != FALSE) != shown) { ::ShowWindow(child, shown ? SW_SHOWNA : SW_HIDE); }
        if (shown) {
            RECT old{};
            ::GetWindowRect(child, &old);
            ::MapWindowPoints(nullptr, context_, reinterpret_cast<POINT*>(&old), 2);
            const RECT next{static_cast<LONG>(x * scale), static_cast<LONG>(y * scale), static_cast<LONG>((x + width) * scale), static_cast<LONG>((y + height) * scale)};
            if (old.left != next.left || old.top != next.top || old.right != next.right || (id != kFontSizeCombo && old.bottom != next.bottom)) {
                ::SetWindowPos(child, nullptr, next.left, next.top, next.right - next.left, next.bottom - next.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        if (reinterpret_cast<HFONT>(::SendMessageW(child, WM_GETFONT, 0, 0)) != font) { ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); }
    };
    for (int id : {kFontSizeCombo, kEffectStrengthSlider, kOpacitySlider, kFontSizeSlider, kWatermarkTextEdit, kEffectStrengthValue, kOpacityValue, kFontSizeValue}) {
        const auto it = std::find_if(context_fields_.begin(), context_fields_.end(), [id](const auto& field) { return field.first == id; });
        if (it == context_fields_.end()) {
            position(id, false, 0, 0, 1, 1);
            continue;
        }
        const auto r = it->second;
        position(id, true, r.left / scale, r.top / scale, (r.right - r.left) / scale, id == kFontSizeCombo ? 280.0f : (r.bottom - r.top) / scale);
    }
}

void EditorContextPanel::PlaceWatermarkClear(float scale) {
    RECT field{};
    ::GetClientRect(Control(kWatermarkTextEdit), &field);
    const LONG size = ui::theme::Scaled(32, scale), inset = ui::theme::Scaled(2, scale);
    // Moved and the margin set only when they change: this runs on every
    // key, and a window moved under the pointer makes Windows re-send it.
    const RECT place{field.right - size - inset, field.top + (field.bottom - field.top - size) / 2,
                     field.right - inset, field.top + (field.bottom - field.top - size) / 2 + size};
    RECT current{};
    ::GetWindowRect(Control(kWatermarkClearButton), &current);
    ::MapWindowPoints(nullptr, Control(kWatermarkTextEdit), reinterpret_cast<POINT*>(&current), 2);
    if (::EqualRect(&current, &place) == FALSE) {
        ::SetWindowPos(Control(kWatermarkClearButton), HWND_TOP, place.left, place.top, size, size, SWP_NOACTIVATE);
    }
    const LONG margin = size + inset * 2;
    if (static_cast<LONG>(HIWORD(::SendMessageW(Control(kWatermarkTextEdit), EM_GETMARGINS, 0, 0))) != margin) {
        ::SendMessageW(Control(kWatermarkTextEdit), EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, margin));
    }
}

// ---------------------------------------------------------------------------
// Placement and the brush size pop-up
// ---------------------------------------------------------------------------

void EditorContextPanel::Place(bool follow_frame) {
    const HWND tools = owner_.tools_island();
    if (!tools || !context_ || repositioning_ || owner_.modal_active()) { return; }
    repositioning_ = true;
    const float scale = owner_.island_scale();
    RECT main{};
    ::GetWindowRect(tools, &main);
    const auto monitor = platform::MonitorForRect(follow_frame ? owner_.island_frame() : main);
    const auto work = monitor.work_area;
    const LONG gap = ui::theme::Scaled(ui::theme::kIslandGap, scale);
    const LONG width = main.right - main.left;
    const LONG height = main.bottom - main.top;
    const LONG popup_space = brush_popup_open_ ? gap + ui::theme::Scaled(kToolbarRowHeight + 6, scale) : 0;
    const LONG shown = height + (context_visible_ ? gap + context_height_ : 0) + popup_space;
    // Outside the frame while anything outside has room, and never over the
    // mode island (ui/island_layout.h). An island the user moved, or one that
    // only changed its height, stays where it is unless that covers the mode
    // island.
    const RECT keep_clear = owner_.IslandKeepClear();
    RECT group = platform::ClampRectToBounds({main.left, main.top, main.left + width, main.top + shown}, work);
    RECT covered{};
    if (follow_frame || ::IntersectRect(&covered, &group, &keep_clear)) {
        // Placed around the frame with room for the tallest property island,
        // shown or not: choosing a tool must not send the islands to the other
        // side of the frame.
        const LONG reserved = std::max(shown, GroupSize().cy + popup_space);
        const RECT frame = owner_.island_frame();
        const auto frame_monitor = platform::MonitorForRect(frame);
        group = ui::PlaceIslandForFrame(frame, frame_monitor.bounds, frame_monitor.work_area, SIZE{width, reserved}, gap, keep_clear).rect;
    }
    ::SetWindowPos(tools, nullptr, group.left, group.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    RECT context{group.left + (width - context_width_) / 2, group.top + height + gap, 0, 0};
    context.right = context.left + context_width_;
    context.bottom = context.top + context_height_;
    context = platform::ClampRectToBounds(context, platform::MonitorForRect(group).work_area);
    ::SetWindowPos(context_, HWND_TOPMOST, context.left, context.top, context_width_, std::max(1, context_height_), SWP_NOACTIVATE);
    ui::RoundWindow(context_);
    ui::RoundWindow(tools);
    const bool show = owner_.islands_shown() && !owner_.frame_interacting() && context_visible_;
    if (show && !::IsWindowVisible(context_) && ui::theme::AnimationsEnabled()) {
        context_fade_started_ = ::GetTickCount64();
        ::SetLayeredWindowAttributes(context_, 0, 0, LWA_ALPHA);
        ::SetTimer(context_, kContextFadeTimer, ui::theme::kAnimationTickMs, nullptr);
    }
    if (!show) {
        ::KillTimer(context_, kContextFadeTimer);
        context_fade_started_ = 0;
        ::SetLayeredWindowAttributes(context_, 0, 255, LWA_ALPHA);
    }
    ::ShowWindow(context_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (brush_popup_open_) { ShowBrushPopup(true); }
    repositioning_ = false;
}

void EditorContextPanel::ShowBrushPopup(bool show) {
    if (!brush_popup_) { return; }
    if (show && owner_.islands_shown() && !owner_.modal_active() && PropertiesOf(owner_.ContextTool()).effect) {
        brush_popup_open_ = true;
        // Reserve all three islands as one group before showing the final one.
        // At the bottom edge, move the group upward instead of covering tools.
        if (!repositioning_) {
            Place();
            return;
        }
        const float scale = owner_.island_scale();
        RECT anchor{}, context{};
        ::GetWindowRect(Control(kBrushMethodButton), &anchor);
        ::GetWindowRect(context_, &context);
        const LONG width = ui::theme::Scaled(132, scale), height = ui::theme::Scaled(kToolbarRowHeight + 6, scale), gap = ui::theme::Scaled(ui::theme::kIslandGap, scale);
        const auto work = platform::MonitorForRect(anchor).work_area;
        const LONG center = (anchor.left + anchor.right) / 2, callout = ui::theme::Scaled(6, scale);
        const LONG top = context.bottom + gap;
        RECT r{center - width / 2, top, center + width - width / 2, top + height};
        r = platform::ClampRectToBounds(r, work);
        const LONG callout_x = std::clamp(center - r.left, callout * 2, width - callout * 2);
        HRGN shape = ::CreateRoundRectRgn(0, callout, width + 1, height + 1, gap * 2, gap * 2);
        POINT triangle[] = {{callout_x - callout, callout}, {callout_x, 0}, {callout_x + callout, callout}};
        HRGN pointer = ::CreatePolygonRgn(triangle, 3, WINDING);
        if (shape && pointer) { ::CombineRgn(shape, shape, pointer, RGN_OR); }
        if (pointer) { ::DeleteObject(pointer); }
        if (shape && !::SetWindowRgn(brush_popup_, shape, FALSE)) { ::DeleteObject(shape); }
        for (int id = kBrushSizeButtons.first; kBrushSizeButtons.Contains(id); ++id) {
            const auto item = std::find_if(items_.begin(), items_.end(), [id](const auto& value) { return value.identifier == id; });
            if (item != items_.end()) { ::SetWindowPos(Control(id), nullptr, static_cast<int>(item->rect.left), static_cast<int>(item->rect.top), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
        }
        ::SetWindowPos(brush_popup_, HWND_TOPMOST, r.left, r.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::InvalidateRect(brush_popup_, nullptr, FALSE);
        brush_leave_started_ = 0;
        ::SetTimer(context_, kBrushPopupHoverTimer, 30, nullptr);
    } else {
        brush_popup_open_ = false;
        ::ShowWindow(brush_popup_, SW_HIDE);
        ::KillTimer(context_, kBrushPopupHoverTimer);
        brush_hover_started_ = brush_leave_started_ = 0;
    }
}

void EditorContextPanel::OnBrushMethodHover() {
    if (!brush_hover_started_) { brush_hover_started_ = ::GetTickCount64(); }
    ::SetTimer(context_, kBrushPopupHoverTimer, 30, nullptr);
}

void EditorContextPanel::StopForDialog() {
    ::KillTimer(context_, kContextFadeTimer);
    context_fade_started_ = 0;
    ::KillTimer(context_, kWatermarkClearHoverTimer);
    ::SendMessageW(Control(kFontSizeCombo), CB_SHOWDROPDOWN, FALSE, 0);
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

std::optional<LRESULT> EditorContextPanel::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_TIMER:
            if (wparam == kWatermarkClearHoverTimer) {
                SyncWatermarkClear();
                return 0;
            }
            if (wparam == kBrushPopupHoverTimer) {
                OnBrushHoverTimer();
                return 0;
            }
            if (wparam == kContextFadeTimer && hwnd == context_) {
                OnFadeTimer();
                return 0;
            }
            break;
        case WM_HSCROLL:
            if (!syncing_context_) {
                OnSliderScroll(wparam, lparam);
                return 0;
            }
            break;
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure && measure->CtlType == ODT_COMBOBOX) {
                measure->itemHeight = ui::theme::Scaled(32, owner_.island_scale());
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draw && ui::DrawComboItem(*draw)) { return TRUE; }
            break;
        }
        default:
            break;
    }
    return std::nullopt;
}

void EditorContextPanel::OnBrushHoverTimer() {
    POINT point{};
    ::GetCursorPos(&point);
    const auto over = [&](HWND window) { RECT r{}; ::GetWindowRect(window,&r); return ::IsWindowVisible(window) && ::PtInRect(&r,point); };
    const auto now = ::GetTickCount64();
    const bool hand = over(Control(kBrushMethodButton));
    if (hand || over(brush_popup_)) {
        brush_leave_started_ = 0;
        if (hand && brush_hover_started_ && now - brush_hover_started_ >= 350 && !::IsWindowVisible(brush_popup_)) { ShowBrushPopup(true); }
    } else if (!brush_leave_started_) {
        brush_leave_started_ = now;
    } else if (now - brush_leave_started_ >= 250) {
        ShowBrushPopup(false);
    }
}

void EditorContextPanel::OnFadeTimer() {
    const float progress = std::clamp(static_cast<float>(::GetTickCount64() - context_fade_started_) / ui::theme::kIslandFadeMs, 0.0f, 1.0f);
    ::SetLayeredWindowAttributes(context_, 0, static_cast<BYTE>(255 * progress), LWA_ALPHA);
    if (progress >= 1 || !ui::theme::AnimationsEnabled()) {
        ::KillTimer(context_, kContextFadeTimer);
        ::SetLayeredWindowAttributes(context_, 0, 255, LWA_ALPHA);
    }
}

void EditorContextPanel::OnSliderScroll(WPARAM wparam, LPARAM lparam) {
    const int id = ::GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
    auto value = owner_.ContextStyle();
    const float position = static_cast<float>(::SendMessageW(reinterpret_cast<HWND>(lparam), TBM_GETPOS, 0, 0));
    if (id == kEffectStrengthSlider) { value.strength = position; }
    if (id == kOpacitySlider) { value.opacity = position; }
    if (id == kFontSizeSlider) { value.font_size = position; }
    owner_.ChangeStyle(value);
    if (LOWORD(wparam) == TB_ENDTRACK) { owner_.CommitContextTransaction(); }
}

bool EditorContextPanel::HandleCommand(int id, int notification) {
    if (id == kFontSizeCombo && notification == CBN_DROPDOWN) {
        COMBOBOXINFO info{sizeof(info)};
        ::GetComboBoxInfo(Control(kFontSizeCombo), &info);
        ::SetWindowLongPtrW(info.hwndList, GWL_STYLE, ::GetWindowLongPtrW(info.hwndList, GWL_STYLE) & ~WS_BORDER);
        ::SetWindowLongPtrW(info.hwndList, GWL_EXSTYLE, ::GetWindowLongPtrW(info.hwndList, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
        ui::RoundWindow(info.hwndList);
        // Shown again on every drop-down: excluded as an opaque layered
        // window, it leaves no hole in a capture.
        static_cast<void>(ui::ExcludeFromCapture(info.hwndList));
        return true;
    }
    if (syncing_context_) { return false; }
    if (id == kFontSizeCombo && (notification == CBN_SELCHANGE || notification == CBN_KILLFOCUS)) {
        OnFontSizeEntered(notification);
        return true;
    }
    if (id == kWatermarkClearButton && notification == BN_CLICKED) {
        OnWatermarkClear();
        return true;
    }
    // The island writing the document's text into the field changes nothing.
    if (id == kWatermarkTextEdit && notification == EN_CHANGE && !syncing_context_ && PropertiesOf(owner_.ContextTool()).watermark) {
        OnWatermarkEdited();
        return true;
    }
    return false;
}

void EditorContextPanel::OnFontSizeEntered(int notification) {
    wchar_t text[32]{};
    if (notification == CBN_SELCHANGE) {
        ::SendMessageW(Control(kFontSizeCombo), CB_GETLBTEXT, ::SendMessageW(Control(kFontSizeCombo), CB_GETCURSEL, 0, 0), reinterpret_cast<LPARAM>(text));
    } else {
        ::GetWindowTextW(Control(kFontSizeCombo), text, 32);
    }
    wchar_t* end = nullptr;
    const double size = std::wcstod(text, &end);
    while (end && std::iswspace(*end)) { ++end; }
    if (end && ::lstrcmpiW(end, L"pt") == 0) { end += 2; }
    if (end && *end == 0 && std::isfinite(size) && size >= 6 && size <= 288) {
        auto value = owner_.ContextStyle();
        value.font_size = static_cast<float>(size);
        owner_.ChangeStyle(value);
    } else {
        ui::ShowCaptureStatusNotice(owner_.island_frame(), strings::Get(IDS_EDITOR_NOTICE_FONT_SIZE_RANGE));
        Sync();
        owner_.Invalidate();
    }
    if (notification == CBN_KILLFOCUS) { owner_.CommitContextTransaction(); }
}

void EditorContextPanel::OnWatermarkClear() {
    // The clearing is kept, with what the field held before it.
    owner_.ClearWatermarkField(Control(kWatermarkTextEdit));
    ::SetFocus(Control(kWatermarkTextEdit));
    // A field that kept the focus gets no WM_SETFOCUS to open the next
    // edit: open it here, so that Esc reverts what is typed after this.
    if (::GetFocus() == Control(kWatermarkTextEdit)) { owner_.OpenContextTransaction(); }
    SyncWatermarkClear();
}

void EditorContextPanel::OnWatermarkEdited() {
    if (!owner_.FeatureUsed()) { return; }
    const int length = ::GetWindowTextLengthW(Control(kWatermarkTextEdit));
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(Control(kWatermarkTextEdit), text.data(), length + 1);
    text.resize(std::min(length, 16));
    if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) { text.pop_back(); }
    if (text.size() != static_cast<size_t>(length)) {
        syncing_context_ = true;
        ::SetWindowTextW(Control(kWatermarkTextEdit), text.c_str());
        ::SendMessageW(Control(kWatermarkTextEdit), EM_SETSEL, text.size(), text.size());
        syncing_context_ = false;
    }
    owner_.SetWatermarkText(std::move(text));
}

void EditorContextPanel::Paint(HDC dc, HWND window) const {
    const float scale = owner_.island_scale();
    RECT client{};
    ::GetClientRect(window, &client);
    platform::PaintBuffer buffer(dc, client);
    dc = buffer.dc();
    ui::theme::DrawPanel(dc, client, scale);
    if (window == context_)
        for (const auto& r : context_separators_) { ui::DrawSeparator(dc, r, scale); }
}

}  // namespace easnap::editor
