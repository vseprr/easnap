// Easnap - the buttons of the screenshot editor's islands.
#include "editor/toolbar_item.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "editor/annotation_tools.h"
#include "strings/strings.h"
#include "ui/control_ids.h"

namespace easnap::editor {
namespace {

/// Every option and command button of the editor with its look. The tool
/// buttons take theirs from the tool records (editor/annotation_tools.h).
constexpr std::array<EditorButtonLook, 19> kEditorButtonLooks{{
    {kTextPlainButton, ui::Icon::Text, IDS_EDITOR_OPTION_PLAIN_TEXT},
    {kTextFilledButton, ui::Icon::TextFill, IDS_EDITOR_OPTION_FILLED_TEXT},
    {kTextOutlinedButton, ui::Icon::TextOutline, IDS_EDITOR_OPTION_OUTLINED_TEXT},
    {kBlurButton, ui::Icon::Droplet, IDS_EDITOR_OPTION_BLUR},
    {kPixelateButton, ui::Icon::Mosaic, IDS_EDITOR_OPTION_PIXELATE},
    {kBrushMethodButton, ui::Icon::Hand, IDS_EDITOR_OPTION_BRUSH, ui::ButtonRole::Standard, true},
    {kRegionMethodButton, ui::Icon::Region, IDS_EDITOR_OPTION_REGION},
    {kSpotlightRectangleButton, ui::Icon::Rectangle, IDS_EDITOR_OPTION_SPOTLIGHT_RECTANGLE},
    {kSpotlightEllipseButton, ui::Icon::Ellipse, IDS_EDITOR_OPTION_SPOTLIGHT_ELLIPSE},
    {kSpotlightRoundedButton, ui::Icon::RoundedRectangle, IDS_EDITOR_OPTION_SPOTLIGHT_ROUNDED},
    {CommandButton(kCommandUndo), ui::Icon::Undo, IDS_EDITOR_UNDO_BUTTON},
    {CommandButton(kCommandRedo), ui::Icon::Redo, IDS_EDITOR_REDO_BUTTON},
    {CommandButton(kCommandClear), ui::Icon::Clear, IDS_EDITOR_CLEAR_BUTTON},
    {CommandButton(kCommandCopy), ui::Icon::Check, IDS_EDITOR_COPY_BUTTON, ui::ButtonRole::Success},
    {CommandButton(kCommandSave), ui::Icon::Save, IDS_EDITOR_SAVE_BUTTON},
    {CommandButton(kCommandCancel), ui::Icon::Discard, IDS_EDITOR_DISCARD_BUTTON},
    {CommandButton(kCommandTheme), ui::Icon::Appearance, IDS_EDITOR_THEME_BUTTON},
    {CommandButton(kCommandScrollshot), ui::Icon::Scissors, IDS_SCROLLSHOT_TOOL},
    {CommandButton(kCommandPin), ui::Icon::Pin, IDS_EDITOR_PIN_BUTTON},
}};

/// The look of the button `identifier`; null when it has none in the table.
constexpr const EditorButtonLook* FindButtonLook(int identifier) noexcept {
    for (const auto& look : kEditorButtonLooks) {
        if (look.identifier == identifier) { return &look; }
    }
    return nullptr;
}

/// True when every button of `range` has a look.
constexpr bool RangeHasLooks(ui::ControlRange range) noexcept {
    for (int index = 0; index < range.count; ++index) {
        if (FindButtonLook(range.Id(index)) == nullptr) { return false; }
    }
    return true;
}

static_assert(RangeHasLooks(kTextModeButtons) && RangeHasLooks(kEffectButtons) &&
                  RangeHasLooks(kEffectMethodButtons) && RangeHasLooks(kSpotlightShapeButtons) &&
                  RangeHasLooks({CommandButton(kCommandUndo), kCommandPin - kCommandUndo + 1}),
              "every option and command button needs a look");

/// The subclass of every item button. `reference` is its ToolbarButtonOwner.
LRESULT CALLBACK ToolbarButtonProc(HWND hwnd, UINT message, WPARAM wparam,
                                   LPARAM lparam, UINT_PTR identifier, DWORD_PTR reference) {
    auto* owner = reinterpret_cast<ToolbarButtonOwner*>(reference);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, ToolbarButtonProc, identifier);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (ui::ConsumePopupInput(message, wparam)) { return 0; }
    if (message == WM_MOUSEMOVE && ::GetDlgCtrlID(hwnd) == kBrushMethodButton) { owner->BrushMethodHovered(); }
    // Every other control fixes the screenshot on its first press. Cancel,
    // the theme switch and Scrollshot must not acquire an image.
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) &&
        ::GetDlgCtrlID(hwnd) != CommandButton(kCommandCancel) && ::GetDlgCtrlID(hwnd) != CommandButton(kCommandTheme) &&
        ::GetDlgCtrlID(hwnd) != CommandButton(kCommandScrollshot) && !owner->FeatureUsed()) { return 0; }
    if (message == WM_KEYDOWN && (wparam == VK_ESCAPE || (::GetKeyState(VK_CONTROL) & 0x8000) != 0)) {
        return owner->ForwardButtonKey(message, wparam, lparam);
    }
    return ::DefSubclassProc(hwnd, message, wparam, lparam);
}

}  // namespace

EditorButtonLook ButtonLook(int identifier) noexcept {
    if (kToolButtons.Contains(identifier)) {
        const ToolDefinition& tool = Tool(static_cast<ToolId>(kToolButtons.Index(identifier)));
        return {identifier, tool.icon, tool.name};
    }
    return *FindButtonLook(identifier);
}

ToolbarFlow::ToolbarFlow(float available_width, float scale) noexcept
    : s(scale), pad(kEdgePadding * s), gap(kItemGap * s), row(kToolbarRowHeight * s),
      limit(std::max(available_width - pad, pad + kToolButton * s)), x(pad) {}

D2D1_RECT_F ToolbarFlow::Place(float width) noexcept {
    if (x > pad && x + width > limit) {
        x = pad;
        y += row;
    }
    const D2D1_RECT_F r{x, y + (row - kToolButton * s) / 2, x + width, y + (row + kToolButton * s) / 2};
    x += width + gap;
    return r;
}

RECT ToolbarFlow::Separator(float width) noexcept {
    const auto split = Place(width);
    return {static_cast<LONG>((split.left + split.right) / 2), static_cast<LONG>(split.top + 8 * s), static_cast<LONG>((split.left + split.right) / 2 + 1), static_cast<LONG>(split.bottom - 8 * s)};
}

bool CreateToolbarButton(const ToolbarItem& item, HWND parent, HWND tooltip, ToolbarButtonOwner& owner) {
    using namespace ui;
    constexpr std::array<UINT, 8> colors{IDS_EDITOR_COLOR_RED, IDS_EDITOR_COLOR_AMBER, IDS_EDITOR_COLOR_GREEN, IDS_EDITOR_COLOR_BLUE,
                                         IDS_EDITOR_COLOR_VIOLET, IDS_EDITOR_COLOR_WHITE, IDS_EDITOR_COLOR_BLACK, IDS_EDITOR_COLOR_GRAY};
    ButtonStyle style;
    std::wstring name = item.tooltip;
    style.icon_only = true;
    switch (item.kind) {
        case ToolbarItem::Kind::Tool:
        case ToolbarItem::Kind::Option:
        case ToolbarItem::Kind::Command: {
            const EditorButtonLook& look = ButtonLook(item.identifier);
            style.icon = look.icon;
            style.role = look.role;
            style.dropdown = look.dropdown;
            style.segmented = item.kind == ToolbarItem::Kind::Option;
            break;
        }
        case ToolbarItem::Kind::Color:
            name = strings::Get(colors[static_cast<size_t>(kColorButtons.Index(item.identifier))]);
            style.swatch = true;
            style.content_color = RGB(static_cast<BYTE>(std::lround(item.color.r * 255)),
                                      static_cast<BYTE>(std::lround(item.color.g * 255)), static_cast<BYTE>(std::lround(item.color.b * 255)));
            break;
        case ToolbarItem::Kind::Thickness:
            name = strings::Format(IDS_EDITOR_THICKNESS_BUTTON, {std::to_wstring(static_cast<int>(item.thickness))});
            style.sample = 2;
            style.sample_size = kBrushSizeOptions[static_cast<size_t>(kThicknessButtons.Index(item.identifier))];
            break;
        case ToolbarItem::Kind::BrushSize:
            name = strings::Format(IDS_EDITOR_BRUSH_SIZE_BUTTON, {std::to_wstring(static_cast<int>(item.brush_size))});
            style.sample = 2;
            style.sample_size = item.brush_size;
            break;
    }
    const DWORD type = item.kind == ToolbarItem::Kind::Command ? BS_PUSHBUTTON : BS_RADIOBUTTON | BS_PUSHLIKE;
    const bool first = item.identifier == kToolButtons.first || item.identifier == kColorButtons.first ||
                       item.identifier == kThicknessButtons.first || item.identifier == kBrushSizeButtons.first ||
                       item.identifier == CommandButton(kCommandUndo) || item.identifier == CommandButton(kCommandScrollshot);
    HWND button = ::CreateWindowExW(0, L"BUTTON", name.c_str(), WS_CHILD | WS_TABSTOP | type | (first ? WS_GROUP : 0),
                                    0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(item.identifier)), ::GetModuleHandleW(nullptr), nullptr);
    if (!button || !StyleButton(button, style) ||
        !::SetWindowSubclass(button, ToolbarButtonProc, kToolbarButtonSubclass, reinterpret_cast<DWORD_PTR>(&owner))) { return false; }
    if (tooltip) {
        TOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = parent;
        info.uId = reinterpret_cast<UINT_PTR>(button);
        info.lpszText = name.data();
        ::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    return true;
}

void PlaceToolbarButton(HWND button, const ToolbarItem& item, HFONT font) {
    if (((::GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE) != 0) != item.shown) { ::ShowWindow(button, item.shown ? SW_SHOWNA : SW_HIDE); }
    if (item.shown) {
        const RECT rect{static_cast<LONG>(item.rect.left), static_cast<LONG>(item.rect.top),
                        static_cast<LONG>(item.rect.right), static_cast<LONG>(item.rect.bottom)};
        RECT old{};
        ::GetWindowRect(button, &old);
        ::MapWindowPoints(nullptr, ::GetParent(button), reinterpret_cast<POINT*>(&old), 2);
        if (!::EqualRect(&rect, &old)) { ::MoveWindow(button, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE); }
    }
    if (reinterpret_cast<HFONT>(::SendMessageW(button, WM_GETFONT, 0, 0)) != font) {
        ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void MarkToolbarButton(HWND button, const ToolbarItem& item, bool selected, bool enabled) {
    if ((::IsWindowEnabled(button) != FALSE) != enabled) { ::EnableWindow(button, enabled); }
    const WPARAM checked = selected ? BST_CHECKED : BST_UNCHECKED;
    if (item.kind != ToolbarItem::Kind::Command && ::SendMessageW(button, BM_GETCHECK, 0, 0) != static_cast<LRESULT>(checked)) {
        ::SendMessageW(button, BM_SETCHECK, checked, 0);
    }
}

}  // namespace easnap::editor
