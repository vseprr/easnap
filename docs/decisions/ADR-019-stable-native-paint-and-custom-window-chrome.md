# ADR-019: Stable native painting and custom window chrome

**Status:** Implemented, 2026-09-17; amended by the user-accepted 2026-09-21 correction baseline.
Resizable chrome follows [ADR-024](ADR-024-native-trim-frame.md); annotation EDIT follows [ADR-022](ADR-022-text-input-and-content-sized-trim.md).
New windows must follow the [implementation guide](../ui/window-implementation-guide.md).

## Context

The user reported foreign rectangles during button presses and capture fixation,
ghost native glyphs during annotation typing, repeated chrome repaint during
movement, and Windows-owned outer frames on the trim and discard windows.
The existing Light/Dark neumorphic material remains the visual authority.

## Decisions

- Keep real BUTTON, EDIT, STATIC, COMBOBOX and dialog semantics. Suspend native
  synchronous painting around the state-changing messages that actually draw,
  then invalidate for the custom paint pass. Radio activation itself stays
  visible: hiding its outer input dispatch breaks native sibling enumeration.
  Nested suspension and explicit visibility changes are preserved.
- Buffer complete panels, controls, native labels and text edits. Annotation
  movement invalidates the canvas without repainting the unchanged tools island.
  Standard print requests route through the same custom client painter.
- A capture image window is first shown at alpha zero, then explicitly painted
  through its allocated window DC before becoming opaque. A hidden-window DC
  does not reliably populate the initial redirected surface. Retain the
  redirected layered-window path and display-affinity exclusion; direct layered
  publication did not retain the same affinity behavior on the tested Windows 10.
- Annotation movement uses unrestricted image coordinates. Preview, export,
  selection decoration and the native text host are clipped to the source.
  Fully off-canvas effects do not modify the source. Undo retains the object.
- A full-monitor selection centers the editor group or recording configuration.
  Existing placement relative to dragged regions remains. Recording tolerates
  the one-pixel adjustment needed for encoder-compatible dimensions.
- Shared custom chrome supplies the caption and buttons. For resizable windows,
  ADR-024 preserves native side/bottom insets and nonclient painting instead of
  removing the whole frame; fixed dialogs retain their dedicated shared path.
  Trim retains native resize, move, minimize, maximize, system close and keyboard
  behavior. Confirmation remains a modal native dialog with real default/cancel
  actions; closing the discard dialog means Keep recording.
- Primary actions use coral ink on neutral material. Screenshot boundaries use
  pastel khaki; recording boundaries use coral. Paused time is refreshed on every
  transition, and Pause/Resume uses a latched recessed native check state.

## Evidence and limits

See [validation](../ui/stability-corrections-validation.md). Tests read already
presented button pixels before pumping WM_PAINT, compare native typing with the
production renderer, inspect the first composed full-screen pixel, preserve
capture exclusion, and exercise actual dialog/trim HWNDs and UI Automation.
These checks do not establish motion quality on every GPU, physical DPI setup,
IME or screen reader.

The redraw guard follows the documented [WM_SETREDRAW visibility behavior](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-setredraw).
Layered-window initialization retains the documented [SetLayeredWindowAttributes path](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setlayeredwindowattributes).
