# Trim endpoint artifact repair — 2026-09-29

## 30 September after restart: continuous timeline seeking

**Current checkpoint: available same-build checks finished; seek/endpoint checks
pass, strict resize gates fail, and user acceptance is pending. Phase 4 has not
started.** The user restarted the computer
and reported that both the old backup executable and the latest Release
executable now resize without the previously visible dragging defects. An
unchanged Release paused top-left repeat on SAMSUNG then captured **453 frames
with zero detected defects**. This is a clean diagnostic subset, not a new
all-edge/full-state pass. Restarting can change graphics/session state, but the
cause of the earlier environment-dependent behavior is not established.

Before the restart, the separately authorized Win+Ctrl+Shift+B shortcut was
injected once. Injection does not establish that a graphics reset completed or
that it resolved anything. The following unchanged Trim comparison still failed
both cases: **464 frames, 149 mixed observations and 42 exposed-background
observations**. The independent native-window post-shortcut recording was
interrupted by the user before full interpretation; it supplies no resolution
claim. These results distinguish that diagnostic from the later full restart.

The user's new report concerns playing a 6–10 s recording and repeatedly
clicking different timeline points before playback ends: a single click can
restart twice, and prolonged interaction can briefly remove the picture and
disable controls. The new user recording had been temporary and was deleted;
the original 6.710 s source is also absent from this checkout. New validation
therefore uses generated real media, without claiming a rerun of either absent
user recording. The retained final frame, logical/exclusive Out, replay from In,
paused-first-frame opening, native window geometry and existing export behavior
remain required.

### Demonstrated causes and focused corrections

1. **One pointer gesture issued duplicate seeks.**
   `TrimWindow::HandleTimelineMessage` called `UpdateTimelineFromPoint` on
   press, captured moves and release. Its Seek branch always declared a change.
   A stationary press could therefore seek, allow playback to advance, then
   seek back to the same point again on release. The correction remembers the
   last requested target only within that pointer gesture. Identical targets
   do not issue another seek; changed drag targets and a new click at the same
   point still do. Press/release, capture loss, Hide and Destroy clear the
   gesture state. Keyboard and UIA retain their direct seek path.
2. **The helper could mistake completed backward seeks for stalled playback.**
   `PreviewLiveness::Observe` previously reset its playback baseline only when
   a periodic observation saw a pending operation. A real Pause/Seek/Play cycle
   can finish entirely between two 100 ms observations. The old higher native
   position then remained the baseline after a legitimate backward seek;
   playback below it could trigger a false two-second stall. Recovery replaces
   the helper, accounting for the temporary missing picture and disabled view.
   The correction recognizes a changed actual native command sequence even in
   a settled observation. Accepted user input alone cannot reset the deadline.
   Native command ownership, genuine missing-callback/stall detection,
   generation checks and the existing two-second bound are retained.

Microsoft documents MFPlay's asynchronous operation/completion model and
creating-thread callbacks in
[Getting Started with MFPlay](https://learn.microsoft.com/en-us/windows/win32/medfound/getting-started-with-mfplay).
The correction follows that model; it does not invent a native completion or
add a delay. The durable contract is in
[ADR-010](decisions/ADR-010-trim-view-ownership-and-orchestration.md), and required
implementation/regression steps remain in the existing
[window guide](ui/window-implementation-guide.md).

### Before/after evidence and test-input corrections

| Check | Focused result or initial run |
|---|---|
| Deterministic transport/health regression | Before: 510 failing assertions. After: **210,363/210,363 assertions pass**. Four fixed seeds each complete 80 real transport-policy Pause/Seek/Play cycles between observations; the tests also retain exact stall and missing-callback deadline failures. |
| Real 8 s H.264/AAC continuous-seek seed 0 | Before: false `liveness-timeout=2` at diagnostic input index 108 despite native Playing state and advancing playback. After: **30 s, 131 seek requests, 67 backwards, no helper replacement**, maximum observed command age **188 ms**. This first seed does not establish a pass for all three seeds. |
| Timeline duplicate-seek baseline | Across 12 held gestures in 6 s/10 s media and both themes: **24 same-coordinate move/release failures**, plus **one readiness assertion** (25 total). The readiness failure is not a duplicate-seek defect. |
| Aligned-pointer corrected quick proof | All **12 gestures across four media/theme cases** pass. Playback advances while held; repeated identical moves and release do not reset its position. Changed targets and later same-point clicks still seek. This is a focused proof, not the complete stress matrix. |
| First corrected full UI stress | **288 injected click attempts** retained healthy sampled video, enabled controls, requested Play and helper identity. The run nevertheless failed **19 assertions**: 18 later quick-proof assertions involved inconsistent actual/sent pointer coordinates; one did not confirm asynchronous capture at 20 ms. Preserve this failing result rather than reporting every attempted gesture or the run as passed. |
| First combined Release suite | **35/36** passed. The new timeline stress test faulted during WGC observer shutdown; preserve this failed run separately from later observer/readiness corrections. It is not a final suite pass or user acceptance result. |

The quick proof sends production native timeline messages to isolate down/move/up
effects. Mouse capture can also synthesize a real move using the physical pointer
position. The revised test aligns and checks that position against its sent
coordinates and drains the previous gesture before starting another. Native
click stress now waits up to **250 ms for actual capture ownership**, still
asserting failure if ownership never arrives. These are bounded test-input
preconditions, not production sleeps, looser picture comparisons or permission
for a visible interruption. All media/control/helper and responsiveness
assertions remain active. Final results belong to the aligned test and the same
production binaries used for endpoint/resize regression.

The older `TestPreviewLivenessDeadlinesAndNativeProgress` fixture was also
corrected to retain native sequence 11 after sequence 11, instead of supplying
sequence 1. Production observations are ordered within one generation; that
backwards fixture sequence did not model a stalled command epoch. Its timeout
times and expected failures are unchanged. The new regression separately checks
completed epochs between samples and that repeated input cannot postpone a
genuinely owned command's deadline.

The new registered `trim_timeline_seek_stress` exercises the real Trim and helper,
generated 6 s/10 s media, both themes and continuous native random clicks without
a test-inserted pause/restart. Three registered backend checks,
`mfplay_preview_continuous_seek_0`, `_1` and `_2`, exercise separate deterministic
H.264/AAC input sequences. Backend health, timeline request ownership and actual
desktop presentation are distinct checks; none substitutes for the others.

### Observer shutdown and consuming-readiness findings

The first combined Release suite finished **35/36**, with all other checks
passing. The new timeline stress process suffered an execute access violation
at an address in unloaded `GraphicsCapture.dll` during the WGC observer's
asynchronous shutdown. This is an observed test-observer failure; it does not
establish a product playback failure or a successful full run.

The registered `trim_timeline_seek_stress` now selects the already available
DXGI observer through `EASNAP_TEST_CAPTURE=duplication`. A direct
`test_ui_modernization --seek-stress` invocation still defaults to WGC unless
that environment variable is set. Picture/blank-frame thresholds, helper
identity, control state and responsiveness checks are shared and unchanged.
No production rendering change, DLL pin or teardown sleep was added. Standard
endpoint and resize validation continue to exercise WGC, so this choice does
not remove that presentation path from the regression set.

The shared test `WaitFor` also re-evaluated its predicate after observing true.
A capture-readiness predicate consumes a frame; a second call can find an empty
queue and incorrectly report preparation failure. It now returns immediately
on the first true result, preserving the timeout and all authentication checks.
Together with physical-pointer/foreground checks, this corrects input/evidence
ownership rather than relaxing defect detection. These corrections require
their own rerun; they do not convert the recorded 35/36 result into a pass.

### Same-production-build results after observer/input corrections

Evidence manifest: 30 September seek repair (`evidence/trim-seek-repair-20260930.json`, retained locally).
Local full stdout, CSVs, actual monitor PNGs and per-frame geometry remain under
`.local/seek-repair-20260930` and the registered test's build evidence directory.
The matching Release application/helper remain in `build/bin/Release`; no new
distribution package was installed. **User acceptance remains pending.**

- Final affected Release rerun: **4/4**, 259.84 s, including native UI,
  endpoint presentation, timeline stress and material parity. The final
  authenticated pointer test completed **288/288 clicks**, **7,799 desktop
  frames**, zero blank frames/disabled controls/lost Play/helper replacements,
  maximum observed picture freeze **63 ms** and message-pump interval **32 ms**.
  The four endpoint playback/replay cycles captured **2,488 frames**, with zero
  damaged samples. This does not replace the failed initial full 35/36 run
  with a fictitious second full 36/36 invocation.
- Affected Debug: **15/16**, 597.35 s. The only failed check is timeline stress:
  both 6 s cases abort because actual pointer coordinates differ from the
  intended stationary point by 2–13 pixels per coordinate. They provide no stress coverage.
  Both 10 s cases complete 144/144 clicks and 3,990 frames with no blank video,
  disabled controls, lost Play or helper change. The three new backend seeds
  pass (389 seeks, 197 backwards, maximum native-command age 203 ms). Debug
  endpoint captures 1,865 undamaged frames. After renewed confirmation that the
  pointer is idle, the **unchanged focused repeat passes 1/1**, 167.37 s:
  all four 6/10 s/theme cases, **288 clicks, 7,888 frames**, zero blank video,
  disabled controls, lost Play or helper change; maximum picture freeze **78 ms**
  and pump interval **32 ms**. No second full 16/16 invocation is claimed.
- Separate **120 s continuous backend run passes**: 539 seeks, 267 backwards,
  227 requests while a command is owned, maximum command age **203 ms**,
  unchanged process/generation and zero failures. Resource samples stay within
  the test's existing bounds (handles +64, GDI/USER +8, private memory +64 MiB),
  then final seek convergence, Pause, exclusive Out and replay checks pass.

The separate resize gate remains strict. These complete runs actually exercise
all four edges and all four corners, caption motion and maximize/restore, both
themes, native paused/playing video and retained endpoint on SAMSUNG at 125%.

| Run | Cases | Desktop frames | Visual-failing cases | Other assertions | Settled invalid / samples |
|---|---:|---:|---:|---:|---:|
| Release fast | 60 | 14,726 | 22 | 5 minimum-width | 0 / 1,193 |
| Debug fast | 60 | 12,854 | 16 | 1 minimum-width | 0 / 1,060 |
| Release slow | 60 | 23,870 | 10 | 4 minimum-width | 0 / 1,149 |
| Visible status/progress/Cancel | 40 | 9,645 | 11 | 2 minimum-width | 0 / 783 |

Together: **220 cases, 61,095 desktop frames, 59 visual-failing cases and 12
minimum-width assertions**. All **4,185 settled samples** are clean. Status,
progress and Cancel state/callback assertions pass; their during-motion visual
gate does not. No case failed preparation or produced zero desktop frames.

None of the completed resize runs has a capture error or a failed post-input
resize-work drain assertion. Maximum measured dispatch is 47 ms across them.
That responsiveness result does **not** permit a transient visual defect.
The minimum-width failures remain failures; no unproven production minimum-size
cause or successful clamp is inferred from them.

Actual failing PNGs were opened: paused top-right motion shows native video
briefly overlapping the timeline; retained-endpoint right resizing clips
right-side controls/readout; maximize shows displaced video and duplicated
caption pixels. These findings extend beyond three sampled patches. The gate
forces `DWMWA_TRANSITIONS_FORCEDISABLED=TRUE` and observes four maximize/restore
transitions (two cycles). The endpoint test maximizes/restores **before** its
pixel observation. Microsoft documents the flag as forcibly disabling DWM
transitions ([DWMWINDOWATTRIBUTE](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute));
it does not establish the cause of these Easnap captures. The gate was not
weakened and no new rendering workaround was added during the seek correction.

Resource logs retain the same owner/helper PIDs through each matrix. Handle/GDI
samples stabilize late in the fast runs, but private memory rises in the slow
run (owner about 65.1 to 86.6 MiB; helper 68.4 to 90.6 MiB). This does not prove
indefinite leak freedom or locate that growth in production versus observer
allocations. Physical 100%/150% and mixed-DPI transitions remain unexecuted.
The absent user recordings and the exact pre-restart desktop/driver cause also
remain unavailable. The reboot report is preserved as the user's observation,
not a claim that the complete strict resize gate passes.

## Historical pre-restart continuation: resize and endpoint validation together

The following sections preserve the earlier investigation and test results.
Their references to a "current", "latest" or "final" build refer to that dated
checkpoint, before the restart and continuous-seek corrections above. They are
not new failures or passes attributed to the current seeking build.

User acceptance remains pending. The current repair includes every trim control
and the supervised preview; it does not start Phase 4. Preserve the 480 x 400 DIP
client minimum, native-frame allowance, work-area handling, fonts/input targets,
existing fixed-dialog/island interaction and deliberate tool hiding during
capture-frame interaction. No whole-window scrolling or responsive redesign is
authorized. Other surfaces change only for a demonstrated defect.

Initial change audit, before further rendering changes:

| Implicated change | Disposition | Basis / remaining verification |
|---|---|---|
| ADR-024 native side/bottom frame and caption-only customization | KEEP | Accepted native behavior; current parent code matches the pre-endpoint copy. Verify every edge/corner again. |
| Trim deferred child layout and shared owned-thread paint completion | KEEP, measure | Do not replace with blanket copy/erase flags. Correlate completed layout with actual desktop frames. |
| Bounded preview resize/intent transport and generation/recovery checks | KEEP credit/recovery; REWORK timer-only wake | One outstanding resize remains. A reproduced timer-starvation case now uses one coalesced posted poll; see before/after evidence below. |
| Exact endpoint selection, logical/exclusive Out, paused first frame and replay | KEEP | Required outcomes; run endpoint and resize checks on the same final binaries. |
| Layered retained-still surface and explicit submission | KEEP pending causal measurement | Endpoint evidence supports this change; resize interaction and duplicate paint cost are not yet cleared. Rework is authorized if evidence requires it. |
| Three-patch all-edge observer and evidence naming | REWORK | Insufficient control coverage; paused/held artifacts overwrite each other; first mixed frames can consume the evidence budget before green/settled failures. Preserve failure thresholds while diagnosing. |
| Earlier rejected continuous backing, pause conversion and top-level flag experiments | REVERT already reflected in current source | Not present in the delivered implementation; do not reintroduce them as unmeasured workarounds. |

This audit is not a passing test result. Historical results below remain dated
evidence; the current resize repair remains open.

### Before restart, 30 September: observer-free physical confirmation and isolated reference

The user watched `samsung-readout-no-observer.log` on SAMSUNG with **neither
screen recording nor the pixel comparator running**. They confirmed visible
defects at the left edge, top edge and top-right corner; they did not see them
at the right/bottom edges or bottom-right corner, and were unsure about
bottom-left. Do not infer a pass for the other directions. Both themes and all
eight directions were exercised. Exit 77 means only the nonvisual diagnostic
checks succeeded; it is not a visual pass. This establishes that a recorder or
pixel comparator is not necessary for the reported Trim defect.

That run reached the native minimum in all 16 edge/corner cases, completed
20 cases including caption movement/maximize, and recorded a maximum 47 ms
resize dispatch. Owner GDI/USER counts stayed 18/36; handles ended at 450 versus
451 initially. Helper working set went from about 68 MB to 70 MB and private
memory from 73 MB to 84 MB, with stable final samples. This bounded run does
not establish long-duration leak freedom.

A second isolated reference uses only public Win32/GDI: standard native frame,
no child HWNDs, no custom caption, no `WS_EX_COMPOSITED`, no Easnap includes,
no retained bitmap cache and no media. Each paint allocates a memory bitmap,
fills the whole client, draws four anchors, and BitBlts before EndPaint. It
invalidates on size and completes paint after default WINDOWPOSCHANGED handling.
The same external SAMSUNG DXGI capture shows clipped right anchors during fast
top-left resizing (`native-plain-fast-01.png` / `02.png`, source frames 370/376
of `samsung-native-plain.mkv`). All four injected drags entered native sizing.
The local source is `native-plain/native_resize_probe.cpp` under the evidence
directory. This narrows the comparison beyond Easnap's shared renderer; it does
**not** identify a Windows/driver fault or prove that no application fix exists.
The offline locator's 273 suspect frames are not 273 confirmed defects.

`TrimWindow::UpdateLabels` now measures changed readouts using a memory DC,
matching `LayoutControls`, with the actual UI font. The previous GetDC/ReleaseDC
pair acquired a composited window DC for measurement; releasing it is a
presentation boundary even when the caller intended no drawing. This removes
that unnecessary boundary. The observer-free run above is **after** this change
and still visibly fails: it is not the complete resize correction.

The first status/progress run had 12 reference-preparation failures and 13
foreground/ownership failures. Its dark bottom status reference PNG actually
contains the editor desktop rather than Trim: current WindowFromPoint ownership
cannot authenticate an earlier capture frame. Preparation now additionally
matches the captured caption pixels against the owned caption's real painter,
before motion begins. The three-second readiness limit and strict moving-frame
comparisons remain; printing never touches foreign video or occurs during
observed motion. This prevents invalid references; it does not excuse genuine
resize artifacts. The earlier Debug playing/light/bottom-left settled mismatch
is confined to the Save patch; its precise material/hover cause remains
unclassified and is not counted as a demonstrated layout defect.

Before the new caption-readiness check, the status/progress repeat completed
40 cases with 36 visual failures: 9,330 frames, 2,102 mixed observations,
375 exposed-background observations and 708 clean settled samples. Maximum
dispatch was 47 ms. Two native-minimum assertions still failed. Production view
checks for disabled timeline, visible progress/Cancel, cancel callback,
cancelling state, recovery to editable controls and Save/Copy/Discard callbacks
did not fail. These callback checks do not execute the export backend.
Evidence stems are `samsung-readout-controls` and `-repeat`.

The caption-authenticated current run also has directly inspected damage:
`samsung-readout-release-fast/paused-all-edge-0-top-left-frame-28-mixed-1-holes-324-settled-0.png`
shows exposed green background inside the right/bottom client edges while the
native frame already has its new dimensions. This is visible missing backing,
not merely a mismatched pixel hash. The title reference in this run is checked
against the actual caption painter before the drag. The separately observed
standard native reference demonstrates clipped anchors rather than this exact
green-backdrop case; do not equate all failure modes with one proven cause.

### Historical same-build validation before restart, 30 September

Both Release/Debug builds succeeded with warnings as errors. The same production
binaries were used for endpoint, transport and resize checks below. The test
executables also contain the caption-reference authentication correction.
The source/binary/result manifest (`evidence/trim-window-repair-20260930.json`, retained locally)
records SHA-256 hashes and individual cases; no new dist package is installed.

| Check | Actual result |
|---|---|
| Full Release CTest | 31/32, 515.45 s. Native UI's first light/playing drag did not start; its five assertions are not five demonstrated pixel defects. All other registered tests passed, including endpoint presentation (54.75 s). |
| Affected Debug CTest | 11/11, 167.14 s; endpoint presentation passed in 58.00 s. |
| Release fast eight-direction gate | 55/60 visual case failures; 14,555 frames, 2,974 mixed and 737 exposed-background observations; 1,187 clean settled samples; maximum resize dispatch 47 ms. One initial foreground/ownership assertion and three native-minimum assertions also failed. |
| Release slow eight-direction gate | 54/60 visual case failures; 23,930 frames, 5,033 mixed and 17 exposed-background observations; 1,213 clean settled samples; maximum dispatch 47 ms. Five minimum assertions also failed. |
| Debug fast eight-direction gate | 54/60 case failures; 11,404 frames, 2,537 mixed and 399 exposed-background observations; 991 clean settled samples; maximum dispatch 47 ms. Three light/paused cases (right, bottom-right, bottom) had zero captured frames because reference preparation failed; their visual/settled assertions are unavailable coverage, not observed defects. Four ownership and two minimum assertions also failed. |
| Release authenticated status/progress gate | 36/40 visual case failures; 9,815 frames, 2,118 mixed and 412 exposed-background observations; 809 clean settled samples; maximum dispatch 63 ms. Three minimum assertions also failed. No foreground, preparation, capture or control-callback assertion failed. |

Each full directional run exercises four edges, four corners, caption movement
and maximize/restore in both themes. The 60-case gates include native paused,
retained endpoint and playing preview; the 40-case control gate includes status
and progress/Cancel. Preparation failures and minimum misses are retained as
failures, not silently removed. The Release native UI repeat on the same binary
completed with zero assertions (`readout-native-ui-repeat.log`), including the
previously unstarted first gesture. Its maximum native dispatch was 47 ms;
one 109 ms health-response timeout remains diagnostic, below the one-second
dispatch bound. This does not replace the full 31/32 result or the separate
all-edge failures.

The three missing Debug paused directions were repeated individually in both
themes (`readout-debug-repeat-right`, `-bottom-right`, `-bottom`). All six
actually captured and resized (868 total frames), and all six failed the visual
comparison, with zero capture errors. The light/right minimum assertion also
failed again (1181 rather than 618 pixels); other repeated minima were reached.
That input/minimum discrepancy remains unclassified, not silently waived.

No directional run exceeded the one-second dispatch limit or the assertion
against additional resize messages after the post-gesture drain. The passing
backpressure checks cover bounded coalescing and documented delayed-poll
recovery; these are not a promise of uninterrupted rendering. No test crash
occurred in these latest runs. Resource samples and the observer-free bounded
run are retained; long-duration leak freedom is not established.

Actual desktop DPI was 120 (125%) on SAMSUNG. Other physical DPI settings,
mixed-DPI transitions, other Windows/GPU environments, and the absent original
6.710 s source were **not** rerun. Pure geometry/font tests do not substitute
for that physical matrix. At this pre-restart checkpoint, user acceptance was
pending and the main resize defect remained unresolved.

### Continuation measurements (not acceptance)

The checkout had no `build/`, `dist/` or `bug-video-footage/` at the start of this
continuation. A fresh VS 2022 x64 Release build with warnings as errors was made.
The original 6.710 s source has not been rerun in this continuation; historical
results below do not establish a pass on the current build.

At the user's request, subsequent desktop tests target **SAMSUNG**, currently
`\\.\DISPLAY2`, physical bounds `(1920,0)-(3840,1080)`, 120 DPI. The desktop is
extended; ASUS is DISPLAY1. `EASNAP_TEST_MONITOR` selects an explicit active
display for the trim/endpoint/lifetime checks and fails if that display is absent.
The preview fixture, preview startup occluder, recording integration surface,
recording island and editor input coordinates now use that selection too.
Capture overlays intentionally span both displays; the user explicitly
authorized those tests. Overlay/workspace test entry points now opt into PMv2
before creating HWNDs. The previous DPI-unaware overlay fixture reported
virtualized 96-DPI bounds and failed its fade check; the PMv2 rerun reported
120 DPI and passed. This is a test-environment correction, not an overlay change.

The fresh `--resize-edges` baseline on SAMSUNG covered all four edges and four
corners, caption movement and maximize/restore, in Light/Dark and native-paused/
retained-endpoint states: **40 cases, 9,338 captured frames, 1,068 mixed-geometry
observations, 436 exposed-background observations, zero capture errors, 32 failed
case assertions**. All 777 settled observations were clean. No input/foreground
assertion failed in this full run. Right-edge and caption-move cases passed; the
other directions and maximize/restore failed during motion. This is not a pass,
and does not replace the still-required final-build resize/endpoint validation.
Evidence: `.local/window-repair-20260929/samsung-full-before.log` and its matching
PNG/JSON directory. PNGs include the full desktop, not only the measured patches.

The following controlled top-left diagnostic comparisons did **not** solve the
defect and were reverted. They are evidence, not suggested implementation steps:

| Comparison | Evidence stem under `.local/window-repair-20260929/` |
|---|---|
| Remove Trim's composited style | `probe-no-composited` (before the monitor request) |
| Hold a DC only across layout/owned painting | `probe-dc-scope` (before the monitor request) |
| Release the preview entirely | `samsung-layout-timing` |
| Use the complete default native caption/frame | `samsung-native-frame-probe` |
| Use a 32-bit DIB paint buffer | `samsung-dib-probe` |
| Flush GDI after the complete resize paint traversal | `samsung-flush-probe` |
| Flush the final BitBlt before EndPaint | `samsung-present-flush-probe` |
| Reverse the owned-control paint traversal | `samsung-paint-order` |
| Let Windows paint the whole subtree, with no foreign children present | `samsung-subtree-probe` |
| Allow automatic redraw in child placement | `samsung-child-redraw` |
| Empty valid rectangles / preserve the screen intersection | `samsung-validrects-probe`, `samsung-preserve-screen-probe` |
| Wait for DWM presentation | `samsung-dwm-probe` |
| Keep normal DWM transition settings | `samsung-default-transitions` |
| Let normal WM_PAINT dispatch follow positioning | `samsung-deferred-paint` |
| Invalidate old geometry before positioning | `samsung-old-damage-probe` |
| Disable DWM nonclient rendering (diagnostic only) | `samsung-nc-diagnostic` |
| Hold a DC from WINDOWPOSCHANGING through completed painting | `samsung-dc-transaction` |
| Suppress automatic top-level redraw and explicitly repaint after layout | `samsung-top-noredraw` |
| Declare horizontal/vertical class redraw styles | `samsung-class-redraw` |

The paint-order dark drag did not start; the flush/paint-order runs also recorded
foreground interference. Those runs cannot establish complete directional
coverage. The user subsequently reserved the pointer/keyboard for the full run.
No timing delay, frame flag or alternate buffer from these trials is retained.

Releasing the real preview leaves the top-left defect reproducible: native video
and endpoint presentation are not necessary causes of this resize failure.
Full-frame inspection shows old-size content translated to the new origin and,
in shrinking bottom-right captures with video, clipped/duplicated controls and
black fragments. The precise production cause is still under investigation.

The user also confirmed seeing the jumps/clipping on the physical SAMSUNG
display. The `samsung-never-opened` run reproduced it when the preview helper
was never created. Endpoint retention is therefore not a necessary cause.
Physical observation rules out a comparison-only false positive; it does not
rule out the observer inducing a visible presentation disturbance. These
observations do not identify a proven replacement
for the accepted native-frame policy.

The observer was extended from three to eleven patches: title, range, both
Set buttons, Play, Save, Copy, Discard, close/minimize glyphs and duration.
Full desktop PNGs expose clipping that the original three patches missed,
including the close button and right-aligned duration during right-edge shrink.
The newer minimum-size exercise also exposed two observer errors: overshooting
the native minimum could release the pointer over a button, and a centered
patch on a wide icon button was blank but contained its left-aligned label at
narrow widths. The final observer returns the pointer to the real frame before
release and follows the actual left-aligned/centered text anchors. Pixel
tolerances and the zero-defect gate remain unchanged. The earlier
`samsung-fast-current` settled failures cannot all be classified as application
defects; retain their PNGs as diagnostic evidence, not a final result.

`EASNAP_TEST_RESIZE_SLOW=1` selects a 3,200 ms shrink/grow cycle, versus four
400 ms cycles in the default fast case. Both reach and assert the native minimum.
The gate now covers native paused, retained endpoint and native playing states;
playing cases seek Out and replay from In through the real controls. It retains
full settled frames, logs owner/helper resources, requires dispatch below one
second and verifies that resize messages stop accumulating after input ends.
The corrected-anchor slow top-left subset failed in all six state/theme cases,
while its settled frames passed. This subset is not the full directional gate.

After reverting experimental source copies, force recompilation when a restored
timestamp predates its object file. One restored `native_controls.cpp` was
initially skipped by incremental MSBuild; the relevant source timestamps were
refreshed and compilation was verified in `build-restored-release.log` before
the later current-build checks. Trial logs are rejected experiments, not proof
that an alternative is correct or that the historical accepted binary regressed.

The observer now keeps separate paused/endpoint evidence, representative mixed,
hole and settled failures, reference frames and geometry JSON; its strict
thresholds are unchanged. Timing probes measure owned paints and layout. An
optional `EASNAP_TEST_CAPTURE=duplication` comparison observes DXGI desktop
presentation. Its first top-left run also showed clipped old geometry, but no
green-hole observations; it supplied no new frames after settling. That run
failed, including the existing settled-frame assertion. It must not be reported
as a pass or used to dismiss the WGC findings. Broader visual coverage and
capture timing correlation remain unfinished.

A separate concrete lifetime defect was reproduced by `--window-lifetime`:
destroying the owner of a configuring recording island destroyed its HWND while
the C++ view retained its old handle. The baseline failed its retirement check.
`RecordingControls::Window::HandlePanelMessage` now clears panel/control handles
on `WM_NCDESTROY`; reopening recreates the view. `CaptureCoordinator::Shutdown`
and `OnDeferredCleanup` retire dependent islands before the overlay owner.
Twelve real owner-destroy/reopen cycles passed in Release on SAMSUNG, with GDI
objects fixed at 13 and USER objects at 8 after every cycle
(`Release-lifetime-resources.log`). The Release native UI check also passed.
Before the later resize-poll/readout corrections, full Release passed 32/32 in
527.23 s, and affected Debug passed 11/11
in 163.27 s, including native UI/lifetime, endpoint, startup, backpressure,
workspace, frame, overlay, annotation and material checks. Logs/JUnit files are
`Release-current-full` and `Debug-current-affected` under the same evidence root.
These suites do not contain the opt-in resize gate. This is separate from
the unresolved trim visual defect; island interaction was not redesigned.

### Expanded matrices and supplied old backup

The `samsung-fast-final-observer` and `samsung-slow-final-observer` runs each
exercise 60 cases: ten directions/transitions, two themes and three media
states. Fast: 14,649 frames, 2,651 mixed and 629 exposed-background observations,
1,173 clean settled frames. Slow: 23,880 frames, 4,467 mixed and 26 exposed-
background observations, 1,171 clean settled frames. Each fails 54 visual cases;
caption movement passes. Neither reports capture/input errors; maximum measured
resize dispatch is 47 ms, with no post-input resize-work assertion failure.
Those runs also report 12/16 minimum-size assertions. A later measurement moves
the minimum read before pointer return, but some assertions still fail; actual
reached dimensions are now logged. These failures are not yet classified as a
production minimum-size defect. Owner resource counts include the in-process
capture observer; their growth cannot yet be attributed to production UI.

The user supplied a local backup checkout. Its production sources
match `.local/hardening/source-baseline.zip` after newline normalization; the
archive's status records 21 September acceptance. Its unchanged Release
`test_ui_modernization.exe` with adjacent original helper was run using its
original `--resize-edges` observer. A separate placement driver moved the Trim
during its initial preview-ready wait to SAMSUNG, before monitor capture and
dragging. All later rectangles remain on SAMSUNG, allowing the normal nine-pixel
maximized native frame outside physical bounds. The backup was not modified.
Result: **15/18 visual cases fail**, 4,060 frames, 679 mixed observations, 310
holes, zero capture/input failures, 365 clean settled samples. Evidence:
`samsung-user-backup-original-observer.log`, PNGs and placement JSON. Direct PNG
inspection shows clipped duration/actions and a missing Close glyph during
left shrink. This is a result under today's conditions, not a retraction of
historical user acceptance. The backup's own docs name a separate accepted
portable directory absent from this backup; do not claim these hashes match
that missing package manifest.

Backup Release SHA-256: Easnap `caafce016199650bb1b92f969d71334dcf1df5244c9875ca880bb0e3ad223cc4`;
helper `d74237efa728ef430c9f93ce975102952641bd00e43d96b4fd255ee455a8fe65`;
test `8c0bc032b7bbb935729be8c194fc77337897fa5405e8e7366dd19f918cdf507b`.
The actual run used the test executable and helper, not Easnap.exe itself.

The later `samsung-user-backup-real-app` comparison **did** use the unmodified
backup `Easnap.exe` and its adjacent helper. Its normal capture/record/Stop flow
created a fresh 7.106 s recording, then four real drags (slow/fast top-left,
slow right and fast bottom) were recorded externally through DXGI. Every drag
confirmed foreground ownership and the native sizing loop. The fresh recording
was discarded through the UI, the test-owned app exited and the original settings
bytes were restored. No in-process observer or test backdrop was installed.
Offline screening found 38 suspect label frames among 1,321; this is only a
locator, not a visual pass/fail oracle. Direct inspection of
`samsung-user-backup-real-app-incomplete-363.png` shows the caption buttons,
duration and lower actions clipped at the new bounds. Thus the unchanged old
main application also exhibits the pattern under today's SAMSUNG conditions.
The user clarified that the historical successful manual test was on ASUS.

Source comparison (`user-backup-window-source.diff`) found the native frame,
Trim layout, theme and ordinary PaintBuffer operations unchanged from the
backup. The one Trim playback change handles the Ended snapshot to preserve
logical Out. A controlled candidate moved the backup's exact out-of-line
PaintBuffer constructor/Present/destructor into one shared source file, while
keeping the current layered still submission. All six top-left cases still
failed (`samsung-backup-paint-combined`). This structural candidate was reverted;
moving identical operations between a header and source is not an established
repair. The current endpoint behavior remains intact.

The desktop reports an NVIDIA GeForce RTX 4060 Ti, driver 32.0.16.1088; ASUS
runs at 144 Hz and SAMSUNG at 60 Hz. A controlled 144/60/restored-144 Hz primary
display comparison kept Trim on SAMSUNG. Both light/dark paused top-left cases
failed in all three runs, with valid native minimums and clean settled samples.
ASUS was restored to 144 Hz successfully; no permanent mode change was saved.
Evidence: `refresh-comparison.json` and `samsung-refresh-primary-*.log`.
Matching refresh rates did not fix this case; these observations do not identify
a driver or OS defect.

With the user's explicit exception to the SAMSUNG-only test location, a short
ASUS comparison rebuilt the archived old production sources with the same
current observer used for the current sources. Both paused top-left theme
cases failed visually in each build (`asus-old-source-paused`,
`asus-current-paused`). All settled samples were clean. The old-source dark
case also missed the minimum-size measurement, so it is not complete clean
geometry coverage. These runs do not reproduce a passing old/new failing pair
under today's conditions. Subsequent regression tests return to SAMSUNG.

An isolated rebuild of the old production sources with the current observer
also fails the paused top-left subset in both themes. One initial foreground
assertion failed in that archive run, so it is not complete clean input coverage.
The new `--trim-observe` diagnostic fixture keeps production Trim paused for
60 seconds with no in-process capture/comparator/geometry probe. External
recording/input comparisons are in progress; this fixture alone is not a visual
pass. Current automatic matrix thresholds have not been relaxed.

External recording now establishes that the in-process observer is not required
for the clipping: `samsung-external-current.mkv` was recorded by a separate
FFmpeg GDI desktop capture while `--trim-observe` had no capture, comparator or
geometry probe. Offline label screening found 28 suspect frames among 1,321
frames; direct inspection of frame 365 shows missing caption buttons, clipped
duration and a lower action row cut off by the new bounds. This is a demonstrated
failure, not an inference from three sampled clean frames. The offline screening
only locates suspect labels and is not a replacement visual gate. Separate DXGI
recordings and an identical-input/backdrop `--resize-external` diagnostic are
also retained; their full frame analysis is not complete.

A minimal diagnostic Windows reference (`native-probe/native_resize_probe.cpp`
under the local evidence root) uses the full default native frame, horizontal/
vertical class redraw, one initialized shared PaintBuffer and four fixed-anchor
text labels. It has no child HWNDs, media, custom caption, theme renderer, timer,
capture or geometry observer. The same external input driver and DXGI desktop
recorder reproduce clipped right/bottom text during fast top-left movement
(`samsung-native-reference.mkv`; directly inspected
`native-reference-frame-08.png`). This demonstrates the pattern outside Easnap's
layout/preview implementation under these desktop conditions. It does **not**
identify a particular OS/driver defect, prove every frame correct, or establish
that a different rendering design would fix Easnap. The diagnostic is not used
as a production/test substitute. No frame policy was changed on this basis.

### Preview resize credit starvation: reproduced and corrected, visual resize still failing

Inspection of the visible helper lag identified a concrete additional path:
`ProcessMediaPreview::CreateOwnerWindow` polls snapshots every 100 ms through
low-priority WM_TIMER. Acknowledging one resize requires reading that snapshot;
the next coalesced size cannot be sent until then. Sustained higher-priority
input/painting can defer the timer. Microsoft documents that scheduling rule
for [WM_TIMER](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-timer).

`ResizeWhileTimersAreDeferred` in `test_preview_backpressure.cpp` services real
posted/sent messages while deferring timers and changing the production preview
parent. Before correction, 40 requests produced only one helper size change;
the repeated-progress assertion failed both before and after correcting this
fixture's PMv2 entry point. After correction, 40 requests produced 16 observed
changes, 80 dispatched posted messages and zero timer dispatches. The original
3.6-second no-poll recovery check also still passed with the same helper process
and player generation. Evidence: `resize-credit-before-pmv2.log` and
`resize-credit-after.log` (Release, SAMSUNG 120 DPI).

`UpdateVideo` now schedules at most one private owner-window poll message when
the size changes. It consumes available acknowledgements outside the layout
call; it does not recursively poll, wait for the helper, continuously repost,
or increase outstanding resize/intent/snapshot credit. The 100 ms timer and
existing recovery bounds remain the fallback; Close clears the pending marker.
This is a separate bounded scheduling correction, not a claim that the stale
owner-control pixels are fixed. Before the later memory-DC correction, full
Release passed 31/32;
`ui_modernization_native` fails on one transient title change and one black
gutter sample during export-state resizing. Affected Debug passes 11/11 in
170.51 s. Both endpoint checks pass (Release 55.13 s, Debug 58.30 s), as do
backpressure, startup and all three Release transport stress seeds. Evidence:
`Release-final-full` and `Debug-final-affected` logs/JUnit files. These results
do not supersede the separate failing directional gate or prove a new regression
caused by the posted-poll correction. The earlier 32/32 result is historical.

The observer was subsequently expanded to sixteen patch slots (caption actions,
timeline In/Out, visible status/progress in addition to the earlier controls),
and `--resize-controls` covers status and progress/Cancel through the production
view's public transitions. A missed minimum no longer relies on an arbitrary
100 ms sample: it waits for the real clamp for at most one second, observing
every available frame throughout, and still fails if it misses. Caption input
activation and premature drag cancellation are checked explicitly. These are
test-only changes; production binaries are unchanged. Current directional
results and affected UI/endpoint reruns are recorded separately below.

## Historical original-endpoint reproduction and cause

The supplied JPEG and external recording show stale Out/timeline/readout pixels
inside the video near logical 6.506 s. The external recording contains the
transition near 11.9 s. The original H.264/AAC source is 1920x1080 at 30 fps:
video duration 6.672067 s, container/audio duration 6.710567 s. A complete FFmpeg
decode reported no errors; the source's final pictures contain no trim UI.

A new test of the production TrimWindow and supervised helper reproduced the
same stale-control pattern after maximize/restore, using Windows Graphics
Capture monitor frames at 120 DPI. The old tests sampled a standalone preview,
so their successful results did not establish that surrounding trim pixels
remained correct during the handoff.

The GDI still window was hidden and sibling-clipped during native playback.
Showing it after hiding native video could expose stale parent backing pixels
before its complete paint reached desktop composition. Redraw suppression,
additional redraw/flush calls, copy flags, changing top-level compositing and
pre-scaling alone did not reliably solve this. Those experiments were reverted.

## Retained original-endpoint correction

The existing helper-owned still HWND now has its own opaque layered surface.
Its GDI aspect-fit drawing still uses shared PaintBuffer; PresentLayered submits
the complete bitmap with UpdateLayeredWindow before native video is hidden.
The still keeps sibling clipping and stays hidden during ordinary native
playback/paused preview. It is explicitly presented on handoff and WM_SIZE,
including restore after an empty client area. Replay hides it after In's actual
sample interval. Presentation errors become real preview failures.

The helper embeds a Windows 10 compatibility manifest. An initial layered-child
experiment without that manifest failed with **HRESULT 0x80070490** and opened
the production error dialog; its test timed out. That was an implementation
error, not bad source media. After the manifest was added, opening succeeded.
Relying on WM_PAINT alone then left the layer uninitialized/black; explicit
bitmap submission fixed that path. Those failed runs are retained as evidence.

There is no additional decoded-frame cache, protocol change or synchronous
parent-process painting. Both siblings retain clipping. Native transport/audio,
actual endpoint PTS, exclusive Out, ordinary paused frames, replay from In and
the accepted native top-level trim frame remain unchanged.
[ADR-010](decisions/ADR-010-trim-view-ownership-and-orchestration.md) records the
contract. Microsoft's [layered-child requirements](https://learn.microsoft.com/en-us/windows/win32/winmsg/using-windows#using-layered-windows)
and [bitmap submission API](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatelayeredwindow)
explain the platform mechanism; local desktop captures establish the observed
before/after behavior.

## Historical original-endpoint validation and delivery

- Before: real-monitor captures reproduced duplicated Out/timeline/readout and
  stale panel/chrome pixels inside the video. Failed candidates remain evidence.
- Continuous parent-backed painting passed an initial 16 transitions, but
  exposed a future endpoint during paused resize. Restricting that paint
  reproduced the duplicated controls in Debug. Converting all settled pauses
  to still presentation also worsened resize behavior. These candidates were
  rejected; none is the delivered implementation.
- Final Release/Debug builds succeeded with warnings as errors. Full Release
  passed **32/32** (334.38 s), and affected Debug passed **5/5** (155.88 s):
  startup, backpressure, production UI, full-trim endpoint and annotation editing.
- The supplied clip passed **16/16** transitions in two Release/Debug rounds
  (both themes, natural end and replay), with moving pictures, endpoint samples,
  no duplicated UI strips, no black frames and successful completion. The
  second round saves evidence after a 200 ms settling interval because monitor
  capture trails the logical Out notification. Only optional artifact timing
  changed after CTest; assertions/production code were unchanged and both test
  binaries were rebuilt. The final image visibly shows Play and 6.710/6.710.
- The strict opt-in all-edge monitor gate is **not clean** and is separate from
  the 32 registered CTest cases. The helper backed up at the start of 2026-09-29,
  paired with the current test executable, produced 39 assertion failures,
  versus 44 for the endpoint-corrected Release and 43 for Debug. This was not
  the accepted 2026-09-21 package and cannot establish that it also failed.
  The counts are failed assertions, not distinct product defects. Visible
  cursor highlighting overlaps the title comparison patch and can affect
  equality, but has not been shown to explain every failure. Both builds also
  report mixed geometry, green samples and invalid settled frames. A product
  regression has not been ruled out. The comparison
  covers all eight edges/corners, caption movement, maximize/restore, both
  themes and native paused/held endpoint modes at 125% DPI. The later final
  source change only handles empty clients and presentation errors.

The observer's `mixed_geometry` count means the title, range readout and Set In
pixel patches could not all match a single recent window geometry (allowing a
one-pixel offset). `holes` means at least 16 sampled pixels inside the common
client area matched the test's bright green backdrop. `settled invalid` means
one of those conditions persisted in samples after the gesture. These are
pixel-comparison assertions, not Windows HRESULT errors, and require inspection
of the actual failing regions before assigning a cause.

The accepted 2026-09-21 record remains valid: 4,065 drag frames, 363 clean settled
samples and user acceptance. Its archived build directory is absent from this
workspace, so an exact binary comparison with that package was not performed.
The next required slice is to isolate the current failures without pointer
highlight interference, repair any demonstrated regression and pass the resize
matrix. This is unresolved repair work, not an accepted limitation or Phase 4
deferral; endpoint success alone does not close the whole repair.

`trim_endpoint_presentation` is a registered serial desktop test (32 total CTest
cases). It generates real moving H.264, maximizes/restores the real trim, observes
both themes and replays from Out. It rejects long timeline/panel-colored strips
inside the picture, black frames, missing motion, missing endpoint samples and
failure to complete. The strip assertions are specific to the generated fixture
and supplied clip; this is not an arbitrary-content video validator.

Commands after building:

```powershell
ctest --test-dir build -C Release --output-on-failure --parallel 4
ctest --test-dir build -C Debug -R "^(trim_endpoint_presentation|mfplay_preview_startup|mfplay_preview_backpressure|ui_modernization_native|annotation_editing_native)$" --output-on-failure
.\build\bin\Release\test_ui_modernization.exe --trim-endpoint .local\trim-artifact\sample bug-video-footage\6-710-sn-video-sample-itself.mp4
.\build\bin\Release\test_ui_modernization.exe --resize-edges .local\trim-artifact\resize-release
.\build\bin\Debug\test_ui_modernization.exe --resize-edges .local\trim-artifact\resize-debug
```

Local logs, before/after frames and recovery sources/binary are under
`.local/trim-artifact/`. Sandbox monitor capture initially terminated in Windows;
real desktop checks run outside the sandbox. This is an environment failure,
not successful coverage. Manual acceptance and Phase 4's broader display/device
matrix remain separate from these automated checks.

The matched Release pair is installed in `dist/bin`; each installed executable's
SHA-256 matches `build/bin/Release`. SDK `mt.exe` extracted the installed helper's
manifest and confirmed the Windows 10 compatibility declaration is embedded.
The main trim and helper host source files are byte-identical to the pre-fix
copies; no top-level layout/frame changes remain. This does not prove resize
behavior is unchanged: the helper's still surface and WM_SIZE presentation
changed and remain subject to regression validation. Serena's standalone clang
diagnostics lacked this project's include path/C++20 configuration, so the
successful warnings-as-errors MSVC builds are the compiler validation.

The durable validation and SHA-256 manifest (`evidence/trim-endpoint-artifact-20260929.json`, retained locally)
records the actual test results, source inputs, installed pair and limitations.
Desktop evidence: before the fix (`evidence/trim-endpoint-before-20260929.png`, retained locally)
and the retained endpoint after the fix (`evidence/trim-endpoint-after-20260929.png`, retained locally).
Historical report links to absent build trees were converted to explicitly
historical paths; their test outcomes were preserved. The local Markdown link
check covers README, the roadmap and all Markdown documents under `docs/`.
