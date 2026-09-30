# UI modernization validation report

Workspace availability (2026-09-23): historical build-tree evidence referenced here
was absent before this task; see [current status](../project-status.md). Recorded results below
remain historical.

Historical implementation/test evidence. The current accepted UI baseline and
2026-09-21 session closure are in [project status](../project-status.md) and
[session validation](text-trim-validation.md). The dated checks and review items
below describe that earlier revision; current work is indexed by the roadmap.

**Date:** 2026-09-14  
**Scope:** The user's entire agent-prompt section, applied to the actual Win32
application. All target presentation surfaces are implemented. Validation below
is evidence of specific checks, not a claim that every release gate passed.

## Latest user validation (2026-09-14)

After the agent runs below, the user reported completing manual testing and
passing **all 21 tests** in their build. This supersedes the earlier failing
agent runs as the current reported test status. The user also reported an
apparent WASAPI-related crash with loss of desktop audio; restarting restored
audio. An environment-related explanation for the earlier failures is plausible,
but neither root cause nor causation by Easnap/the tests has been established.

This is user-provided validation, not a new agent execution. No build
configuration, test log or individual WASAPI scenario/skip counts were provided;
do not claim separate Release and Debug 21/21 runs or a new WASAPI 14/14 result.
Manual cases were not itemized. The remaining sections preserve the original
agent results and their coverage limits, including failed menu automation, rather
than relabeling those attempts as passed. The broader Phase 4 matrix remains
unstarted. No source-code fix was made in response to this update.

## Implementation delivered

| Slice | Result |
|---|---|
| U0: repository/reference baseline | Existing settings, coordinator, capture/editor, recording, preview/trim ownership inspected; pre-change source/docs/binaries and SHA-256 baseline saved |
| U1: shared design system | Light/Dark semantic palette, dimensions/fonts, GDI geometry icons, restrained depth, focus/hover/pressed/checked/disabled states; effective system high-contrast override |
| Appearance persistence | Strict Light/Dark INI key, Light for missing/invalid values; tray and recording configuration commands; immediate save of accepted settings without accepting pending recording choices |
| U2: recording configuration | Start above MP4/GIF; independent System audio/Microphone toggles; actual GIF presets and cursor option; compact/narrow reflow; retry error presentation and retained workspace on Start failure |
| U3: live recording | Grip, Pause/Resume, active elapsed time and Stop; compact finalizing status; no fake timer limit or progress |
| U4: workspace/editor | Native mode selector, semantic frame/labels/magnifier/status, native screenshot toolbar and tool properties; acquisition, source pixels, history and fade contracts retained |
| U5: trim/completion | Preview-led responsive layout, distinct In/Out/playhead, native transport/actions/status, three UIA RangeValue children, keyboard handle navigation |
| U6: integration/evidence | Release/Debug builds, full suites, targeted checks, actual desktop automation, rendered native-window images, performance comparison and portable install performed; limitations below remain |

The [design system](design-system.md) maps components to production files.
[ADR-016](../decisions/ADR-016-shared-appearance-and-native-controls.md) records
settings, rendering and accessibility ownership. Only screenshot **toolbar**
presentation moved from D2D/DXGI to GDI/native buttons; the existing image renderer,
single document and compositor remain. Capture/recording/media backend source is
unchanged against the saved baseline. There are no camera, OCR, Scrollshot,
Mouse Effects, device-picker or artificial-success controls.
The final SHA-256 comparison checked 39 files in those three backend directories
and found zero changes (`backend-integrity.json`).

## Environment and reproduction

Windows 10 build 19045, one observed 1920 x 1080 desktop at 125%, MSVC
19.44.35213, Windows SDK 10.0.26100, Visual Studio 2022 x64 generator.
`EASNAP_BUILD_TESTS=ON` and `EASNAP_WARNINGS_AS_ERRORS=ON`.
No Git metadata was supplied; no commit, push, installer publication or release
was attempted. Generated evidence is local, not a required source dependency.

```powershell
cmake -S . -B build-ui-validation -G "Visual Studio 17 2022" -A x64 -DEASNAP_BUILD_TESTS=ON -DEASNAP_WARNINGS_AS_ERRORS=ON
cmake --build build-ui-validation --config Release --parallel 4
cmake --build build-ui-validation --config Debug --parallel 4
ctest --test-dir build-ui-validation -C Release --verbose
ctest --test-dir build-ui-validation -C Debug --verbose
ctest --test-dir build-ui-validation -C Release -R '^ui_modernization_native$' --output-on-failure
ctest --test-dir build-ui-validation -C Debug -R '^ui_modernization_native$' --output-on-failure
build-ui-validation/bin/Release/test_ui_modernization.exe --evidence build-ui-modernization/evidence
cmake --install build-ui-validation --config Release --prefix build-ui-validation/package
```

Desktop/media integration needs normal Windows desktop and protected temporary
storage access. Initial restricted runs could not access the private store.
Checks were rerun with normal access; ACLs and production security were preserved.
The old baseline build directory had a locked test executable after a timeout;
the final build uses the separate directory above.

## Builds and test gates

Logs in this table are under `build-ui-modernization/`.

| Check | Actual result | Evidence |
|---|---|---|
| Pre-change Release build and three baseline checks | Passed; 3/3 in 2.23 s | Baseline snapshot/logs and binaries |
| Final Release / Debug x64, warnings as errors | Both passed | `release-build.log`, `debug-build.log` |
| Initial restricted full Release | 16/21; private-store access failures and timeouts | `release-ctest.log`; superseded where normal-access checks passed |
| Full normal-access Release | 18/21 in 573.67 s; audio preview 180 s, real recording 90 s, initial audio-dependent UI fixture 90 s timeouts | `release-ctest-normal.log` |
| Full normal-access Debug | 20/21 in 493.23 s; audio preview 180 s timeout | `debug-ctest-normal.log` |
| Release targeted recording/workspace/UI retry | 2/3 in 93.55 s; recording still timed out at 90 s | `release-final-targeted.log` |
| Six affected Release tests | 6/6 in 43.12 s | `release-ui-final.log` |
| Six affected Debug tests | 6/6 in 47.08 s | `debug-ui-final.log` |
| Final success-color/playhead-outline checks | Native UI test passed: Release 2.58 s, Debug 5.47 s | `release-contrast-final.log`, `debug-contrast-final.log` |
| Final production-window images and assertions | 0 failures | `ui-final-native.log`, `evidence/` |
| Portable install | Both executables installed; each hash matches its Release build | `install.log`, `package-hashes.txt` |

The six affected checks are `session_state_transitions`,
`trim_workflow_orchestration`, `region_selector_logic`,
`capture_overlay_interaction`, `capture_workspace_production` and
`ui_modernization_native`. Consult the log for exact CTest names if filtering.
The full suites also exercise settings/workflow, drawing/history, recording
cadence and workers, writer/trim export, completion/clipboard/storage, GIF,
preview recovery and audio. A prior dated 20/20 suite is historical evidence,
not this change's result.

The new UI test originally used an AAC fixture and hit the same audio readiness
problem as the dedicated preview test. It now creates a real video-only H.264
fixture through the production writer. The existing audio fixture and dedicated
audio tests remain. This isolates UI validation; it does not establish audio
preview success or hide the failing gate. No production media success was faked,
and no timeouts were increased to turn failures into passes.

### Media results and unresolved checks

- Debug real desktop recording passed **426 assertions**, including static and
  changing desktop recordings, 15/30 fps, pause/resume and GIF working media.
  Three System/Microphone/Both scenarios were skipped for missing endpoints.
- WASAPI reported **7 passed, 0 failed, 7 skipped of 14** in both normal-access
  full runs. The environment exposes no usable system/microphone endpoints to
  those tests; skipped device tests are not passes.
- Audio-preview transport timed out in both configurations. A bounded 45 s run
  of the saved pre-change binary also reported failure to reach native preview
  readiness/duration (`baseline-preview-audio.log`). That is evidence of a
  baseline/environment problem, not a complete diagnosis of all timeouts.
- Release real-recording still timed out. Debug's successful run took 88.20 s.
  The baseline recording experiment was stopped at 45 s with no result; it is
  **inconclusive**, not evidence that the 90 s failure predates this change.
- A later installed-app audio Start probe did not return to the expected
  retryable configuration during the observation period. Only that owned test
  process was terminated so the helper could unwind and restore the original
  settings. Its subsequent assertions are invalid as acceptance evidence
  (`package-failed-start.log`). The real failed-Start/retry interaction remains
  unverified; the test did not establish whether a pipeline stall or another
  cause prevented it. The normal silent desktop flow below did reach trim.

## UI, state and accessibility checks

The new test constructs production windows, calls the real compositor and
queries a real Windows UI Automation client on a COM thread while the owner
thread pumps messages. It does not draw a replacement test-only UI.

- Missing/invalid/case-mismatched appearance keys resolve to Light; valid Dark
  round-trips. Other/unknown INI keys and accepted recording preferences survive.
- Repeated theme changes preserve HWNDs, bounds, focus, configuration, source
  image/composited pixels, undo/redo and provisional acquisition state. A final
  100-switch run retained 14 GDI objects before/after; earlier runs retained
  46/46. Absolute counts depend on the test desktop; growth was zero.
- Mode controls disable at fixation and retire after the existing fade; theme
  repaint does not resurrect them. Existing workspace tests exercise source
  acquisition/clipboard contention, toolbar/padding fixation, drawing tools,
  double-click Copy, second-click drags and real clipboard pixels.
- Native System/Microphone/cursor controls expose Toggle; Start exposes Invoke.
  Timeline exposes three named RangeValue children with finite seconds/bounds,
  mutation through real trim/seek logic, invalid-value rejection and disabled
  states. HWND teardown detaches provider callbacks.
- Compact trim layout retains real control size at 400 x 320 DIP and 200%
  layout cases; 100/125/150/200% coordinate/layout checks are automated. This is
  not physical mixed-monitor DPI validation.
- Text/control contrasts are checked over normal, raised, selected, hover and
  pressed surfaces: primary labels at least 4.5:1, boundaries/focus at least 3:1.
  Representative main/secondary/primary ratios: Light 13.52/6.18/5.02;
  Dark 12.28/6.65/7.47. The playhead has a contrasting outline where it crosses
  the selected interval. High contrast uses system colors; live OS switching
  was not exercised.

### Actual application and native menu limits

A helper connected a fresh thread to the Windows input desktop and interacted
only with the validation executable. It exercised capture workspace, editor,
MP4/GIF configuration, silent Start, Pause, Resume, Stop and arrival at trim
(`actual-desktop.log`). The user's original INI bytes were restored after exit.
The installed package also launched and relaunched successfully.

Appearance checks verified immediate Light/Dark persistence through real host
and panel commands, preservation of pending GIF/preset choices, unchanged
Standby on visual system messages, actual popup opening, accessible Light/Dark
names, and Escape closing the menu without cancelling configuration.

**Menu selection is unresolved:** external MSAA default action and injected
pointer activation did not change the preference, even after the bridge used
per-monitor DPI coordinates and verified the popup under the target point.
Direct panel commands do reach the coordinator. These facts do not prove that
ordinary mouse/keyboard menu activation works, or that the failure is solely
the automation environment. Keep interactive selection, Enter/Space completion,
tray-menu selection and screen-reader operation pending. Logs:
`appearance-integration-routing.log`, `appearance-package-pointer-dpi.log`.
Neither those failed assertions nor the later stalled-Start probe are reported
as passed tests.

## Visual evidence and provenance

Images under `build-ui-modernization/evidence/` were generated in process with
`PrintWindow(PW_CLIENTONLY)` on **actual production HWNDs**. They contain no AI
mockups. The editor's patterned pixels and the trim fixture are synthetic test
content, not replacements for product rendering. Active/error/export states in
these images are driven by the UI harness; real recording was checked separately.

| Surface | Evidence filenames |
|---|---|
| Modes | `capture-modes-light.png`, `capture-modes-dark.png` |
| MP4 configuration | `record-config-light.png`, `record-config-dark.png` |
| GIF configuration | `record-gif-light.png`, `record-gif-dark.png` |
| Retry presentation | `record-retry-light.png` |
| Active / paused | `recording-active-light.png`, `recording-active-dark.png`, `recording-paused-light.png`, `recording-paused-dark.png` |
| Finalizing | `recording-finalizing-dark.png` |
| Screenshot toolbar | `editor-provisional-light.png`, `editor-provisional-dark.png`, `editor-rectangle-light.png`, `editor-rectangle-dark.png`, `editor-mosaic-dark.png` |
| Trim / compact / terminal states | `trim-light.png`, `trim-dark.png`, `trim-compact-dark.png`, `trim-exporting-light.png`, `trim-error-light.png` |

Rendered images were visually inspected; clipped Copy text, retry wrapping and
client capture offsets found during inspection were corrected. The preview area
is black in PrintWindow images because this method does not retrieve the helper's
composited video. Cross-desktop external captures (`app-*.png`) were entirely
black, including a non-excluded trim window, so they are **not usable visual
evidence** and do not demonstrate capture exclusion. Capture-exclusion behavior
must be confirmed interactively with real exported pixels/video.

## Frame performance

Five alternating pre-change/final native replay pairs at 1920 x 1080 used the
same move/resize sequences. Values below are medians across the five run metrics,
in milliseconds; `frame-comparison.json` retains individual results.

| Operation | Before median | After median | Before p95 | After p95 |
|---|---:|---:|---:|---:|
| Screenshot move | 0.370 | 0.376 | 0.450 | 0.495 |
| Screenshot resize | 0.368 | 0.373 | 0.475 | 0.486 |
| Record move | 0.371 | 0.372 | 0.431 | 0.444 |
| Record resize | 0.371 | 0.371 | 0.471 | 0.462 |

Each sequence retained 160 updates and identical damage totals: 5,956,340 pixels
for move, 6,099,378 for resize. Median changes are small in this workload;
Screenshot-move p95 rose 0.045 ms. This native input/paint replay is not a physical
mouse latency measurement or a universal frame-rate guarantee.

## Remaining environment coverage and handoff

Not completed: physical 100/150/200% and mixed-DPI monitor transitions,
negative-origin/multi-monitor/4K/high-refresh layouts, Windows 11, live system
high contrast, screen readers, additional GPU/audio devices, capture exclusion
of all popup/tooltip states in output, and broad external clipboard targets.
Existing automatic tests provide narrower coverage, not substitutes for this
matrix. The audio/recording and native menu concerns above remain explicit.

Final portable output is `build-ui-validation/package/bin/` with **Easnap.exe
and EasnapPreviewHost.exe together**. This validates the existing install path;
it is not Phase 4 packaging/release work. Source/docs changes and unchanged
backend checks are recorded in `change-manifest.json`, `review.patch` and the
documentation validation artifacts under `build-ui-modernization/`.
Documentation validation passed for 31 Markdown documents, 83 local link/image
targets and one heading anchor, with no broken targets, invalid UTF-8 or
unbalanced fences. The evidence manifest verifies 22 rendered PNGs and both
installed executable hashes.

[Project status](../project-status.md) and the [roadmap](../../EASNAP_ROADMAP.md)
preserve Phase 3/3.3 acceptance on 2026-09-14. Phase 4 remains unstarted and is
the next bounded development slice; its validation must use this final UI.
Scrollshot remains after Phase 4/v1.
