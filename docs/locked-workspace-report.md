# Locked screenshot interaction report

Workspace availability (2026-09-23): historical build-tree evidence referenced here
was absent before this task; see [current status](project-status.md). Recorded results below
remain historical.

**Date:** 2026-09-13

**Acceptance update (2026-09-14):** The user confirmed manual
implementation/testing, reported no issues in the app and closed Phase 3.3.
The current locked-image interactions and fade are accepted. The measurements
below remain the results of the 2026-09-13 run; see
[project status](project-status.md) for the current handoff.

## Cause and changes

Double-click completion previously recognized only Content clicks. Selecting a
drawing tool routed image input through the Annotation target, which explicitly
excluded double-click completion. This was a concrete input-policy mismatch,
not a capture/clipboard failure or a speculative timing problem.

The production pointer policy now accepts same-target Annotation double-clicks
as well as Content pairs. Copy runs on the second release, after cancelling its
pending mark. A second-click drag continues drawing. The first click retains
normal tool behavior; the change introduces no single-click delay. Every tool
uses the existing full-resolution composite and clipboard publication path.

Previously, only an actual editor feature invoked deferred acquisition. Blank
toolbar input and native grip movement bypassed it. Toolbar padding, information
strip and grip presses now use that same acquisition boundary. Capture failure
remains retryable; cancellation still exits without acquisition.

Successful acquisition disables mode switching immediately. The top mode
options now occupy a small layered GDI companion and fade out with a 220 ms
smoothstep opacity curve. The animation updates only that surface's alpha,
shares the monitor's existing drawing buffer and stops its timer when hidden
or destroyed. The frame, hints and inline image remain visible. This preserves
the damage-limited drag/resize implementation from ADR-015.

## Regression validation

Release x64 built with warnings as errors. The full CTest run passed **20/20**
in **324.60 seconds**. The WASAPI case reported 14 passed, 0 failed, 0 skipped.
The portable CMake install produced both executables. Documentation validation
checked 25 documents and 53 local link/image targets without errors.

New tests exercise real production logic and native resources:

- Clipboard CF_DIB pixels and PNG availability for Pointer, Pen, Arrow,
  Rectangle, Ellipse, Text and Mosaic after toolbar acquisition.
- No completion before the second release; second-click drags continue editing;
  incompatible targets do not form a copy pair; geometry stays fixed.
- Real clipboard contention retains source/edits and allows a later double-click
  retry. The expected image includes the first click's normal tool behavior.
- Blank/info/grip acquisition, capture failure followed by successful retry,
  and no repeated acquisition after fixation.
- Native layered-window alpha decreases through intermediate values, hides
  within the test deadline, blocks mode changes immediately, preserves focus,
  stays hidden after resume and resets for a new session. Destruction during
  fade releases the companion window.

Existing coverage includes selection/magnifier behavior, annotation pixels,
undo/redo and mosaic, partial/full render equivalence, resource lifetime,
recording/capture, audio mixing/resampling, MP4/GIF, trim, supervised preview and
publication cleanup. Generated image fixtures verify pixel correctness;
the subsequent manual acceptance is recorded above.

## Comparison with the approved performance version

Five alternating before/after native replay runs used the saved executable from
the user-approved frame fix and the current Release build. Each case delivered
160 real production-window geometry updates at 1920 × 1080 and serviced native
paints. Values below are the median of each run's median update time.

| Case | Approved version | Current version |
|---|---:|---:|
| Screenshot move | 0.461 ms | 0.378 ms |
| Screenshot resize | 0.457 ms | 0.374 ms |
| Record move | 0.461 ms | 0.378 ms |
| Record resize | 0.460 ms | 0.373 ms |

Damage counts remain 5,956,340 pixels per move case and 6,099,378 per resize
case, with 160 callbacks. No regression appeared in this workload. Timings
include production message handling and paint work; they do not measure complete
physical-pointer-to-display latency or promise identical results on other PCs.

Local evidence is retained under `build-remediation/`:
`locked-workspace-build.log`, `locked-workspace-final-ctest.log`,
`locked-workspace-final-test-details.log`,
`locked-workspace-performance-comparison.json`, the comparison script and raw
logs. `locked-workspace-baseline/` preserves the pre-change source/documents and
replay executable. A review patch and SHA-256 manifest make the final diff
inspectable in this supplied workspace without Git metadata.

## Documentation and acceptance

README, architecture, user flows, security and project status now describe the
actual two-process application, deferred screenshot acquisition, implemented
MP4/GIF/Both audio and durable completion. The handoff is concise, future UI
direction is separate, and obsolete phase instructions have been removed or
amended. Relevant ADR contracts and historical investigation evidence remain.

The user accepted the responsiveness fix on 2026-09-13 and confirmed the current
app and Phase 3.3 closure on 2026-09-14. The fade and locked-image double-click
acceptance task is closed. Broader device and Windows-version coverage belongs
to Phase 4 release validation in the [roadmap](../EASNAP_ROADMAP.md); these dated
test results do not establish that whole compatibility matrix.
