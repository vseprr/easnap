# ADR-008 — Recording controls and safe finalization

**Status:** Accepted  
**Date:** 2026-08-28  
**Amended:** 2026-09-13 — current workspace and terminal completion

## Context

The recording backend owns capture, active-time cadence, audio, bounded queues
and Media Foundation finalization. Native controls must not move that work onto
the UI thread or become part of captured pixels.

## Decision

1. `CaptureCoordinator` owns application state. The recording surface reflects
   it and forwards commands.
2. Record Editing shows native configuration inside the provisional workspace
   with mode options available. MP4/GIF/audio/cursor choices survive mode round
   trips. Only Start retires the workspace and releases its snapshot/provider;
   `DwmFlush` precedes live capture. Capabilities follow ADR-012/014.
3. Four topmost click-through bars show the actual encoder-compatible bounds.
   They sit outside the region where possible. Bars and controls request
   `WDA_EXCLUDEFROMCAPTURE` for supported Windows capture paths.
   Initial auto-placement does not overwrite later user movement on timer,
   Pause/Resume or Stop updates; display/work-area changes may re-clamp it.
4. Elapsed time comes from recording diagnostics and excludes pauses.
   The accepted 2026-09-21 island uses icon-only Pause/Resume, preserving its
   native latched check state, recessed/raised material and state colors.
   The compact layout has no vertical separators; elapsed time is centered
   between Pause/Resume and Stop. Accessible names retain the action text.
   Only terminal worker/writer/audio conditions trigger runtime failure;
   transient capture misses remain diagnostics.
5. Pause/Resume preserve active-time and delivery boundaries. Stop/drain/finalize runs on
   an owned worker and posts a generation-tagged completion to the host.
   Since 2026-09-23, `RecordingFinalizer` retains the result independently of
   message delivery. The existing 250 ms recording status timer also attempts
   completion while finalizing. An early, duplicate or stale-generation message
   cannot join unfinished work or consume another session's result. The owner
   joins only a completed worker before reading results, and always joins on
   shutdown before releasing the session.
6. Successful finalization opens the shared preview/trim/completion workflow
   (ADR-010/011/014). Failure is explicit; non-empty recoverable media remains
   in protected storage with actionable recovery information. A wrapped status
   control displays finalization, and recording chrome is hidden before modal
   failure feedback.
7. Esc cancels configuration before a temporary recording exists. During
   recording or pause it means Stop and safe finalization; during finalization
   it cannot discard media. Reusing the global shortcut activates the existing
   panel instead of creating another session.

## Consequences

Recording controls add no duration-dependent buffering. Capture, audio,
encoding and finalization remain off the UI thread, and shutdown joins work
before storage cleanup. Odd dimensions lose at most one right/bottom pixel so
the boundary matches even NV12 encoder dimensions.

Save/Copy/confirmed Discard and clipboard-file lifetime follow ADR-011/014.
Native focus, capture exclusion, device behavior and multi-monitor/DPI layout
require hardware checks in addition to automated tests. Current manual acceptance
and the release-validation boundary are recorded in [project status](../project-status.md).

The completion-recovery target is within one second after worker completion on
a responsive owner message loop (250 ms poll interval). It is tested with real
failed `PostMessageW`, stale/duplicate messages and repeated worker lifetimes.
It does not bound native Stop/drain, Windows scheduling or a blocked UI thread.
Startup and pause also synchronously wait for their existing native/barrier work;
their completion has no newly introduced timeout guarantee.
If worker creation fails, the existing synchronous safe-drain fallback remains.
Permanently hung native calls retain the limitation in ADR-021.

## Capture exclusion of the islands (2026-10-30)

**Defect.** A window excluded with `WDA_EXCLUDEFROMCAPTURE` is not always
left out of a picture: Windows 10 can put a black hole of its shape in its
place. Two beta reports had this cause. A recording panel once appeared as a
uniformly black box in a GIF; and a screenshot whose frame had been dragged
until the tools island lay over it, then fixed, held a black rounded
rectangle of 1040 x 60 px, the tools island at 125 %, while the island moved
to the place freed above the frame (development PC, 2026-10-30 22:03).

**Measured cause** (`island_capture_exclusion`, lab VM, probes of
2026-10-30). A backdrop is painted once; islands are then shown, shown again
after hiding (as during a frame drag) and moved while visible, and nothing
under them is drawn again before the screen is read as a screenshot reads
it (`CaptureDesktop`) and as a screen blit:

| Window | First shown | Shown again after hiding | Moved while visible |
|---|---:|---:|---:|
| Plain excluded test window, 160 x 120 | 0 | 19,200 | 19,200 |
| Layered excluded test window (alpha 255) | 0 | 0 | 0 |
| Tools island, before the change | 46,744 | 46,744 | 45,304 |
| Recording panel, before the change | 28,984 | 28,984 | 28,984 |
| Scrollshot island, before the change | 34,142 | 34,142 | 31,982 |
| Each of the three after the change | 0 | 0 | 0 |

The figures are black pixels under the window, the same through Desktop
Duplication and through the screen blit. The islands had been shown once
before the first round, so every round was a showing again. A plain window
leaves no hole when it is first shown, and none while the content under it
is being redrawn: the first version of the test repainted its backdrop before
each read and passed, on the development PC as well (68 checks). A visible
tooltip elsewhere on the screen also suppressed the hole in one run. That is
why the defect came and went.

**Decision.** `ui::ExcludeFromCapture` makes a window that is not layered a
layered window at full opacity before excluding it; it looks and paints as
before. The tools island, the brush pop-up, the property island (layered
already), the recording panel and its boundary bars, the Scrollshot island
and its preview, and the Copied/Saved and status notices go through it. A new
window that is excluded from capture uses it as well.

**Tooltips.** A tooltip is layered from creation, and Windows refused its
exclusion before it was first composed: the affinity read 0 after styling and
after showing, and the tooltip appeared in pictures with its own colour
(5,060 px of 0x2c2f33). `StyleTooltip` now excludes it again whenever a
showing begins (`WM_WINDOWPOSCHANGING` with `SWP_SHOWWINDOW`); it then reads
0x11 and leaves nothing in the picture in all three rounds.

**Not validated.** The changed build on the development PC (the owner's
manual check and a host run of the test); message dialogs and menus, which
are created anew for each showing, are not covered by the test.
