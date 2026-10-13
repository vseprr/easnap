# ADR-036 — Islands stay outside the frame and never cover the mode island

**Status:** Accepted
**Date:** 2026-10-13

## Context

The screenshot tools island tried the place below the frame and then the place
above it, and clamped the result to the work area. With a frame from the top
edge of the monitor to the bottom edge, neither place exists; the clamp put the
island on the top edge, on top of the mode island. The first press on the tools
island fixes the screenshot by design, so the only way to move it aside also
ended the choice of mode.

The three islands of the provisional workspace each had a placement of their
own. The recording panel and the Scrollshot island kept out of the strip of
the mode island; the tools island did so only for its very first position.
The mode island itself never moved, also when the frame was drawn over it.

## Decision

### One rule for an island around the frame

`ui/island_layout` holds the geometry, as pure functions over desktop pixels.
`PlaceIslandForFrame` tries, in this order:

1. below the frame, centred on it;
2. above the frame, centred on it;
3. right of the frame, level with its bottom edge;
4. left of the frame, level with its bottom edge;
5. over the bottom of the frame.

A place outside the frame counts only when the whole island is inside the work
area. The island is over the frame only when none of the four outside places
has room. A frame that is the whole monitor keeps the island in the middle of
the work area, as before.

The screenshot tools island (with its property island and brush sizes as one
group) and the recording configuration panel use this function. The Scrollshot
island keeps `PlaceScrollshotSurfaces`, which tries the same four places and
then another monitor before it goes over the frame: during a scrolling capture
an island over the frame takes the wheel and the pointer from the content.

### The mode island is never covered

- The tools island receives the rectangle of the mode island (`keep_clear`). A
  place that would touch it is moved along the frame's edge, or away from it,
  before the next place is tried. The island may share the top edge with the
  mode island when it does not touch it.
- The recording panel and the Scrollshot island do not enter the strip of the
  mode island at all, as before. That also holds while the user drags the
  recording panel.
- Once modes can no longer be switched the mode island fades away and takes no
  room.

### The mode island follows the frame along its edge

- Its home is the top centre of the monitor on which the capture was started.
- When the frame takes that place, the island moves along the top edge to the
  nearer side of the frame that has room (`PlaceModeIsland`). When neither side
  has room it stays at home, over the frame.
- While the frame is drawn, moved or resized, the mode island is hidden, like
  the tools island. It returns when the gesture ends, already in its place for
  the new frame. It is placed before the other islands hear of the new frame.
- The user can still drag it along the edge. Where it is left becomes its home;
  the tools island places itself again around it.

### The recording panel follows the frame

Until now the panel kept its first position whatever happened to the frame.
It now follows the frame by the rule above when a gesture ends, and when
Record is entered again after the frame changed. It does not move during the
gesture and is not hidden. A panel the user has dragged stays where it was
put. Following is a move only: nothing is laid out or painted again.

## Consequences

- With a frame of full height the tools island is beside the frame where the
  display has room for it, else over the bottom of the frame, far from the
  mode island. It no longer has to be moved, so the frame is no longer fixed
  by accident.
- The mode island hides during every drag of the frame, including the first
  one. A click that accepts the proposed monitor is not a drag and hides
  nothing.
- A stitched image in the editor follows the same rule. Its view area leaves
  room below the picture, so its tools are below it as before.
- Sharing the top edge between the tools island and the mode island depends on
  the exact rectangle, not on a reserved strip. A top-docked taskbar still
  overlaps the mode island; that limitation is unchanged.

## Validation

- `island_layout_logic`: the order of preference, the mode island's moves and
  a sweep of frames over five display and island sizes, 25,214 checks. For
  every frame the island is inside the work area, never touches the mode
  island, is outside the frame whenever a place outside has room, and is over
  the frame only when none has.
- `capture_island_placement` (private desktop, production windows wired as the
  coordinator wires them): a frame drawn with the pointer, stretched to the
  full height by its handles, a narrow frame, a frame over the mode island's
  home, a mode island moved by the user, and the recording panel during and
  after a gesture and after the user placed it.
- In the running application on a 1920 x 1080 display at 125 %: a frame of
  full height with gaps at the sides put the tools island over the bottom of
  the frame and left the mode island at its home; both were hidden during a
  resize and shown after it; a narrow frame put the tools island beside it; a
  frame over the top centre moved the mode island beside the frame; the
  recording panel went beside the frame. Nothing was fixed on the way.
