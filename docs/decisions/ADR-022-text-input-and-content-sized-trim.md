# ADR-022 — Native text input and content-sized trim presentation

**Status:** Implemented  
**Date:** 2026-09-19  
**Amended by:** [ADR-023](ADR-023-trim-resize-and-text-click-continuations.md) (resize minimum, blank double-click continuation); [ADR-024](ADR-024-native-trim-frame.md) (native frame and painting policy)

Accepted and closed, 2026-09-21: the user accepted all session corrections,
including native text painting, selection/blank clicks, trim resize, playhead,
export progress, paused preview opening and the compact recording island.

Future window work follows the [implementation guide](../ui/window-implementation-guide.md).

## Context

Desktop review found a stationary annotation caret, an extra click when moving
between text boxes, oversized trim letterboxing/status space and a watermark
clear action that stayed revealed. This authorized correction slice continues
Phase 3 follow-up work; it does not start Phase 4.

## Decisions

- EDIT retains native selection, keyboard, clipboard, undo and caret ownership.
  Suspend its GDI redraw without removing the visibility needed by input logic.
  Native input may still draw synchronously while the HWND stays visible: clip
  that glyph/background HDC to an empty region in the annotation control-color
  handler. The separate custom source-backed paint pass alone draws annotation
  pixels; it obtains an unrestricted DC and preserves the native caret.
  Resize the annotation host after native input dispatch finishes, and position
  a collapsed caret against the rendered DirectWrite layout. The caret is
  hidden for the duration of each message that can change pixels and shown
  again after it has been positioned (v1.1): EDIT would otherwise show it at
  its own GDI position first, and Windows does not erase a caret inside an
  update region, so that column stayed inverted until the next paint. Reapply the font
  to each newly created EDIT. Enter inserts a line; Ctrl+A selects all;
  double-click selects a word and triple-click selects its paragraph.
- Clicking another text/tag during editing commits the old box and immediately
  edits the target. A blank click only ends the active edit; another blank click
  with neither active edit nor selection creates text. Single-click selects
  existing text for dragging; blank click clears that selection, even after
  repeated box switches. Re-click/double-click of the same active box
  keeps its HWND, including empty text. Removing empty boxes cannot invalidate
  the next target index. Existing frame dragging and document Undo remain.
- Initial trim dimensions derive from source aspect ratio, work area and a
  readable control width. Fonts/targets keep their DPI scale. Manual resizing
  retains the 512 x 400 client-DIP minimum established by ADR-023, with native
  outer-frame allowances from ADR-024. The video HWND fits the available
  area without distortion, so surplus space disappears before the image shrinks.
- Remove the redundant recording heading. Status uses measured wrapped text
  height and collapses when empty; export progress occupies a separate rounded
  recessed track only when needed. Its coral fill uses actual exporter progress,
  with the existing short-operation delay and native progress accessibility.
  Keep that rounded fill; do not add a second sheen stroke over its ends.
- Batch child layout, avoid synchronous whole-window region repaint during
  sizing, reuse chrome fonts until DPI changes, and preserve the video
  presenter's last pixels until its resize update instead of erasing to black.
- Watermark clear follows pointer presence, including leave and timer fallback;
  focus does not latch it open. Use the shared animated circular button material
  with a soft shadow toward the lower right and a recessed press. Undo updates
  both document and input text even when the input retains focus.

## Amendment 2026-11-17: typing and the pointer, the watermark field

Reported by the owner on v1.3.0 and located with diagnostic builds on the
owner's PC, each switch confirmed by eye:

- With "Hide pointer while typing" on, the pointer dimmed at every key in the
  Text, Tag and watermark inputs. Windows shows the pointer it hid for typing
  again at any move, resize, z-order, region or visibility change of a
  window, and two such changes came with each key: the redraw suspension
  (`WM_SETREDRAW` takes `WS_VISIBLE` away and gives it back) and the
  annotation host growing with its text. Keyboard messages to an EDIT are no
  longer suspended; the annotation EDIT's own drawing stays off the screen
  through the control colour handler, and a styled EDIT draws on the same
  background as its buffered pass, so nothing flickers (checked by the owner
  with the suspension off). The annotation host changes only what changed,
  and becomes topmost only when it appears. Its growth cannot be avoided, so
  `ui::TypingPointer` keeps the pointer hidden over Easnap's windows, by the
  thread's display count, from the first typed character until the pointer
  really moves; leaving the text, losing the focus or hiding the editor shows
  it again. Nothing is hidden while the setting is off. The pointer check
  of the owner's diagnostic sessions: hiding a window change at a time showed
  that each of the two re-showed the pointer; re-setting the cursor shape
  only when it differs did not change anything.
- In the watermark field the caret stayed on one letter once the text was
  wider than the field. A styled EDIT keeps `WS_VISIBLE` while its redraw is
  suspended (as the annotation EDIT already did), and the clear button and
  the field's margin are set only when they change. The isolated test
  desktop does not reproduce the stuck caret; the owner confirmed the
  correction.
- The property island edits the image's one watermark whenever the
  watermark tool is chosen, selected or not: after Esc its text, clear
  button and sliders acted on the tool's defaults and added a second,
  empty watermark. A click on the watermark's text does not select it: the
  watermark is a setting of the whole image, not an object (the owner's
  decision, after a trial build showed the click had no purpose).
- Esc in the watermark field reverts what was typed since the field took
  the focus, every time: after a revert, and after the clear button, the
  field opens its next edit at once although it keeps the focus. Leaving the
  field keeps its text. Writing the document's text into the field changes
  nothing in the document.

`capture_island_placement` covers the watermark field (scrolling, Esc,
clear button, sliders, return to the tool); its checks of the island fail
against the v1.3.0 code. `capture_workspace_production` and
`annotation_editing_native` check that every way out of typed text shows
the pointer again, the latter with real keyboard input and the pointer
hidden while typing where the setting is on.

## Validation boundary

Native HWND regressions cover caret movement, newlines, navigation, selection,
replacement/undo, box transitions, re-entry, double-click, Escape, hover exit,
content-sized status, progress visibility and aspect-preserving resize geometry.
Existing source-backed paint and annotation/export tests remain applicable.
Native message/pixel tests do not establish perceived motion on every GPU,
physical mixed-DPI behavior, every IME path or screen-reader acceptance.
The accepted 2026-09-21 correction additionally checks native text dispatch before
WM_PAINT across themes, modes and new/reopened editors. The measurements of
that session are in [ADR-024](ADR-024-native-trim-frame.md).

The visibility behavior is documented in Microsoft's
[WM_SETREDRAW reference](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-setredraw).
