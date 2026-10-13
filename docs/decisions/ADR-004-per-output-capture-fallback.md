# ADR-004 — Per-output DXGI/GDI capture fallback with blank-frame detection

**Status:** Accepted  
**Amended by:** [ADR-028](ADR-028-scrollshot-capture-workspace.md) (Scrollshot awaits the first composed frame)  
**Date:** 2026-08-27

## Context

DXGI Desktop Duplication (`IDXGIOutput1::DuplicateOutput`) is the preferred
capture backend and uses the GPU. Output format and driver capabilities still
constrain what can be captured. In particular:

1. It is unavailable in remote-desktop sessions and on some protected outputs.
2. On certain hardware (observed: 144 Hz DisplayPort primary at 125 % DPI),
   `AcquireNextFrame` returns `S_OK` but the captured texture is entirely black —
   a silent failure not detectable from the HRESULT alone.

The original design treated DXGI failure as a single process-level decision:
either everything uses DXGI or everything falls back to GDI.

## Decision

The fallback decision is made **per output**, not per process:

1. `CaptureMonitor` attempts DXGI Desktop Duplication for that monitor only.
2. `ImageLooksBlank` samples a 24 × 24 grid of the result. A frame that is
   uniformly black or fully transparent is treated as a capture failure even though
   every HRESULT said `S_OK`.
3. That output is marked `unusable` and all subsequent captures of it use GDI
   `BitBlt`. Other outputs keep using DXGI.

The region path used while recording (`CaptureMonitorRegion`) applies rule 3
only to an output that has not delivered content yet (`delivered_content`).
Once an output has returned real pixels, a later black frame is content and
does not switch that output to GDI (v1.0.1).

`FindOutputIndex` matches a `MonitorInfo` to a duplication slot by `HMONITOR`
first and by desktop rectangle second, so the mapping survives enumerations that
return monitors in a different order. A rebuild triggered during frame acquisition
sets `needs_rebuild_` rather than calling `Release()` re-entrantly.

The GDI fallback (`CaptureMonitorWithGdi`) is a documented product behaviour,
not an error path. It is never logged as a warning unless it replaces a previously
working DXGI path.

## Consequences

**Positive:**
- A single bad output does not degrade all monitors to GDI.
- Silent `S_OK`-but-black failures (confirmed on real hardware) are caught.
- Fallback is selected by output capability instead of a global OS-version branch.

**Negative / accepted trade-offs:**
- GDI `BitBlt` cannot capture hardware-overlaid content (e.g., certain video
  playback or HDR surfaces).
- Fallback cannot guarantee access to protected or otherwise unavailable content.
- The blank-frame heuristic may misfire on a legitimately all-black desktop, but
  the cost is falling back to GDI for that session, which is recoverable.

**Do not simplify** this to a single per-process decision; the hardware failure
mode that motivated it is real and reproducible.
