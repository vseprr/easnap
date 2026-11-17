// Easnap - what the screenshot editor knows about each annotation tool.
//
// One record per tool: its identity, its place on the tools island and how
// its button looks. Adding a tool means adding its ToolId, its record here,
// and handling it where a decision genuinely differs per tool (drawing, hit
// testing, handles); those are found by searching for ToolId.
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>

#include "editor/annotation_document.h"
#include "string_ids.h"
#include "ui/control_ids.h"
#include "ui/icons.h"

namespace easnap::editor {

/// One annotation tool: how the tools island shows it and the decisions that
/// depend only on the tool.
struct ToolDefinition {
    ToolId id;                     ///< The tool.
    int identifier;                ///< Its button's control identifier: kToolButtons.Id(int(id)).
    int toolbar_position;          ///< Its place on the tools island, 0 for the leftmost.
    bool separator_before{false};  ///< A separator stands before it on the tools island.
    UINT name;                     ///< String id of its tooltip and accessible name.
    ui::Icon icon;                 ///< The icon its button shows.
    /// Its annotation is text typed into a box (Text, Tag): a press places the
    /// text field, a double-click edits it, and its box is its text layout.
    bool text_box{false};
    /// Its annotation changes the image pixels themselves (Mosaic, Spotlight):
    /// it is applied to the base image, never drawn over it.
    bool base_effect{false};
};

/// How many tools there are.
inline constexpr std::size_t kToolCount = static_cast<std::size_t>(ToolId::Watermark) + 1;

/// Every tool, in ToolId order.
inline constexpr std::array<ToolDefinition, kToolCount> kToolDefinitions{{
    {.id = ToolId::Pointer, .identifier = kToolButtons.Id(0), .toolbar_position = 0, .name = IDS_EDITOR_TOOL_POINTER, .icon = ui::Icon::Cursor},
    {.id = ToolId::Pen, .identifier = kToolButtons.Id(1), .toolbar_position = 5, .name = IDS_EDITOR_TOOL_PEN, .icon = ui::Icon::Pen},
    {.id = ToolId::Arrow, .identifier = kToolButtons.Id(2), .toolbar_position = 4, .name = IDS_EDITOR_TOOL_ARROW, .icon = ui::Icon::Arrow},
    {.id = ToolId::Rectangle, .identifier = kToolButtons.Id(3), .toolbar_position = 1, .name = IDS_EDITOR_TOOL_RECTANGLE, .icon = ui::Icon::Rectangle},
    {.id = ToolId::Ellipse, .identifier = kToolButtons.Id(4), .toolbar_position = 2, .name = IDS_EDITOR_TOOL_ELLIPSE, .icon = ui::Icon::Ellipse},
    {.id = ToolId::Text, .identifier = kToolButtons.Id(5), .toolbar_position = 6, .name = IDS_EDITOR_TOOL_TEXT, .icon = ui::Icon::Text, .text_box = true},
    {.id = ToolId::Mosaic, .identifier = kToolButtons.Id(6), .toolbar_position = 8, .separator_before = true, .name = IDS_EDITOR_TOOL_MOSAIC, .icon = ui::Icon::Mosaic, .base_effect = true},
    {.id = ToolId::Line, .identifier = kToolButtons.Id(7), .toolbar_position = 3, .name = IDS_EDITOR_TOOL_LINE, .icon = ui::Icon::Line},
    {.id = ToolId::Tag, .identifier = kToolButtons.Id(8), .toolbar_position = 7, .name = IDS_EDITOR_TOOL_TAG, .icon = ui::Icon::Tag, .text_box = true},
    {.id = ToolId::Spotlight, .identifier = kToolButtons.Id(9), .toolbar_position = 9, .name = IDS_EDITOR_TOOL_SPOTLIGHT, .icon = ui::Icon::Spotlight, .base_effect = true},
    {.id = ToolId::Watermark, .identifier = kToolButtons.Id(10), .toolbar_position = 10, .name = IDS_EDITOR_TOOL_WATERMARK, .icon = ui::Icon::Watermark},
}};

/// The record of `tool`.
[[nodiscard]] constexpr const ToolDefinition& Tool(ToolId tool) noexcept {
    return kToolDefinitions[static_cast<std::size_t>(tool)];
}

/// True when the table is in ToolId order, each identifier is the tool button
/// base plus the tool's value, and the toolbar positions are 0..count-1 once each.
[[nodiscard]] constexpr bool ToolDefinitionsAreConsistent() noexcept {
    std::array<bool, kToolCount> placed{};
    for (std::size_t i = 0; i < kToolDefinitions.size(); ++i) {
        const ToolDefinition& tool = kToolDefinitions[i];
        if (static_cast<std::size_t>(tool.id) != i || tool.identifier != kToolButtons.first + static_cast<int>(tool.id)) {
            return false;
        }
        const auto position = static_cast<std::size_t>(tool.toolbar_position);
        if (tool.toolbar_position < 0 || position >= kToolCount || placed[position]) { return false; }
        placed[position] = true;
    }
    return true;
}
static_assert(ToolDefinitionsAreConsistent(), "one tool record per ToolId, in ToolId order, each with its own place");
static_assert(kToolButtons.count == static_cast<int>(kToolCount), "one tool button per ToolId");

/// The tools in the order the tools island shows them, left to right.
inline constexpr std::array<ToolId, kToolCount> kToolbarOrder = [] {
    std::array<ToolId, kToolCount> order{};
    for (const ToolDefinition& tool : kToolDefinitions) { order[static_cast<std::size_t>(tool.toolbar_position)] = tool.id; }
    return order;
}();

/// True for the tools whose annotation is typed text in a box: Text and Tag.
[[nodiscard]] constexpr bool IsTextTool(ToolId tool) noexcept { return Tool(tool).text_box; }

/// True for the tools that change the image pixels: Mosaic and Spotlight.
[[nodiscard]] constexpr bool IsBaseEffect(ToolId tool) noexcept { return Tool(tool).base_effect; }

/// True when `annotation` is a stroke that follows the pointer point by point:
/// the pen, and the mosaic with the brush method.
[[nodiscard]] inline bool IsFreehand(const Annotation& annotation) noexcept {
    return annotation.tool == ToolId::Pen || (annotation.tool == ToolId::Mosaic && annotation.style.method == EffectMethod::Brush);
}

}  // namespace easnap::editor
