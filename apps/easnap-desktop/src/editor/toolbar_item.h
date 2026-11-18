// Easnap - the buttons of the screenshot editor's islands.
//
// The tools island (editor/editor_toolbar.h), the property island and its
// brush size pop-up (editor/editor_context_panel.h) are rows of the same kind
// of button: a tool, an option, a colour, a thickness, a brush size or a
// command. This header holds that item, the metrics and the row flow both
// islands lay their items out with, and the native button every item becomes.
#pragma once

#include <windows.h>

#include <array>

#include <d2d1.h>

#include "editor/annotation_document.h"
#include "ui/icons.h"
#include "ui/native_controls.h"

namespace easnap::editor {

// Island metrics in device-independent pixels.
/// Height of one row of an island.
inline constexpr float kToolbarRowHeight = 48.0f;
/// Width and height of a tool or option button.
inline constexpr float kToolButton = 34.0f;
/// Width and height of a command button.
inline constexpr float kCommandButton = 34.0f;
/// Width and height of a colour swatch.
inline constexpr float kSwatch = 32.0f;
/// Space between two neighbouring items of a row.
inline constexpr float kItemGap = 4.0f;
/// Space that separates the tools from Scrollshot on the tools island.
inline constexpr float kGroupGap = 14.0f;
/// Space between an island's edge and its first item.
inline constexpr float kEdgePadding = 10.0f;

/// Line thicknesses the property island offers, in captured pixels.
inline constexpr std::array<float, 3> kThicknessOptions = {2.0f, 4.0f, 7.0f};
/// Mosaic brush diameters in captured pixels (independent of the preview zoom).
inline constexpr std::array<float, 3> kBrushSizeOptions = {16.0f, 32.0f, 64.0f};

/// The editor's commands. The button of command `n` is `CommandButton(n)`.
enum CommandId {
    kCommandUndo = 1,
    kCommandRedo,
    kCommandClear,
    kCommandCopy,
    kCommandSave,
    kCommandCancel,
    kCommandTheme,
    kCommandScrollshot,
    kCommandPin,
};

/// One button of an island: what it chooses or runs and where it goes.
struct ToolbarItem {
    /// What pressing the button does.
    enum class Kind { Tool,
                      Color,
                      Thickness,
                      BrushSize,
                      Option,
                      Command };

    Kind kind{Kind::Tool};             ///< What pressing it does.
    ToolId tool{ToolId::Pointer};      ///< The tool a Tool item chooses.
    D2D1_COLOR_F color{};              ///< The colour a Color item chooses.
    float thickness{0.0f};             ///< The line thickness a Thickness item chooses.
    float brush_size{0.0f};            ///< The diameter a BrushSize item chooses.
    int command{0};                    ///< The CommandId a Command item runs.
    const wchar_t* label{L""};         ///< Its name; the button takes its text from its look.
    const wchar_t* tooltip{L""};       ///< Its tooltip before its look replaces it.
    D2D1_RECT_F rect{};                ///< Its place in its island's client pixels.
    int identifier{};                  ///< Its control identifier (ui/control_ids.h).
    bool shown{true};                  ///< False while its island's tool does not offer it.
};

/// How one tool, option or command button of the editor looks: the same for
/// every session, so it is fixed in one table rather than where the button is placed.
struct EditorButtonLook {
    int identifier;                                 ///< The button's control identifier.
    ui::Icon icon;                                  ///< The icon it shows.
    UINT tooltip;                                   ///< String id of its tooltip and accessible name.
    ui::ButtonRole role{ui::ButtonRole::Standard};  ///< Its semantic role.
    bool dropdown{false};                           ///< True when it shows a trailing chevron.
};

/// The look of a tool, option or command button: a tool's from its record
/// (editor/annotation_tools.h), the others' from the editor's table.
[[nodiscard]] EditorButtonLook ButtonLook(int identifier) noexcept;

/// Places an island's items left to right in rows, starting a new row before
/// an item that would pass the right edge. Positions are in client pixels.
struct ToolbarFlow {
    /// A flow for an island `available_width` pixels wide at DPI `scale`,
    /// starting at the left padding of the first row.
    ToolbarFlow(float available_width, float scale) noexcept;
    /// The next item, `width` pixels wide, centred vertically in its row.
    D2D1_RECT_F Place(float width) noexcept;
    /// Takes the next `width` pixels as a gap and returns the one-pixel
    /// vertical separator line drawn in its middle.
    RECT Separator(float width) noexcept;

    float s;      ///< DPI scale.
    float pad;    ///< Edge padding in pixels.
    float gap;    ///< Gap between items in pixels.
    float row;    ///< Row height in pixels.
    float limit;  ///< Right edge an item may not pass, unless it starts a row.
    float x;      ///< Left edge of the next item.
    float y{0};   ///< Top of the current row.
};

/// What the buttons of every island ask of the editor that owns them.
class ToolbarButtonOwner {
public:
    /// Fixes the screenshot on the first press of a control. False when the
    /// image could not be acquired; the press is then dropped.
    virtual bool FeatureUsed() = 0;
    /// Handles Esc or a Ctrl shortcut pressed on a button as the tools
    /// island itself does.
    virtual LRESULT ForwardButtonKey(UINT message, WPARAM key, LPARAM flags) = 0;
    /// The pointer moves over the brush method button: the brush size pop-up
    /// opens once it rests there.
    virtual void BrushMethodHovered() = 0;

protected:
    ~ToolbarButtonOwner() = default;
};

/// Creates the button of `item` in `parent`, styled after its look, sending
/// its input through `owner`, and registers its tooltip with `tooltip` when
/// there is one. False when the button could not be made.
[[nodiscard]] bool CreateToolbarButton(const ToolbarItem& item, HWND parent, HWND tooltip, ToolbarButtonOwner& owner);
/// Shows or hides `button` as `item` says, moves it to the item's rectangle
/// and gives it `font`; each only when it changes.
void PlaceToolbarButton(HWND button, const ToolbarItem& item, HFONT font);
/// Enables or disables `button` and checks an option, tool or swatch button
/// that is `selected`; each only when it changes.
void MarkToolbarButton(HWND button, const ToolbarItem& item, bool selected, bool enabled);

}  // namespace easnap::editor
