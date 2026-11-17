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

/// One annotation tool as the tools island shows it.
struct ToolDefinition {
    ToolId id;              ///< The tool.
    int identifier;         ///< Its button's control identifier: kToolButtons.Id(int(id)).
    int toolbar_position;   ///< Its place on the tools island, 0 for the leftmost.
    bool separator_before;  ///< A separator stands before it on the tools island.
    UINT name;              ///< String id of its tooltip and accessible name.
    ui::Icon icon;          ///< The icon its button shows.
};

/// How many tools there are.
inline constexpr std::size_t kToolCount = static_cast<std::size_t>(ToolId::Watermark) + 1;

/// Every tool, in ToolId order.
inline constexpr std::array<ToolDefinition, kToolCount> kToolDefinitions{{
    {ToolId::Pointer, kToolButtons.Id(0), 0, false, IDS_EDITOR_TOOL_POINTER, ui::Icon::Cursor},
    {ToolId::Pen, kToolButtons.Id(1), 5, false, IDS_EDITOR_TOOL_PEN, ui::Icon::Pen},
    {ToolId::Arrow, kToolButtons.Id(2), 4, false, IDS_EDITOR_TOOL_ARROW, ui::Icon::Arrow},
    {ToolId::Rectangle, kToolButtons.Id(3), 1, false, IDS_EDITOR_TOOL_RECTANGLE, ui::Icon::Rectangle},
    {ToolId::Ellipse, kToolButtons.Id(4), 2, false, IDS_EDITOR_TOOL_ELLIPSE, ui::Icon::Ellipse},
    {ToolId::Text, kToolButtons.Id(5), 6, false, IDS_EDITOR_TOOL_TEXT, ui::Icon::Text},
    {ToolId::Mosaic, kToolButtons.Id(6), 8, true, IDS_EDITOR_TOOL_MOSAIC, ui::Icon::Mosaic},
    {ToolId::Line, kToolButtons.Id(7), 3, false, IDS_EDITOR_TOOL_LINE, ui::Icon::Line},
    {ToolId::Tag, kToolButtons.Id(8), 7, false, IDS_EDITOR_TOOL_TAG, ui::Icon::Tag},
    {ToolId::Spotlight, kToolButtons.Id(9), 9, false, IDS_EDITOR_TOOL_SPOTLIGHT, ui::Icon::Spotlight},
    {ToolId::Watermark, kToolButtons.Id(10), 10, false, IDS_EDITOR_TOOL_WATERMARK, ui::Icon::Watermark},
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

}  // namespace easnap::editor
