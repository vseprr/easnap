# ADR-018: Direct annotation manipulation and quiet capture chrome

Date: 2026-09-17  
Status: Implemented; physical interaction review remains.

## Context

The user's correction notes and fifteen screenshots specify interaction concepts,
not a new palette. Keep ADR-017's Light/Dark neumorphic materials. This bounded
slice amends its selection, text editing, compact controls and fixation behavior;
it does not start Phase 4.

## Decisions

- Drawing finishes unselected. Object hit testing works with every tool; hover
  exposes movement, clicking selects, and dragging moves. Handles only appear
  after selection. Selection traces follow actual shapes/paths. Text and tags
  have hollow rounded coral frames and no stretch handles.
- The first outside text click commits/deselects without another object. Native
  EDIT retains caret, selection, IME/input and clipboard ownership; its paint
  draws the same immutable-source/vector composition as the canvas. Selected
  text ranges use DirectWrite hit-test geometry; native caret remains native.
  Double-click resumes the object; the frame can be dragged while editing.
- Selected controls keep material depth -1 and coral ink without a coral outline.
  Neutral keyboard underlines preserve focus indication. Native sliders and the
  trim timeline paint through buffers. Style changes do not reposition islands,
  and repaint suppression never reveals an intentionally hidden slider.
- Main/context/mode/recording islands have rounded native regions. Main tools
  are icons only, normally one 48 DIP row at 780 DIP preferred width. White
  separators group controls. Tutorials and the duplicate size footer disappear;
  capture, save, clipboard and input failures remain visible as transient notices.
- Text mode icons and an owner-drawn pt preset list retain custom 6–288 pt entry.
  Blur uses mosaic/drop, hand/chevron and region icons plus one strength slider.
  Brush sizes open on click/350 ms hover and close on selection, drawing, tool
  change or 250 ms leave. Spotlight uses three shape icons and one opacity slider.
- Watermark input is limited to 16 UTF-16 units without splitting a surrogate
  pair. Its cue is empty-input only. A rotated lattice anchored at image center
  centers each tile's measured bounds; text/size updates grow symmetrically.
- An alpha-1 input window keeps desktop hit testing; a separate clipped shading
  window darkens only outside the selection. No frozen image is painted inside
  provisional frames. The input plane has at most one 8-bit level of compositing
  quantization; fixed image/export pixels are unaffected.
- All overlay surfaces request capture exclusion. On Windows 10 build 19041+
  the coordinator checks every visible app window before keeping the workspace
  visible during acquisition. Otherwise it retains hide/flush/capture/show.
  The opaque image is painted before presentation. The mode bar keeps its
  appearance while becoming disabled and fades for 500 ms.

The capture-exclusion compatibility gate follows Microsoft's
[SetWindowDisplayAffinity documentation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity).

## Validation boundary

Native tests cover document operations, actual caret ownership, source-backed
text painting, frame dragging, popup hover/click/leave, pt validation, hidden
slider state, capture exclusion with changing real desktop-window pixels,
500 ms fade, Light/Dark rendering, UI Automation, DPI messages and undo/export.
See [correction validation](../ui/interaction-corrections-validation.md) for exact
runs. Native render captures and message tests do not certify perceived motion
smoothness, physical mixed-DPI behavior, every IME path or screen-reader UX.
