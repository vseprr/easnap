# ADR-029 — Editing an image larger than the screen

**Status:** Accepted
**Date:** 2026-10-13
**Amended:** 2026-11-09 — a pointer move repaints only what it changed

## Context

A stitched Scrollshot can be 32,000 rows tall. The screenshot editor was
built on one assumption: the image is the frame on screen, one image pixel per
desktop pixel, at a fixed place. Pointer mapping, hit distances, the native
text host, the caret, invalidation, the Direct2D base bitmap and the export
all used the frame rectangle directly.

The stitched image had to be editable with the same tools, and a second editor
or a new kind of window would have duplicated the code that
[ADR-003](ADR-003-editor-window-and-export-model.md) and the window
implementation guide tell contributors to reuse.

## Decision

### One editor, one mapping

`ScreenshotEditor::OpenDocument` puts the existing editor into document mode.
The workspace's image surface becomes a viewport onto the image
(`OverlayController::BeginDocumentView`, `SetDocumentView`). No window class
and no rendering path was added.

`editor/image_viewport.*` defines `ImageView`: the desktop rectangle that
shows the image, the zoom, and the image pixel at its top-left corner. It is
pure geometry and has its own unit test. Every conversion between desktop and
image coordinates in the editor goes through it: pointer positions, the hit
distance of handles and outlines (a screen distance, so it is divided by the
zoom), the placement and scale of the native text host, the caret, selection
handles and damage rectangles. A plain screenshot is the trivial view: zoom 1,
no scrolling, viewport equal to the image.

The origin of the image is kept on whole desktop pixels, so an image shown at
100% is never resampled.

### The view

The image opens fitted to its width, never above 100%, from the top. The
viewport is as large as the scaled image, limited to an area inside the work
area of the frame's monitor: a 12 DIP margin, and 176 DIP kept free below for
the tools island, its context island and the brush pop-up. It starts at the
frame's left edge when the area allows.

Zoom steps: 10, 12.5, 25, 33, 50, 67, 75, 100, 125, 150, 200, 300 and 400%.
Zooming with the wheel keeps the image point under the pointer in place.
Reduced views are filtered; enlarged views keep their pixels sharp.

The workspace draws two indicators on its chrome: a label with the image size
and the zoom, and a thumb on the right edge with the position.

### Rendering in bands

Direct2D bitmaps have a maximum size. An image of up to 8,192 rows is one
bitmap. A taller one keeps a band of 4,096 rows, centred on the rows on show
and uploaded again when the view leaves it. Redaction updates write into the
band when they touch it.

### Export is the whole image

Copy and Save never export the view. An image of up to 8,192 rows is
composited in one pass, as before. A taller one is composited in WIC render
targets of 2,048 rows, each drawing the same annotation list translated to its
rows. The banded result is identical to a single pass in every pixel,
including shapes and blurred regions that cross band edges; a test compares
the two.

### Leaving document mode

The Scrollshot button is disabled on a stitched image. `CompositeCommitted`
returns the image with its annotations drawn in, for the return to
`ScrollshotReview` after a display change. `OpenDocument` takes the image only
when the editor opened, and `TakeImage` hands it back, so no failure path
drops it.

### A move repaints what it changed (amended 2026-11-09)

Moving an annotation and drawing a spotlight invalidated the whole viewport
on every pointer move, so each move repainted the picture, every annotation
and, with a watermark, every tile of the watermark across the whole image.
Now a move invalidates only what it changed, in image pixels:

- `ScreenshotEditor::AnnotationDamage` gives what showing an annotation can
  touch: its bounds, the stroke pad the shape previews already used, an em
  for text, the selection frame and handles when selected (their reach is
  taken from the constants `DrawSelection` draws them with; they keep their
  size on screen, so the pad grows as the zoom falls) and one screen pixel of
  antialiasing. A move invalidates it before and after the change. The em is
  an assumption about how far glyphs overhang their layout box; asking
  DirectWrite (`GetOverhangMetrics`) would make it exact and is left for
  later.
- Pixel effects are not recomputed during input. The paint recomputes them
  (`RebuildRedactedBase`, as before), and the changed rectangle the
  redaction renderer reports is now also invalidated wherever the paint in
  progress does not cover it, so a second paint shows the rest. That paint
  finds nothing left to recompute, so it cannot repeat. Every changed effect
  pixel thus reaches the screen whatever invalidated the paint: when a
  spotlight switches the darkening on or off (its first frame, a cancelled
  preview) the whole viewport follows; a blur that reads what a moved effect
  writes is repainted as far as it is recomputed. A call outside a paint
  (`Composite`) invalidates all of the changed rectangle. Recomputing during
  input instead, as a first version of this amendment did, kept a costly
  recomputation (a tall blur, about 100 ms) in every pointer move, and
  Windows only paints when no input is waiting.
- Below 100 % the picture is filtered: a changed image pixel also invalidates
  one image pixel around it.
- A watermark draws only the tiles that can reach what the render target
  shows, found by turning the target's area back by the tiles' rotation, with
  an em of margin for overhanging glyphs, and in the order of the full grid.
  This bounds both a partial repaint and each 2,048-row band of an export.
- The toolbar is brought up to date when the gesture ends, not on every move.
  The end of a gesture, the keyboard nudge, undo and redo and a style change
  still invalidate the whole viewport.

The same rule ends a transient that a pixelating brush showed before: each
dab that moves the stroke's top-left corner moves the grid its blocks are
aligned to, so the whole stroke changes, while only the newest segment was
invalidated; until the button was released, the rest of the stroke showed
the old blocks (measured below).

Measured with `scrollshot_edit_performance` on the development PC (1920 ×
1080 display at 125 %, image 1,920 × 8,400; viewport 1,890 × 845 at the
fitted 98.4 %, 1,287 × 845 at 67 % and 192 × 840 at 10 %). Each move is
followed by the paints it asks for until nothing is left invalid. The cells
give the median time per move for input and paints together, before → after
(ranges over three alternating runs of each build), and the share of the
viewport repainted per move after; before, every move repainted all of it.
The development PC was shared with other work, so the ranges are wide.

| Gesture, 120 moves | Fitted | 67 % | 10 % |
|---|---|---|---|
| Drag a rectangle | 3.1–3.5 → 0.43–0.69 ms, 4.2 % | 2.2–3.7 → 0.39–0.59 ms, 3.7 % | 4.7–7.3 → 0.31–0.35 ms, 3.6 % |
| Spotlight preview, first frame | 44–59 → 44–74 ms, 100 % | 43–68 → 44–71 ms, 100 % | 45–72 → 43–46 ms, 100 % |
| Spotlight preview, later frames | 3.1–3.7 → 0.33–0.48 ms, 1.1 % | 2.2–3.7 → 0.31–0.47 ms, 0.9 % | 5.1–8.0 → 0.65–0.73 ms, 1.1 % |
| Drag a spotlight | 3.2–3.7 → 0.58–0.96 ms, 5.0 % | 2.4–4.1 → 0.58–0.95 ms, 4.5 % | 6.0–10.1 → 1.6–1.7 ms, 4.3 % |
| Drag a pixelated region beside a blur 6,300 rows tall | 103–113 → 100–171 ms, 46 % | 102–172 → 100–124 ms, 46 % | 103–170 → 102–114 ms, 36 % |
| Drag a rectangle over a watermark | 7.5–13.5 → 0.52–0.88 ms, 4.2 % | 6.6–12.3 → 0.46–0.51 ms, 3.7 % | 9.3–16.8 → 0.50–0.52 ms, 3.6 % |

After the change a pointer move is handled in 1–3 µs in every case (the
press that starts a spotlight in under 1 ms): the first spotlight frame and
the blur beside the dragged region cost what they cost in the paint, one paint per move plus one more for what the blur
changed outside the region. The test pairs paints with moves; it cannot
show how Windows interleaves them. That painting no longer waits for the
input to stop follows from input handling no longer doing the work.

The export and a full repaint of the viewport have the same fingerprint
before and after in all fifteen measured cases.

The test compares the partly repainted picture with a full repaint after
the 60th move of each drag, after the first spotlight frame and after each
gesture. At 10 %, where every screen pixel samples halfway between image
pixels, they are equal, except for 8 pixels under the watermark that differ
by up to 7 levels: rotated glyphs vary with the drawing origin. At 98.4 %
and 67 % no pixel can be expected to be equal: Direct2D resamples the
picture and antialiases edges relative to the rectangle a paint binds.
Drawing an unchanged view in strips of 37 rows differs from drawing it whole
in up to 19,453 pixels, by 1 level, and at antialiased edges by up to 7.
The test therefore counts as stale only a pixel more than 16 levels off.
After the change none is, before the release or after it: the largest
difference is 7 levels, and in all but 7 pixels of a comparison 2. The
code before this amendment fails the same comparisons: after a cancelled
spotlight preview 1,571,844 pixels of the fitted viewport are more than 16
levels off (up to 128), and at the 30th move of a pixelating brush drawn up
and to the left 8, 125 and 9 pixels at the three zooms (up to 25). The press
that starts a spotlight repaints the whole viewport in every version: it
clears the selection.

Not changed by this amendment, and still costly in a tall image: the whole
image is recomputed when the darkening changes (first spotlight frame,
cancelling it, undoing the last spotlight, the opacity slider); a blur or
pixelation whose read area touches a moved effect is recomputed whole, and
repainted, in every paint during the drag; below about 20 % every repaint
uploads again each band of the base bitmap it shows; the keyboard nudge and
undo repaint the whole viewport. Bounding the recomputation to the rows on
show is a later step.

## Consequences

- Tool code has no knowledge of scrolling or zooming; a new tool that maps its
  points through the view works in a stitched image without further change.
- At the 48-million-pixel limit the editor holds the image and its redacted
  base, about 190 MB each, and an export allocates a third buffer of that
  size.
- Text is edited in a native control scaled to the zoom. Below 50% it becomes
  hard to read; the control is not replaced by a magnified one.
- The frame of a stitched image cannot be adjusted, and the view cannot be
  turned back into a capture.

## Validation

- `scrollshot_layout_logic`: fit, layout, clamping, round trips, the zoom
  ladder and the zoom anchor.
- `scrollshot_workspace_production`: an 8,400-row image shows exactly its rows
  at every scroll position across band edges; rectangle, arrow, blur and text
  land where the pointer is at 100%, 50% and 200%; hit distances are screen
  distances; the text host follows scroll and zoom; the banded export equals
  the one-pass export of the same shapes pixel for pixel.
- `capture_workspace_production`, `annotation_editing_native` and
  `ui_modernization_native` cover the unchanged screenshot path through the
  same mapping.
- `scrollshot_edit_performance` (Standard, private desktop; the 25,000-row
  image is `scrollshot_edit_performance_soak`): the five gestures above and a
  pixelating brush at the fitted width, at 67 % and at 10 %, painted as the
  workspace paints. In the middle of each gesture and after it, the partly
  repainted picture equals a full repaint (exactly at 10 % but for watermark
  glyphs, within Direct2D's drawing variance elsewhere, as described above).
  A gesture ending where it began leaves the export unchanged, moving a
  shape or a spotlight repaints under a tenth of the viewport, and switching
  the darkening on, or cancelling it, repaints all of it. Times are printed,
  never asserted.
- `redaction_renderer_logic`: in a 6,000-row picture, moving a spotlight
  reports exactly its old and new extent, and an unchanged effect list
  reports nothing.
