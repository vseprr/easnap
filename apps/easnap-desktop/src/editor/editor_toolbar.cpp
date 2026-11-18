// Easnap - the screenshot editor's tools island.
#include "editor/editor_toolbar.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>

#include "editor/annotation_tools.h"
#include "platform/paint_buffer.h"
#include "platform/screen_geometry.h"
#include "strings/strings.h"
#include "ui/control_ids.h"
#include "ui/native_controls.h"
#include "ui/ui_theme.h"

namespace easnap::editor {
namespace {

/// Gap between the frame and the island below it, in device-independent pixels.
constexpr float kCanvasGap = 8.0f;

}  // namespace

void EditorToolbar::Build(float available_width, float scale) {
    items_.clear();
    separators_.clear();
    ToolbarFlow flow(available_width, scale);
    const float s = scale;
    flow.x = flow.pad + ui::theme::kIslandGripWidth * s;
    for (const ToolId tool : kToolbarOrder) {
        const ToolDefinition& definition = Tool(tool);
        if (definition.separator_before) { separators_.push_back(flow.Separator(8 * s)); }
        ToolbarItem item;
        item.tool = tool;
        item.identifier = definition.identifier;
        item.label = item.tooltip = strings::Get(definition.name);
        item.rect = flow.Place(kToolButton * s);
        items_.push_back(item);
    }
    separators_.push_back(flow.Separator(kGroupGap * s));
    {
        // Scrollshot is a different kind of capture, not an annotation tool or
        // an edit command, so it stands alone between the two groups.
        ToolbarItem item;
        item.kind = ToolbarItem::Kind::Command;
        item.command = kCommandScrollshot;
        item.identifier = CommandButton(kCommandScrollshot);
        item.label = item.tooltip = strings::Get(ButtonLook(item.identifier).tooltip);
        item.rect = flow.Place(kCommandButton * s);
        items_.push_back(item);
        separators_.push_back(flow.Separator(8 * s));
    }
    constexpr int commands[] = {kCommandUndo, kCommandRedo, kCommandClear, kCommandTheme, kCommandPin, kCommandSave, kCommandCancel, kCommandCopy};
    for (const int command : commands) {
        ToolbarItem item;
        item.kind = ToolbarItem::Kind::Command;
        item.command = command;
        item.identifier = CommandButton(command);
        item.label = item.tooltip = strings::Get(ButtonLook(item.identifier).tooltip);
        item.rect = flow.Place(kCommandButton * s);
        items_.push_back(item);
    }
    toolbar_rect_ = {0, 0, static_cast<LONG>(available_width), static_cast<LONG>(std::ceil(flow.y + flow.row))};
}

void EditorToolbar::Resize(int client_width, int client_height) noexcept {
    client_width_ = std::max(1, client_width);
    client_height_ = std::max(1, client_height);
}

bool EditorToolbar::CreateButtons(HWND island, ToolbarButtonOwner& owner) {
    tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
                                 WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, island, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ui::StyleTooltip(tooltip_);
    for (const auto& item : items_) {
        if (!CreateToolbarButton(item, island, tooltip_, owner)) { return false; }
    }
    return true;
}

void EditorToolbar::AddScrollshotDisabledTip(HWND island) {
    if (tooltip_) {
        TOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_SUBCLASS;
        info.hwnd = island;
        info.uId = kScrollshotDisabledTip;
        info.lpszText = const_cast<wchar_t*>(strings::Get(IDS_SCROLLSHOT_TOOL_FIXED));
        ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
}

void EditorToolbar::SuspendTooltips() const {
    if (tooltip_) {
        ::SendMessageW(tooltip_, TTM_POP, 0, 0);
        ::SendMessageW(tooltip_, TTM_ACTIVATE, FALSE, 0);
    }
}

void EditorToolbar::ResumeTooltips() const {
    if (tooltip_) { ::SendMessageW(tooltip_, TTM_ACTIVATE, TRUE, 0); }
}

void EditorToolbar::DestroyTooltip() {
    if (tooltip_) {
        ::DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
}

void EditorToolbar::SyncButtons(HWND island, const ToolbarStatus& status, HFONT font) const {
    for (const auto& item : items_) {
        HWND button = ::GetDlgItem(island, item.identifier);
        if (!button) { continue; }
        PlaceToolbarButton(button, item, font);
        bool selected = false;
        bool enabled = true;
        if (item.kind == ToolbarItem::Kind::Tool) { selected = item.tool == status.tool; }
        if (item.kind == ToolbarItem::Kind::Command) {
            if (item.command == kCommandUndo || item.command == kCommandClear) { enabled = status.can_undo; }
            if (item.command == kCommandCopy || item.command == kCommandSave || item.command == kCommandPin) {
                enabled = !status.exporting;
            }
            if (item.command == kCommandRedo) { enabled = status.can_redo; }
            if (item.command == kCommandScrollshot) {
                // A scrolling capture starts from a provisional frame only.
                enabled = !status.has_image && ui::CaptureExclusionSupported();
                if (tooltip_ && item.shown) {
                    TOOLINFOW info{sizeof(info)};
                    info.hwnd = island;
                    info.uId = kScrollshotDisabledTip;
                    info.rect = RECT{static_cast<LONG>(item.rect.left), static_cast<LONG>(item.rect.top),
                                     static_cast<LONG>(item.rect.right), static_cast<LONG>(item.rect.bottom)};
                    ::SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&info));
                }
            }
        }
        MarkToolbarButton(button, item, selected, enabled);
    }
}

std::optional<ToolbarItem> EditorToolbar::HitTest(POINT client_pt) const {
    if (::PtInRect(&toolbar_rect_, client_pt) == FALSE) {
        return std::nullopt;
    }
    const float x = static_cast<float>(client_pt.x - toolbar_rect_.left);
    const float y = static_cast<float>(client_pt.y - toolbar_rect_.top);
    for (const ToolbarItem& item : items_) {
        if (item.shown && x >= item.rect.left && x <= item.rect.right && y >= item.rect.top && y <= item.rect.bottom) {
            return item;
        }
    }
    return std::nullopt;
}

std::optional<ToolbarItem> EditorToolbar::Find(int identifier) const {
    const auto found = std::find_if(items_.begin(), items_.end(), [identifier](const auto& item) { return item.identifier == identifier; });
    return found != items_.end() ? std::optional<ToolbarItem>(*found) : std::nullopt;
}

void EditorToolbar::Paint(HDC dc, float scale) const {
    using namespace ui;
    RECT client{0, 0, client_width_, client_height_};
    platform::PaintBuffer buffer(dc, client);
    dc = buffer.dc();
    theme::DrawPanel(dc, client, scale);
    theme::DrawGrip(dc, {0, 0, static_cast<LONG>(ui::theme::kIslandGripWidth * scale), toolbar_rect_.bottom}, scale);
    for (const auto& split : separators_) { DrawSeparator(dc, split, scale); }
}

RECT EditorToolbar::InitialWindowRect(const RECT& screen_region, float scale) const {
    const auto monitor = platform::MonitorForRect(screen_region);
    RECT work = monitor.work_area;
    work.top = std::max(work.top, monitor.bounds.top + ui::theme::Scaled(
                                                           ui::theme::kPaletteButtonHeight + 2 * ui::theme::kPalettePadding, monitor.scale()));
    const LONG width = std::min(platform::RectWidth(work), static_cast<LONG>(kIslandWidth * scale));
    const LONG height = toolbar_rect_.bottom;
    const LONG margin = static_cast<LONG>(kCanvasGap * scale);
    const LONG left = screen_region.left + (platform::RectWidth(screen_region) - width) / 2;
    const LONG top = std::min(work.bottom - height - margin, screen_region.bottom + margin);
    return platform::ClampRectToBounds(RECT{left, top, left + width, top + height}, work);
}

}  // namespace easnap::editor
