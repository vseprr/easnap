# ADR-002 — Capture workspace and explicit completion

**Status:** Accepted; supersedes the original explicit-confirmation interaction  
**Date:** 2026-08-27  
**Amended:** 2026-09-13

## Context

Selection, editing and terminal completion must remain distinguishable.
The accepted workspace allows entry into Editing on click/drag release, with
adjustable geometry until screenshot acquisition. Input must also distinguish
a deliberate copy double-click from selection or annotation drags.

## Decision

`CaptureWorkspace`, owned by `OverlayController`, holds local interaction state,
pending mode and the quick-return latch. `RegionSelector` owns geometry, hit
testing and adjustment. Only `CaptureCoordinator` changes application state.

| Local state | Behavior |
|---|---|
| Standby | Screenshot default; implicit initiating-monitor proposal; semantic Screenshot/Record border (orange/red in the normal palettes; ADR-016), no handles; snapshot magnifier/color shortcuts. Click enters Editing unchanged; a real drag replaces the proposal and enters Editing on release. Enter also enters Editing |
| Editing, provisional | Live desktop, handles/body/keyboard adjustment, Screenshot/Record switching, retained tools/configuration. No magnifier or acquired inline image |
| Editing, fixed screenshot | Successful acquisition shows immutable source pixels. Handles and adjustment disappear; mode switching is immediately disabled and its options fade out over 220 ms |

Standby maps to `RegionSelecting`; Editing maps to `ScreenshotEditing` or
`RecordingConfiguring`. Mode round trips preserve geometry and the same controls.
Only Start begins recording. The top mode toolbar exists from entry and remains
available until screenshot fixation or workspace retirement; it is not a
separate lifecycle step.

The first tool/style/command use, annotation interaction, or press anywhere in
the screenshot island—including padding, info and the native move grip—requests
acquisition (ADR-003). Only success fixes the image. Cancellation does not.
After fixation, dragging the island changes placement only.

### Pointer sequences and completion

`WorkspacePointerSequence` tracks down/move/up target, time, origin and drag
threshold. Native double-click time/rectangle limits apply. Two compatible
Content clicks or two compatible Annotation clicks can copy a Screenshot on the
second release. The previous and current targets must match.

A second-click drag remains an ordinary gesture. Creation drags, handles,
top mode options and island/configuration input never qualify for Copy.
Record does not quick-copy. During the fade the visible mode strip remains
excluded; after it is hidden that former area is ordinary workspace content.

For an annotation double-click, the first click keeps the tool's ordinary
behavior. The second pending gesture is cancelled before calling the existing
composite/clipboard path. No second annotation is committed, and no delay or
duplicate drawing/export implementation is introduced.

Input handled by focused native controls retains its normal semantics.
Screenshot completion failure leaves the acquired image and editor available
for another explicit attempt.

### One-session quick return

The workspace explicitly stores `screenshot_fixed`, `editor_feature_used`,
`quick_return_armed` and `automatic_attempt_consumed`.

- Leaving Screenshot Editing before feature use arms the latch.
- Successful acquisition/toolbar use fixes the image and blocks further mode
  changes. Tool selection or fixation alone does not consume an already armed
  latch. Programmatic placement changes do not imply user feature use.
- The editor reports a commit only when the production document revision
  changes: gesture release, text commit, or successful undo/redo/clear.
  Tool selection, pending text and cancelled strokes do not count.
- After returning to Screenshot, an eligible single non-drag content release
  can also consume the latch, including an annotation click with no committed
  shape. Handles and toolbar/configuration input remain excluded.
- The first eligible commit/click consumes the latch **before** publication.
  Manual Copy that commits text supplies the same attempt without recursion.
  Failure retains the session, but later mutations do not automatically retry.

Ordinary sessions allow multiple edits until explicit Copy, Save or cancellation.
Only successful Copy shows the brief green notice and closes to Idle. A new
capture resets all workspace/latch state.

## Validation and limits

Production tests cover entry, mode preservation, native focus, threshold/time
boundaries, incompatible targets and second-click drags. Native editor/clipboard
tests cover every tool, image pixels, acquisition failure, contention/retry,
fixed geometry, toolbar padding/grip and one-shot mutation completion.
The fade test observes real layered-window alpha, disabled mode switching,
hidden/resumed state and destruction. Mixed-DPI placement and subjective visual
acceptance remain desktop checks.
