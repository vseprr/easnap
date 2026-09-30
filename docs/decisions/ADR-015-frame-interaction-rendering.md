# ADR-015 — Damage-limited frame rendering and gesture completion

**Status:** Accepted  
**Date:** 2026-09-13

## Context

Moving/resizing the provisional Editing frame invalidated both full-monitor GDI
planes, allocated new monitor-sized drawing surfaces, and repainted the tools
island on every update. The island's `Present(1, 0)` could wait for vertical sync
on the input thread. Record configuration also moved/repainted its unchanged
child controls and recreated fonts on every region update.

Native production-window replay confirmed this work: each 160-update case
invalidated 663,552,000 pixels at 1920 × 1080. Removing overlay work alone left
Screenshot resize at approximately 7 ms per update; removing its redundant
island presentation reduced it to approximately 0.45 ms. See the performance
report for methodology and validation limits.

## Decision

- Preserve ADR-001's layered-GDI dim/chrome implementation. Share one reusable
  compatible bitmap/DC across a monitor's serial dim, chrome, mode-options and magnifier
  paints. Its allocation grows only when required and is released with the
  overlay. Allocation failure falls back to painting the native clipped DC.
- Geometry changes invalidate the old and new border/handle strips and size
  labels. The renderer shares its label layout with the damage calculation.
  Preserve the disjoint native update region in the buffered DC; its enclosing
  rectangle alone would still redraw the frame's interior. Sub-minimum
  selection/crosshair transitions use full chrome invalidation.
- Provisional Editing's dim plane stays uniform. Frame updates do not repaint
  it, position the hidden image, or relayout the recording configuration panel.
  Mode changes, image fixation and resume retain full invalidation.
- Selection coordinates and the frame's size label update on every distinct
  geometry change. The island's duplicate size text updates on gesture
  completion, including capture loss and release without a final move message.
  Keyboard changes update immediately. This avoids a synchronized island
  presentation during resizing without timers or dropping input coordinates.
- Suppress duplicate geometry notifications but retain the transition out of an
  active gesture. Recording retains the latest region; retiring the workspace
  lays out its live recording boundaries from that region.

## Consequences

Normal frame updates repaint thin strips instead of two monitor-sized surfaces.
One bounded bitmap per monitor is retained for the overlay lifetime (roughly
7.9 MiB at 1920 × 1080 / 31.6 MiB at 3840 × 2160 for 32-bit pixels). This trades
retained memory for eliminating allocation churn; it is not a claim of lower
idle memory. It is shared across the GDI surfaces, not allocated per plane.
The small mode-options companion added in ADR-001 reuses it; fixation fade
changes that window's alpha without adding full-monitor repaint work.

Screenshot capture, immutable image geometry, annotation, undo/redo, clipboard,
recording, audio, encoding, trim and export semantics are preserved. The island's
duplicate dimensions may show their pre-gesture value during adjustment; the
frame's label and capture coordinates remain current.

Regression coverage compares full and partial GDI pixels at 100%, 125% and
200% DPI with positive/negative monitor origins, verifies native geometry and
completion behavior, and checks resource counts across warmed repeated sessions.
The optional native replay reports timings without machine-dependent speed
assertions. Physical-pointer/compositor smoothness and additional display/GPU
configurations still require desktop acceptance.
