# ADR-001 — Overlay rendering: layered GDI, not Direct3D

**Status:** Accepted  
**Amended by:** [ADR-015](ADR-015-frame-interaction-rendering.md) (shared back buffer, damage-limited painting); [ADR-018](ADR-018-editor-interaction-corrections.md) (input plane, clipped shade, 500 ms fade); [ADR-020](ADR-020-workspace-layering-and-outline-interaction.md) (static shade, chrome-owned islands); [ADR-028](ADR-028-scrollshot-capture-workspace.md) (Scrollshot shading strips and input opening)  
**Date:** 2026-08-27  
**Amended:** 2026-09-13

## Context

The capture overlay must dim the entire desktop and present a frozen snapshot
beneath the selection. Two rendering approaches were considered:

1. **Direct3D swap chain** per monitor (original design in the planning document).
2. **Layered GDI windows** with `SetLayeredWindowAttributes` / `UpdateLayeredWindow`.

A D3D swap chain that fails to present — for example when a full-screen exclusive
application holds an output — leaves the overlay as a black rectangle. Because the
overlay covers the whole screen, a black-screen failure is a severe UX regression:
the user cannot see what they are doing and may inadvertently screenshot the black
frame. Hardware testing confirmed this failure mode on a 144 Hz DisplayPort primary
at 125 % DPI.

## Decision

The overlay uses **three full-monitor layered GDI windows** per monitor and small companion
surfaces, with no dimming/chrome swap chain:

| Window | Purpose |
|---|---|
| `dim_` | `LWA_ALPHA` input plane at alpha 1; takes pointer and keyboard input and contributes no visible dimming |
| `shade_` | `LWA_ALPHA` click-through dimming plane of static black pixels; a window region cuts out the selection |
| `chrome_` | `LWA_COLORKEY` click-through plane; handles, readout, hints and notices |
| `mode_toolbar_` | Small `LWA_COLORKEY`/`LWA_ALPHA` mode-options surface on the palette-owning monitor; hosts the native mode buttons |
| `magnifier_` | Small `LWA_ALPHA=255` click-through window, visible only on the pointer's monitor |
| `image_` | Opaque selection/monitor intersection after screenshot fixation, painted by the existing editor's DC renderer |

Provisional Editing paints no captured image; the first screenshot toolbar
interaction or screenshot action takes the final image (ADR-003). The workspace
surfaces stay visible during that capture when every visible window of the
process is excluded from capture, and are hidden for it otherwise (ADR-018).
For fixed-image Editing (amended ADR-003), a selection-sized opaque `image_` plane
is owned by `shade_`, which is owned by `dim_`, and `chrome_` is owned by `image_`. Native ownership keeps
chrome above the inline image and the image above the dimming. The editor paints this companion through its
Direct2D DC renderer; dimming/chrome retain their GDI implementation. The input plane is never transparent
to mouse input, except inside the frame while a Scrollshot is capturing (ADR-028); the click-through planes never intercept clicks.

Screenshot fixation disables mode switching immediately. The mode-options
companion, owned by chrome, fades with a 500 ms smoothstep alpha curve and is
then hidden. Its timer changes only this small window's opacity, stops when
hidden/destroyed, and does not repaint full-monitor surfaces. It shares the
monitor's existing GDI buffer and never allocates another full-monitor bitmap.

`UpdateLayeredWindow` (per-pixel alpha) is deliberately avoided because GDI text
drawing zeroes the alpha channel of a 32-bpp DIB, which would make labels vanish.

### Cursor magnifier extension

The magnifier uses the same native GDI ownership and double buffering, but an
opaque alpha window instead of the chrome color key: otherwise an actual magenta
sample would become a transparent hole. It is owned by the dim plane, never
activates, and passes input through (`WS_EX_TRANSPARENT`, `HTTRANSPARENT`). The
small window is visible only in Standby, follows the pointer, changes placement at monitor edges and avoids
the mode toolbar; it does not allocate another full-screen buffer.

`FrozenBitmap` retains a read-only pointer to its existing top-down snapshot DIB.
The sampler subtracts each monitor's signed virtual origin and indexes physical
BGRA pixels directly; DPI scales the magnifier UI only. The center value and all
25 × 25 grid samples use that same path, across adjacent monitor boundaries when
pixels exist. No screen DC is read while the overlay is open. Missing data or
virtual-desktop gaps return unavailable rather than a fabricated color. The
snapshot copy lives with its overlay window, independent of the source container
and any backend. Destruction releases both DIB and companion window.

## Consequences

**Positive:**
- Overlay dimming/chrome avoid the observed swap-chain black-screen failure.
- Dimming and chrome do not depend on editor GPU resources.
- GDI dim/chrome/mode-options/magnifier painting shares one reusable `BackBuffer` per monitor,
  with native damage clipping (ADR-015). Geometry changes repaint frame strips
  and labels; Editing's uniform dim plane stays clean. Fixed
  image painting uses the editor's buffered DC target bound to the dirty rectangle;
  pointer movement does not allocate/repaint another full-monitor buffer.

**Negative / accepted trade-offs:**
- GDI text rendering quality is slightly below D2D on high-DPI displays.
- Per-pixel alpha gradients are not available (LWA_ALPHA is uniform).
- The existing editor uses D2D in its island and inline image companion;
  GDI remains responsible for dimming, chrome and Standby magnification.

**Do not revert** this decision for performance or aesthetic reasons without first
demonstrating that the black-screen failure mode cannot recur on the target
hardware.

