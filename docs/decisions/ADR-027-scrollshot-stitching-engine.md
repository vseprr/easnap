# ADR-027 — Scrollshot stitching engine

**Status:** Accepted
**Date:** 2026-10-12

## Context

A scrolling screenshot is built from many pictures of one fixed screen
rectangle while the user scrolls its content. Each picture has to be placed on
a growing canvas. Real content does not make that easy: pages have fixed
headers and footers, scrollbars and side rails stay in place, carets blink,
rows highlight under the pointer, images arrive late, lists repeat and blank
stretches carry no detail. Comparing whole frames exactly fails as soon as
anything inside the overlap changes; accepting the best of several doubtful
fits produces a wrong image that looks plausible.

The method had to be original. No scrolling-capture code from another project
was read or adapted, and no image library was added.

## Decision

`src/scrollshot/stitch_engine.*` is a module with no window, thread, clock or
capture dependency. `StitchEngine::Submit` takes one frame and returns where it
was placed, or an explicit reason why it was not. The result depends only on
the frames submitted.

### Row descriptors

Every row is cut into up to 32 segments of at least 24 pixels. A segment is
described by its mean luminance and its mean horizontal gradient, both with
four fractional bits. Two descriptors agree when both values are within
2.5 levels. Capture noise of a few levels per pixel averages well below that
over a segment.

A segment is *informative* on a row when it differs from the row above or
below by at least 3 levels. Only informative segments can tell one vertical
offset from another: a flat area fits anywhere.

Each row also carries a hash of its exact pixels. Equal hashes are full
agreement for the row, counted only where the row differs from a neighbour.
Lossless captures of unchanged content hit this path, which also places smooth
pictures whose segments are not informative; noisy input falls back to the
tolerant comparison.

### Finding the offset

1. A frame identical to the previous one (every segment within tolerance) is
   reported as `Unchanged`.
2. Otherwise the rows at the top and bottom that kept their place since the
   previous frame are measured. They are the candidate fixed header and footer
   and are left out when a moved frame is compared.
3. A coarse pass compares four values per row for every position within one
   frame height of the last placement (the whole canvas after a frame could
   not be placed) and keeps up to 12 local minima.
4. Each of them, and the position that would continue the previous motion, is
   scored with the full descriptors within ±2 rows. The score is the share of
   informative segments that agree with the canvas rows they would cover.
5. The best share wins if it is at least 55 % and no other candidate *ties*
   with it. A rival ties unless the best candidate agrees on clearly more
   detail: at the rival's share it would lose at least two rows' worth of
   segments, and 2 % of its own. Four rows' worth of disagreement is added to
   every total before shares are compared, so a perfect fit on a sliver of
   overlap does not outrank a near-perfect fit on a large one.
6. When candidates tie, the one that continues the motion of the previous frame
   is taken, provided a scroll is in progress, it lies within half that
   displacement (at least 6 rows) of the prediction and every other tied
   candidate is more than twice as far. Otherwise the frame is `Ambiguous`.
7. "The frame did not move" is judged separately and by rows: it is accepted
   when at least 25 % of the rows with detail are exactly as they were (50 %
   before any motion revealed the fixed bands). A picture that finishes loading
   changes many rows and leaves the rest untouched; a jump to other content
   leaves none.

A frame that cannot be placed changes nothing. Its outcome is `Ambiguous`,
`LowInformation` (overlap exists but carries too little detail) or `NoOverlap`.
The search then covers the whole canvas, so scrolling back into captured rows
resumes tracking.

### Writing

- The first frame is written whole. Later frames write the rows between the
  fixed bands; the 12 rows next to each band are written only where nothing is
  known yet, so a header's shadow cannot leave one line per frame.
- Rows that are still visible are overwritten by every placed frame. An image
  that loads late, a caret and a hover highlight therefore end in their latest
  state, and a highlight under a resting pointer survives only in the last
  viewport.
- The header is whatever the first frame showed above the content. The rows
  under the bottom band are written provisionally by the lowest frame and
  replaced when a lower frame arrives, so a footer appears once, at the end.
  The fixed bands used for writing are the larger of the current measurement
  and the median of the last five, which keeps one frame with an animated
  header from stamping it into the content.
- Scrolling above the first captured row is tracked but writes nothing.
- Side rails: columns at the left or right edge that keep their pixels while
  the neighbouring columns scroll, on at least a quarter of the sampled rows
  and in two consecutive moving frames, are fixed for the session. They show
  the first viewport once; rows below continue each column with its median
  colour. A rail is taken only as far as its own detail reaches. A scrollbar
  usually stays below the quarter-height requirement and is then treated as
  content; its columns end up as track, with the thumb where the first and
  last viewports showed it.
- The canvas is stored in strips of 256 rows. Growing never copies rows that
  were already written.

### Limits

The canvas stops at 32,000 rows or 48,000,000 pixels, whichever comes first
(192 MB of pixels). Frames that reach beyond are placed and reported as
`LimitReached`; rows past the limit are dropped.

## Consequences

`tests/test_scrollshot_stitch.cpp` cuts synthetic documents along scripted
trajectories and requires the exact document back for: steps from 1 to 233
rows, eased wheel animations with repeated frames, both directions, scrolling
above the start, a jump followed by a return, fixed header and footer, a
header with a changing clock, a scrollbar, a side rail, a resting hover
highlight with a blinking caret, a late picture, repeating lists, sparse short
lines, narrow and wide frames and random trajectories. Noise of up to 4 levels
per channel must leave every placement exact. An empty stretch taller than the
viewport, a jump inside exactly repeating rows and a jump beyond the overlap
must be reported and must leave the canvas untouched. The thresholds above
were tuned against that table, not against a screenshot.

Known boundaries, all reported rather than guessed:

- Content with no detail for more than one viewport cannot be crossed.
- Exactly repeating content can only be crossed while a scroll that started on
  distinguishable content is in progress.
- Under capture noise, a smooth picture that fills the whole overlap is
  low-information.
- A header or rail that animates on every frame is not recognised as fixed.
- A full-width footer over a side rail shows the rail's fill in the rail
  columns.
- Before the first motion reveals the fixed bands, a jump inside a page whose
  fixed header holds more than half of the detailed rows would be taken for an
  unmoved viewport.
