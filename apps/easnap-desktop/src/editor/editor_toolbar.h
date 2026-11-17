// Easnap - the screenshot editor's tools island.
//
// One row (or more, when the island is narrow) of tool buttons, the Scrollshot
// button and the editor's commands, with a grip on the left. This class lays
// the items out, creates and refreshes their buttons, paints the island and
// owns the tooltip that every island's buttons share. Running a tool or a
// command stays with ScreenshotEditor, which owns the window and its messages.
#pragma once

#include <windows.h>

#include <optional>
#include <vector>

#include "editor/toolbar_item.h"

namespace easnap::editor {

/// Preferred tools island width in device-independent pixels, clamped to the
/// current monitor. One row of tools and commands, with 28 DIPs to spare
/// after the last command.
inline constexpr float kIslandWidth = 870.0f;

/// What the tools island's buttons show at one moment.
struct ToolbarStatus {
    ToolId tool{ToolId::Pointer};  ///< The tool whose button is checked.
    bool can_undo{false};          ///< Undo and Clear are enabled.
    bool can_redo{false};          ///< Redo is enabled.
    bool exporting{false};         ///< A copy or save runs: Copy, Save and Pin are disabled.
    bool has_image{false};         ///< The screenshot is fixed: Scrollshot is disabled.
};

/// The tools island's items, buttons, painting and shared tooltip.
class EditorToolbar {
public:
    /// Lays the items out in rows that fit `available_width` pixels at DPI `scale`.
    void Build(float available_width, float scale);
    /// Remembers the island's client size, at least one pixel each way.
    void Resize(int client_width, int client_height) noexcept;
    /// Client width of the island in pixels.
    [[nodiscard]] int client_width() const noexcept { return client_width_; }
    /// Height of the island's rows in pixels.
    [[nodiscard]] LONG height() const noexcept { return toolbar_rect_.bottom; }

    /// Creates the tooltip every island's buttons share, then this island's
    /// buttons in `island`. False when a button could not be made.
    bool CreateButtons(HWND island, ToolbarButtonOwner& owner);
    /// Registers the tip that explains, on `island` itself, why the disabled
    /// Scrollshot button cannot be pressed: a disabled button gets no mouse input.
    void AddScrollshotDisabledTip(HWND island);
    /// The tooltip every island's buttons share; null before CreateButtons.
    [[nodiscard]] HWND tooltip() const noexcept { return tooltip_; }
    /// Hides a shown tip and stops tips while a dialog covers the islands.
    void SuspendTooltips() const;
    /// Lets tips show again after SuspendTooltips.
    void ResumeTooltips() const;
    /// Destroys the shared tooltip; the islands' buttons go with their windows.
    void DestroyTooltip();

    /// Places, enables and checks the buttons in `island` as `status` says.
    void SyncButtons(HWND island, const ToolbarStatus& status, HFONT font) const;
    /// The item under `client_pt`, as a copy: activating it rebuilds the items.
    [[nodiscard]] std::optional<ToolbarItem> HitTest(POINT client_pt) const;
    /// The item with control identifier `identifier`, as a copy.
    [[nodiscard]] std::optional<ToolbarItem> Find(int identifier) const;
    /// Paints the island's panel, grip and separators.
    void Paint(HDC dc, float scale) const;
    /// The island's first place: centred below `screen_region`, inside its
    /// monitor's work area and below the mode island.
    [[nodiscard]] RECT InitialWindowRect(const RECT& screen_region, float scale) const;

private:
    std::vector<ToolbarItem> items_;  ///< Tools, Scrollshot and the commands, in tab order.
    std::vector<RECT> separators_;    ///< Lines between the groups, client pixels.
    HWND tooltip_{nullptr};
    int client_width_{0};   ///< Client area width in physical pixels.
    int client_height_{0};  ///< Client area height in physical pixels.
    RECT toolbar_rect_{};   ///< Toolbar strip, client pixels (docked to top).
};

}  // namespace easnap::editor
