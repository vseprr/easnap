# ADR-003 — Inline editor island; full-resolution, view-independent export

> 2026-09-16: [ADR-017](ADR-017-neumorphic-editor-and-editable-annotations.md)
> supersedes toolbar composition, annotation input and effect-cache details.
> The immutable source, shared compositor and completion ownership remain.

**Status:** Accepted; standalone-window presentation superseded, export invariant retained  
**Date:** 2026-08-27  
**Amended:** 2026-09-14

Toolbar rendering and native accessibility are amended by
[ADR-016](ADR-016-shared-appearance-and-native-controls.md): the small island uses
GDI/native children; the Direct2D image renderer and export model below remain.

## Context

The original editor used a normal resizable window at half-monitor size with a
scaled preview. The user now explicitly requests inline Screenshot Editing and a
movable bottom island. This replaces that presentation, while retaining the same
production editor document, commands, history, mosaic and export paths.

## Decision

`ScreenshotEditor` remains the only screenshot editor. `Open(region, ...)` opens
the tools without image pixels. The frame is provisional: body/handles/keyboard
and mode switching remain available until the first toolbar interaction or
screenshot action.

The first tool/style/editor command or press anywhere in the island (including
padding, information strip and the native move grip) requests image preparation from the
coordinator. It hides overlay and island, calls `DwmFlush`, captures the current
desktop through the existing provider and crops the exact selected region. Only
a successful `SetImage` fixes it. Capture failure leaves the tools and provisional
frame retryable. Explicit Copy/Save/double-click also prepare the image on demand.
Cancellation does not. The Standby snapshot remains the magnifier source, not the
final editor image.

Once fixed, it holds one immutable region-sized source, one `AnnotationDocument`,
pending gesture/text, selected tool/style and cumulative mosaic cache. Selection
handles disappear; moving/resizing and mode changes are disabled immediately,
and the top mode options fade out over 220 ms. `SetRegion`
refuses to change an acquired image. Full-monitor and custom regions follow the
same rule. Its native HWND
contains the existing wrapping tool/style/command controls, info strip and a grip.
The island opens beside/below the region, stays inside its monitor work area and
reserves the top mode toolbar strip. A grip press acquires/fixes the image before
native island movement; subsequent movement changes neither crop nor document.
Programmatic placement alone does not acquire an image.

During a frame gesture, geometry and the frame's size label update immediately.
The island's duplicate size text refreshes on release/capture loss; keyboard
changes refresh it immediately. Moving the frame does not repaint the island.
This avoids unnecessary island presentation on each resize update
(ADR-015); image preparation always reads the current region.

`OverlayController` hosts a small opaque native image plane for each monitor's
intersection with the fixed selection. It stays hidden during provisional
Editing. The editor renders into that plane through its
Direct2D DC renderer, using the same annotation drawing code as export. This
preserves true magenta pixels; the image does not use the chrome transparency key.
GDI remains responsible for dimming/chrome/magnifier (ADR-001), and the island
uses GDI with native controls (ADR-016). Physical pixel coordinates and a 96-DPI
render target avoid applying UI DPI scaling to the image.

Pointer input is translated from signed desktop coordinates into the original
source image. Frame adjustment changes only provisional geometry; after capture
the source origin and rectangle stay fixed. Before feature use, hiding the island
for Record retains its placement and controls. Returning shows the same editor.
Actual image editing blocks subsequent mode changes. The coordinator retains the native recording configuration in the
same way; neither presentation recreates the other mode's state.

### Export invariant — unchanged

`Composite()` builds an independent WIC render target at the selected region's
original physical resolution. It crops the original/redacted base and translates
source-space annotations to that crop. It never reads a screen DC, island HWND,
dimming/chrome, magnifier, image-view pixels, UI DPI or presentation size.
Save still uses the existing WIC/native dialog path; Copy still publishes the
composited image as CF_DIB and PNG. Compositing/publication failure retains the
session and never falls back to falsely reporting an unannotated image as success.

The green tick, Ctrl+C and eligible Screenshot Content/Annotation double-click all use that
production copy path. Clipboard success shows a brief green notice and then
closes to Idle; failure keeps the image and controls available. The explicit
workspace quick-return latch invokes the same path at a successful mutation
boundary or eligible single-click return (ADR-002), never a second editor or
export implementation.

## Consequences and validation

The tools are independent of capture size. No captured image is stretched or
moved while adjusting the provisional frame. After feature use, content stays at
its captured desktop coordinates until session retirement.

Pointer movement invalidates only affected image bounds, including both old and
new shape bounds. The inline DC renderer binds only that dirty rectangle; it does
not allocate a full-screen GDI back buffer per movement or repaint dim/chrome.
Mosaic caches replay strokes in order using each stroke's brush and footprint.
A later distant stroke cannot alter earlier pixels or block alignment; overlapping
passes still increase intensity. Undo/redo deterministically rebuilds changed
mosaic history, while appended strokes reuse the existing cache. Owned island windows are destroyed
before their overlay owner during deferred cleanup.

Production tests compare full-resolution composited pixels with the native inline
DC render, including negative origins, monitor intersections and magenta. They
exercise deferred acquisition/failure, fixed geometry, annotations, undo/redo,
independent mosaic strokes, partial/full-render equivalence, provisional mode round trips,
one-shot mutation completion, real clipboard contention/retry and native tool
commands. Clipboard tests use a private window station. Real mixed-DPI island
movement and visual acceptance remain manual. Export resolution must never be
tied to island dimensions, display scaling or future presentation changes.
