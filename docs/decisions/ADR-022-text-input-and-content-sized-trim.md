# ADR-022: Native text input and content-sized trim presentation

Date: 2026-09-19  
Status: Implemented; validation is recorded in project status.

Accepted and closed, 2026-09-21: the user accepted all session corrections,
including native text painting, selection/blank clicks, trim resize, playhead,
export progress, paused preview opening and the compact recording island.

Resize constraints and blank double-click continuations are amended by
[ADR-023](ADR-023-trim-resize-and-text-click-continuations.md); the final native
frame/painting policy is [ADR-024](ADR-024-native-trim-frame.md).
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
  a collapsed caret against the rendered DirectWrite layout. Reapply the font
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
  retains the 480 x 400 client-DIP minimum established by ADR-023, with native
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

## Validation boundary

Native HWND regressions cover caret movement, newlines, navigation, selection,
replacement/undo, box transitions, re-entry, double-click, Escape, hover exit,
content-sized status, progress visibility and aspect-preserving resize geometry.
Existing source-backed paint and annotation/export tests remain applicable.
Native message/pixel tests do not establish perceived motion on every GPU,
physical mixed-DPI behavior, every IME path or screen-reader acceptance.
The accepted 2026-09-21 correction additionally checks native text dispatch before
WM_PAINT across themes, modes and new/reopened editors. Its dated build/test
results and final manual acceptance are in [text/trim validation](../ui/text-trim-validation.md).

The visibility behavior is documented in Microsoft's
[WM_SETREDRAW reference](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-setredraw).
