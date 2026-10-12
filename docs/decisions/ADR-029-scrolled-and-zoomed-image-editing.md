# ADR-029 — Editing an image larger than the screen

**Status:** Accepted
**Date:** 2026-10-13

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
