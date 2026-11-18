// Easnap - the screenshot editor's keyboard shortcuts.
//
// One row per shortcut: its key, the modifiers it needs, whether it applies
// only to a document (an image shown through a viewport that scrolls and
// zooms) and the command it runs. The first row that matches wins, so a row
// placed earlier takes precedence. Keys whose meaning depends on the editing
// state (text being typed, a stroke or drag, the selection, Tab) are handled
// by ScreenshotEditor::OnKeyDown before this table is consulted, and the text
// field, the property island and the island buttons handle some keys of their
// own before they reach it.
#pragma once

#include <windows.h>

#include <array>
#include <optional>

namespace easnap::editor {

/// What a keyboard shortcut does.
enum class KeyCommand {
    ScrollPageUp,       ///< Scrolls the document up by most of the viewport.
    ScrollPageDown,     ///< Scrolls the document down by most of the viewport.
    ScrollToTop,        ///< Scrolls to the top of the document.
    ScrollToBottom,     ///< Scrolls to the bottom of the document.
    ScrollUp,           ///< Scrolls the document up by a line.
    ScrollDown,         ///< Scrolls the document down by a line.
    ScrollLeft,         ///< Scrolls the document left by a line.
    ScrollRight,        ///< Scrolls the document right by a line.
    ZoomIn,             ///< One zoom step in, around the viewport's centre.
    ZoomOut,            ///< One zoom step out, around the viewport's centre.
    ZoomToFitWidth,     ///< The zoom at which the document fills the view's width.
    ZoomToActualSize,   ///< One image pixel per desktop pixel.
    Undo,               ///< Clears the selection and undoes the last change.
    Redo,               ///< Clears the selection and redoes the last undone change.
    Copy,               ///< Copies the finished image and closes.
    Save,               ///< Saves the finished image.
    Pin,                ///< Pins the finished image to the screen and closes.
    Discard,            ///< Closes, asking first when work would be lost.
    None,               ///< No command: the default of a row, which every row must replace.
};

/// Whether a modifier key must be held, must not be held, or does not matter.
enum class KeyModifier {
    Any,      ///< Held or not.
    Held,     ///< Must be held.
    NotHeld,  ///< Must not be held.
};

/// One keyboard shortcut. Alt is never looked at.
struct KeyBinding {
    WPARAM key;                                ///< The virtual key.
    KeyModifier control{KeyModifier::Any};     ///< What Ctrl must be.
    KeyModifier shift{KeyModifier::Any};       ///< What Shift must be.
    bool document_only{false};                 ///< Applies only while a document is shown.
    KeyCommand command{KeyCommand::None};      ///< What it does.
};

/// Every shortcut, in order of precedence.
inline constexpr auto kKeyBindings = std::to_array<KeyBinding>({
    // A document larger than its viewport is moved and scaled from the
    // keyboard as well as with the wheel.
    {.key = VK_PRIOR, .document_only = true, .command = KeyCommand::ScrollPageUp},
    {.key = VK_NEXT, .document_only = true, .command = KeyCommand::ScrollPageDown},
    {.key = VK_HOME, .document_only = true, .command = KeyCommand::ScrollToTop},
    {.key = VK_END, .document_only = true, .command = KeyCommand::ScrollToBottom},
    {.key = VK_UP, .document_only = true, .command = KeyCommand::ScrollUp},
    {.key = VK_DOWN, .document_only = true, .command = KeyCommand::ScrollDown},
    {.key = VK_LEFT, .document_only = true, .command = KeyCommand::ScrollLeft},
    {.key = VK_RIGHT, .document_only = true, .command = KeyCommand::ScrollRight},
    {.key = VK_OEM_PLUS, .control = KeyModifier::Held, .document_only = true, .command = KeyCommand::ZoomIn},
    {.key = VK_ADD, .control = KeyModifier::Held, .document_only = true, .command = KeyCommand::ZoomIn},
    {.key = VK_OEM_MINUS, .control = KeyModifier::Held, .document_only = true, .command = KeyCommand::ZoomOut},
    {.key = VK_SUBTRACT, .control = KeyModifier::Held, .document_only = true, .command = KeyCommand::ZoomOut},
    {.key = '0', .control = KeyModifier::Held, .document_only = true, .command = KeyCommand::ZoomToFitWidth},
    {.key = VK_NUMPAD0, .control = KeyModifier::Held, .document_only = true, .command = KeyCommand::ZoomToFitWidth},
    {.key = '1', .control = KeyModifier::Held, .document_only = true, .command = KeyCommand::ZoomToActualSize},
    {.key = VK_NUMPAD1, .control = KeyModifier::Held, .document_only = true, .command = KeyCommand::ZoomToActualSize},
    {.key = 'Z', .control = KeyModifier::Held, .shift = KeyModifier::NotHeld, .command = KeyCommand::Undo},
    {.key = 'Z', .control = KeyModifier::Held, .shift = KeyModifier::Held, .command = KeyCommand::Redo},
    {.key = 'Y', .control = KeyModifier::Held, .command = KeyCommand::Redo},
    {.key = 'C', .control = KeyModifier::Held, .command = KeyCommand::Copy},
    {.key = 'S', .control = KeyModifier::Held, .command = KeyCommand::Save},
    {.key = 'T', .control = KeyModifier::Held, .command = KeyCommand::Pin},
    {.key = VK_ESCAPE, .command = KeyCommand::Discard},
});

/// True when a modifier that is `held` satisfies `wanted`.
[[nodiscard]] constexpr bool ModifierMatches(KeyModifier wanted, bool held) noexcept {
    return wanted == KeyModifier::Any || (wanted == KeyModifier::Held) == held;
}

/// The command of the first binding of `key` that the modifiers and the
/// document mode satisfy; nothing when no binding does.
[[nodiscard]] constexpr std::optional<KeyCommand> FindKeyCommand(WPARAM key, bool control, bool shift, bool document_mode) noexcept {
    for (const auto& binding : kKeyBindings) {
        if (binding.key == key && ModifierMatches(binding.control, control) && ModifierMatches(binding.shift, shift) &&
            (!binding.document_only || document_mode)) {
            return binding.command;
        }
    }
    return std::nullopt;
}

/// True when every row names a command.
[[nodiscard]] constexpr bool EveryKeyBindingHasCommand() noexcept {
    for (const auto& binding : kKeyBindings) {
        if (binding.command == KeyCommand::None) { return false; }
    }
    return true;
}
static_assert(EveryKeyBindingHasCommand(), "a key binding needs a command");

// Precedence that the rows above must keep.
static_assert(FindKeyCommand('Z', true, false, false) == KeyCommand::Undo);
static_assert(FindKeyCommand('Z', true, true, false) == KeyCommand::Redo);
static_assert(FindKeyCommand('Y', true, true, false) == KeyCommand::Redo, "Shift is not looked at on Ctrl+Y");
static_assert(!FindKeyCommand('0', true, false, false), "zoom keys apply only to a document");
static_assert(FindKeyCommand(VK_PRIOR, true, true, true) == KeyCommand::ScrollPageUp, "scroll keys ignore the modifiers");
static_assert(FindKeyCommand(VK_ESCAPE, true, true, false) == KeyCommand::Discard);

}  // namespace easnap::editor
