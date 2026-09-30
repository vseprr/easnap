# Capture/editor and recording follow-up validation

Workspace availability (2026-09-23): historical build-tree evidence referenced here
was absent before this task; see [current status](../project-status.md). Recorded results below
remain historical.

Historical implementation/test evidence. The current accepted UI baseline and
2026-09-21 session closure are in [project status](../project-status.md) and
[session validation](text-trim-validation.md). This report is not an open task list.

2026-09-17. This slice implements the additional user corrections after the
neumorphic paint/chrome work. Reference screenshots define interaction and icon
geometry; the existing Light/Dark theme remains the visual source of truth.

## Implemented behavior

- Editor and recording configuration are owned by the workspace chrome rather
  than the dim input layer. Moving/resizing the provisional frame or moving its
  mode strip no longer places dimming above the tool/configuration island.
  Recording ownership is detached before the workspace is retired.
- Recording panels drag from blank space and informational labels, while buttons
  and settings retain their actions. Pause/Resume has sufficient measured text
  width in a 390-DIP panel, with separators outside its bounds.
- Shape and effect interiors allow new drawing. Selection/movement uses their
  outlines, including arrowhead edges and the mosaic brush contour. Text/Tag
  retain their text interaction area; a repeated click on active text no longer
  consumes the next outside-click commit.
- Black and gray join the color palette. Stroke presets use size dots; the mosaic
  hand and dropdown arrow share one button. Its brush island has a pointing
  callout and the same 8-DIP gap as the main/context islands. Near the bottom
  edge the complete group shifts up enough to retain those gaps without overlap.
- Watermark text has a hover/focus clear button inside the edit field, retaining
  Undo. Native Save dialogs suspend configuration/brush islands, tooltip/fade
  timers and combo dropdowns; cancel restores the relevant configuration.
- The magnifier/color readout uses a 152×166-DIP panel and 4-DIP source pixels
  instead of 190×240 and 12. It samples a wider 25×25 area, removes grid borders
  and retains exact center sampling, RGB/HEX and copy behavior.
- The dim shade is filled before exposure. Frame updates change its clipping
  region without repainting the full monitor. Native screen-pixel checks cover
  expansion and contraction, including newly exposed shade pixels.

Recording initialization, Pause/Resume timing and live temporary-session
ownership changes, reproduction details and stress coverage are recorded in the
[backend investigation](../investigations/2026-09-17-recording-backend-hardening.md).
The completed lifetime correction passed 50 debugger processes and 30 normal
processes, including 440 cancelled exports. The old lifetime path, with its
constructor race already corrected, still crashed on replay 10 inside the new
in-flight cancellation regression; its WER stack matched the NVENC fault.
Durable decisions: [ADR-020](../decisions/ADR-020-workspace-layering-and-outline-interaction.md)
and [ADR-021](../decisions/ADR-021-recording-startup-timeline-and-session-leases.md).

## Validation scope

Complete Release and Debug builds pass in the normal `build` tree; the separate
Release build used for the portable package also passes.

| Final CTest run | Result | Elapsed |
| --- | --- | --- |
| Release, normal build tree | 23/23 passed | 368.50 s |
| Debug, normal build tree | 23/23 passed | 404.72 s |

Both runs included 138/138 MP4 trim checks, 1008/1008 real recording checks,
1240/1240 recording stress checks and WASAPI 14/14 with zero skips. The final
summaries and detailed output are retained as `full-release-final.log`,
`full-debug-final.log`, `ctest-release-detail.log` and `ctest-debug-detail.log`
under `build-followup-hardening/`.

Native HWND fixtures exercise production controls, hit-testing, text editing,
window ownership, modal Save cancellation, magnifier sampling and compositing.
Recording tests use actual desktop capture, encoders, decoded sample timestamps
and installed audio endpoints. They do not substitute synthetic successful media
operations for production behavior.

The final Release editor check and full UI evidence run both completed with zero
failures. Light/Dark magnifier, paused recording, brush popup and watermark clear
renders were visually inspected. Magnifier evidence uses its production
WM_PRINTCLIENT handler; PrintWindow alone returns black for that excluded popup.
The rendered color readout and sampled center agree. Evidence lives under
`build-followup-hardening/evidence/`; logs are `editor-checks-final-release.log`
and `ui-evidence-final.log` in that parent folder.

Performance measurements are retained in
`build-followup-hardening/magnifier-selection-validation.md`: screenshot move
dirty area fell 98.2% and p95 update time fell from 3.045 to 1.142 ms. Initial
release/editor creation measured 51–68 ms. That variation is not attributed to
an initialization change; the reported severe shortcut/first-drag freeze was not
reproduced. Startup capture before the visible workspace and physical input
latency were not measured by this fixture.

## Limits and handoff

Computer Use's native pipe could not connect (OS error 2). Automated native
interaction and pixel evidence do not establish manual motion, mixed-DPI,
multi-monitor, complete IME or screen-reader acceptance. The device stress is
bounded to this Windows/GPU/audio setup, not an indefinite soak.

Unknown legacy temporary folders without session markers are retained. The
one-off temp-write dialog cannot be attributed conclusively without its original
error code. The historical WASAPI failure was not reproduced; see the backend
report for the actual current checks and limits.

The user's running `build/bin/Release/Easnap.exe` initially prevented replacement
(LNK1104); that session was not terminated by this work. The lock subsequently
cleared and the complete normal Release build succeeded, updating that executable.
A separately built matching portable app/preview helper is also available under
`build-followup-hardening/portable/bin/`. Source edits remain in this workspace.
This folder has no Git metadata; the review delta against the fresh session
baseline is `build-followup-hardening/changes.patch`.
