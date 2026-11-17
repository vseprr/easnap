// Easnap - the layout of the screenshot editor's property island.
//
// What the property island shows for a tool (editor/annotation_tools.h,
// ToolProperties), placed in rows that wrap in the width of the tools island:
// option buttons, value labels and sliders, the font size box, the watermark
// field, thicknesses, colours, and the brush sizes of the pop-up.
#pragma once

#include <windows.h>

#include <utility>
#include <vector>

#include "editor/annotation_document.h"
#include "editor/toolbar_item.h"

namespace easnap::editor {

/// Lays out the property island of `tool` for an island `available_width`
/// pixels wide at DPI `scale`, measuring value labels in `font`. Appends its
/// item buttons to `items`, its other controls (identifier and client
/// rectangle) to `fields` and its separator lines to `separators`. Returns its
/// height (0 for the pointer, which has none) and its widest row, in pixels.
[[nodiscard]] std::pair<float, float> LayOutContextPanel(ToolId tool, float available_width, float scale, HFONT font,
                                                         std::vector<ToolbarItem>& items, std::vector<std::pair<int, RECT>>& fields,
                                                         std::vector<RECT>& separators);

}  // namespace easnap::editor
