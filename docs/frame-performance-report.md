# Frame interaction performance report

Workspace availability (2026-09-23): historical build-tree evidence referenced here
was absent before this task; see [current status](project-status.md). Recorded results below
remain historical.

2026-09-13 — MSVC x64 Release, Windows desktop, 1920 × 1080

This report records the original responsiveness fix, subsequently accepted by
the user. The follow-up toolbar/locked-image changes and their comparison with
that accepted version are in [the interaction report](locked-workspace-report.md).
Use [project status](project-status.md) for the latest handoff and validation.

## Outcome

The provisional Editing frame's move/resize path is optimized and the full
Release regression suite passes **20/20**. Five alternating before/after runs
reduced median native input-and-paint time from approximately **8 ms to 0.45 ms**
per update (about **17× faster / 94% less time**). Overlay damage decreased by
approximately **99.1%**. This measures production-window replay, not physical
mouse-to-display latency or a claim of manual smoothness acceptance.

At the start of this investigation, the capture workspace was awaiting desktop
acceptance after the 2026-09-11 19/19 test baseline. The responsiveness fix was
accepted on 2026-09-13; the user subsequently confirmed manual testing and
Phase 3.3 closure on 2026-09-14 with no app issues reported. The measured build was
`build-remediation/bin/Release/Easnap.exe`.

## Confirmed causes

1. `OverlayController::PointerMove` called `RegionChanged`, which invalidated
   both full-monitor dim/chrome windows on every monitor. In Editing, the dim
   plane is uniform: moving the frame changes none of its pixels. Each native
   paint also created and deleted a monitor-sized compatible bitmap and DC.
   At 1080p, 160 updates invalidated **663,552,000 pixels** in every measured case.
2. The region callback called `ScreenshotEditor::SetRegion`, which invalidated
   the entire tools island even for position-only changes. Its render path ends
   in `SwapChainTarget::Present(1, 0)`, introducing a synchronized presentation
   into the UI thread's drag workload. The island shows dimensions, not position;
   its dimensions also duplicate the frame's existing live size label.
3. `RecordingControls::SetRegion` called `Layout(false, true, true)` for every
   visible configuration update. This repositioned the panel and child controls,
   repainted them and rebuilt fonts, although the hosted panel's placement and
   controls do not depend on the moving frame.
4. Duplicate pointer/release coordinates repeated region notifications and
   invalidation. Keyboard handlers also repeated full invalidation after
   `RegionChanged` had already requested it.

These causes are supported by executed production paths and measurements. An
intermediate build that removed overlay/layout work but still refreshed the
island during Screenshot resize left that case at **6.933 ms median**, while
the other three cases were approximately **0.45 ms**. Deferring the duplicate
island readout until gesture completion removed that remaining bottleneck.
The intermediate evidence is `build-remediation/frame-performance-first-after.log`.
No capture/codec delay, fake workload, injected sleep or invented device problem
was used to explain the result.

## Changes

- Invalidate old/new frame edges, handles and size labels. Share label geometry
  with the production renderer and preserve the disjoint update region when
  drawing to the buffer; a bounding rectangle alone would cover the interior.
- Reuse one compatible bitmap/DC per monitor across its dim/chrome/magnifier
  paints. Keep the GDI fallback and existing layered-window architecture.
- Leave the uniform Editing dim plane and hidden image alone during adjustments;
  toolbar hover invalidates only the toolbar. Mode/fixation/resume transitions
  retain full invalidation, and sub-minimum/crosshair changes clear full chrome.
- Update selection/capture coordinates immediately. Keep the frame's size label
  live; refresh the island's duplicate dimensions on release or capture loss.
  Keyboard changes refresh immediately. No timer or artificial delay is added.
- Keep the recording panel's controls/fonts intact during frame changes, while
  storing the latest region for recording-boundary layout at workspace retirement.
- Skip unchanged geometry while preserving gesture-end notification and final
  release coordinates, including release without a preceding move message.

## Reproducible measurement

`test_capture_workspace --frame-performance` creates the production overlay,
screenshot editor and recording configuration. It sends native move/release
messages and services actual pending paints, then flushes GDI. Each case sends
160 updates, expanding/moving then reversing, with the same geometry and input
sequence before and after. Initialization, instrumentation that counts the
update-region area, and final release are outside the per-update timing. Release
and capture-loss correctness are checked separately. Timing thresholds do not
decide test success.

The snapshot is deterministic because this test measures provisional frame UI,
which does not acquire or display a screenshot. Production rendering and input
logic perform the measured work. It is not an end-to-end capture benchmark.

Five runs of each executable were interleaved, alternating order, with normal
Windows access after the full suite finished. Each table value is the median of
the five per-run statistics, in milliseconds:

| Operation | Before median | After median | Before p95 | After p95 |
|---|---:|---:|---:|---:|
| Screenshot move | 8.104 | 0.455 | 14.148 | 0.582 |
| Screenshot resize | 8.043 | 0.454 | 14.004 | 0.541 |
| Record move | 7.881 | 0.455 | 11.603 | 0.520 |
| Record resize | 7.921 | 0.454 | 11.493 | 0.518 |

Post-change per-run medians ranged from **0.450–0.460 ms**; pre-change medians
ranged from **7.863–8.300 ms**. Dirty pixels per 160 updates fell from
663,552,000 to **5,956,340 for move / 6,099,378 for resize**. All 40 measured
cases passed their applicable correctness checks.

Evidence under `build-remediation/`:

- `frame-performance-baseline/`: saved original source files and original
  benchmark executable (SHA256
  `AAEC342EE8D479D452E897B9CE13433EC38A1AEA4D35EEB50D0428178B6A7413`).
- `frame-performance-compare.ps1`: five alternating run pairs;
  `frame-performance-before-1.log` … `-5.log` and corresponding `after` logs;
  `frame-performance-comparison.json`: all measured statistics.
- `frame-performance.patch` and `frame-performance-files.json`: review patch
  and file hashes against the saved source baseline.

## Regression validation

Release `/W4 /WX` build passed. Full CTest passed **20/20 in 325.45 seconds**
with normal Windows access:

- Native overlay: **2,191 checks**, including **576 exact full/partial GDI pixel
  comparisons** at 100%, 125% and 200% DPI with positive/negative monitor origins,
  off-monitor frames, full-monitor frames, shrinking/expanding regions and label
  clamping. Existing click, double-click, quick-return, fixed-image and shortcut
  checks remain enabled.
- Resource checks: 32 redraws per session, one warmup plus three repeat sessions.
  GDI counts stay constant during warmed redraws and return to **17** after each
  measured session; USER counts return to **4**. The first complete paint cycle
  retained one additional GDI resource (16 → 17), with no subsequent growth.
- Workspace/editor: **641 checks**, including real isolated clipboard publication,
  deferred acquisition, fixation, annotation/mosaic/undo/redo, existing drawing
  damage equivalence and recording configuration retention.
- Frame interaction: **25 checks** for final coordinates, duplicate updates,
  live geometry without image acquisition, deferred island repaint, release
  without a final move and capture loss.
- All recording, media writer, GIF, MP4 trim, preview, completion and audio
  suites pass. WASAPI reports **14 passed, zero failures, zero skips**.

Logs: `frame-performance-release-build.log`, `frame-performance-overlay-build.log`,
`frame-performance-final-ctest.log`, `frame-performance-final-test-details.log`.
The earlier restricted baseline run passed 15/19; four storage-dependent suites
could not create their temporary media sessions in the sandbox. The successful
final run used normal Windows access. No backend workaround or relaxed test
threshold was introduced.

## Measurement limits and subsequent acceptance

- The reusable buffer retains approximately **7.9 MiB per 1080p monitor** or
  **31.6 MiB per 4K monitor** at 32 bits per pixel while the overlay is open.
  This removes allocation churn; it does not reduce idle memory. Destruction
  releases the owned bitmap/DC.
- The island's duplicate size text refreshes at gesture completion. The frame's
  label and capture coordinates remain current throughout the gesture.
- No functional regression was detected in these tests. The user subsequently
  accepted the responsiveness fix and later closed Phase 3.3. This benchmark did
  not validate a broader desktop-compositor, 4K/high-refresh/multiple-monitor
  and mixed-DPI matrix; that coverage belongs to Phase 4 release validation.
  The measurements do not imply 17× application-wide speed or a particular FPS.
- Recording/audio/codec/trim backends were unchanged. Earlier intermittent
  WASAPI/Both tone-test observations are historical evidence, while source-zero
  scan cost for late MP4 trims remains an implementation limit. The UI performance
  work established no backend fix; subsequent manual acceptance is recorded in
  [project status](project-status.md).

The durable rendering and gesture policy is recorded in ADR-015. Project status,
architecture and the affected existing ADRs have been updated.
