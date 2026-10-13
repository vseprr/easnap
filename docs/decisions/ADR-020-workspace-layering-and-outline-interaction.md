# ADR-020: Workspace ownership and outline interaction

Date: 2026-09-17  
Status: Accepted for the requested follow-up corrections

## Context

Activating the mode bar or selection surface could raise the overlay's shading
and input planes above unowned recording controls or the screenshot editor.
The controls then appeared dark and stopped receiving pointer input. Filled
annotation hit areas also prevented drawing inside existing shapes.

## Decision

- Expose a separate presentation owner above the overlay's image/shading stack.
  Editor and recording configuration islands belong to that chrome window.
  The original input HWND remains the keyboard/pointer endpoint. Recording
  detaches from the workspace before its overlay is destroyed.
- Shape and effect hit targets follow visible outlines, including rectangular
  mosaic regions and the boundary of brush footprints. Empty interiors permit
  another annotation. Text/Tag bodies retain their text-editing semantics.
- Native Save dialogs temporarily suspend auxiliary editor islands, combo lists,
  tooltips and their placement/fade timers. Cancelling restores the context island
  without reopening the brush-size popup.
- When brush size opens, reserve all three islands as one group inside the work
  area. Shift the group only as needed; keep equal gaps and the callout below its
  hand control instead of covering the main tools with an upward fallback.
- The outside-shading surface contains static black pixels. Prepare it before
  interaction and change only its window region as the selection moves; do not
  request another full-monitor paint for each new region.

## Consequences

Window ownership enforces layering during native activation/movement rather
than relying on repeated foreground activation. Hit tests and rendering use the
same shape geometry. The system keeps native input/accessibility and Save dialog
behavior. Tests exercise real window hit targets after activation, outline-only
nesting, modal cancellation, source clipping and native drag/paint timings.

The magnifier keeps exact source-pixel/color sampling at 4 DIP per source pixel.
In the 2026-09-17 validation of this slice, the dirty area of a screenshot-frame
move fell by 98.2% and the p95 update time fell from 3.045 ms to 1.142 ms.
UI render timings are observations, not hardware-independent guarantees.
