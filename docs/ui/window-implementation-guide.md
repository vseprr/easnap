# Window implementation guide

The required path for new or changed Easnap windows. Read it before designing
a window or adding controls to an existing one.
[ADR-024](../decisions/ADR-024-native-trim-frame.md) records the reasons for the
resize policy.

One limitation of the resize path is open: the strict all-edge resize gate
fails during motion and maximize/restore, and no cause is established; see
[project status](../project-status.md#known-limitations-and-unresolved-risks).
The rules below are the baseline to preserve, not a claim that the resize path
is free of visible defects. The all-edge gate and the endpoint check observe
different phases: the gate disables DWM transitions and captures four
maximize/restore transitions, the endpoint check maximizes and restores before
its observation period. An endpoint pass does not establish a pass for
maximize-transition presentation or for the whole resize matrix.

## 1. Choose the existing window path

| Surface | Reuse | Boundary to preserve |
|---|---|---|
| Resizable native content window | `StyleWindowChrome(hwnd, true)`, `WindowChrome`, and the `TrimWindow` layout/lifetime pattern | Native resize frame and OS move/maximize/restore behavior |
| Fixed confirmation/dialog | Shared nonresizable chrome and existing native modal-dialog/control helpers | Real default/cancel buttons, keyboard behavior and owner lifetime |
| Floating editor/recording island | `ui/island_layout` for the place around the frame, shared controls and theme materials | Owner/z-order, capture exclusion through `ui::ExcludeFromCapture` (layered at full opacity; a plain excluded window leaves a black hole in pictures, [ADR-008](../decisions/ADR-008-recording-ui-and-safe-finalization.md#capture-exclusion-of-the-islands-2026-10-30)), grip dragging, work-area containment, and never covering the mode island ([ADR-036](../decisions/ADR-036-island-placement-around-the-frame.md)) |
| Capture image/overlay | Existing redirected layered-image and damage-limited overlay path | Immutable source, display affinity and controlled first exposure |

These are different surface types. The trim's `WS_EX_COMPOSITED` choice is not
a blanket instruction to change layered overlays, native text hosts or dialogs.
Reuse shared implementation rather than copying window procedures into a new
feature. Extend shared code only when the new requirement needs it, and verify
existing callers as well.

Every window that shows text takes it from the string tables and must fit in
both languages: add it to `text_fits_in_every_language`
(`tests/test_text_fit.cpp`). Every dialog that runs its own message loop
declares a `platform::ModalLoopScope`, so that Exit, the capture shortcut and
deferred cleanup know it is open.

Bind each custom window procedure to its real owner during creation
(`CreateWindowExW` creation parameter and the existing `WM_NCCREATE` pattern).
Verify that paint, erase and size messages actually reach that owner; a generic
control factory with a null parameter previously bypassed the video handlers.

### Verified entry points and implementation order

Paths in this table are relative to `apps/easnap-desktop/src`.

| Surface | Creation and state/layout entry points to inspect together |
|---|---|
| Trim and timeline/video children | `ui/trim_window.cpp`: `TrimWindow::Open`, `CreateControls`, `CreateControl`, `RebuildFonts`, `LayoutControls`, `UpdateLabels`, `UpdateStatusLayout`, `HandleMessage`, `HandleVideoMessage`, `Hide`, `ReleaseMedia`, `Destroy`; `ui/trim_layout.cpp`: `InitialTrimWindowSize`, `CalculateTrimWindowLayout`, `FitTrimVideo` |
| Shared caption and controls | `ui/native_controls.cpp`: `StyleWindowChrome`, `WindowChrome::Layout`, `PaintResize`, `ChromeParentProc`, `NativeRedrawGuard`, styled control procedures; `ui/paint_buffer.h`: `PaintBuffer`; `ui/ui_theme.cpp`: `DrawMaterial` |
| Fixed About/error/confirmation dialog | `ui/native_controls.cpp`: `ShowMessage`, `ShowChoice`, `ConfirmDestructive`, `MessageDialogProc`; callers in `main.cpp` and the coordinator. Retain modal default/cancel and native PMv2 dialog behavior. A dialog with named answers sizes its button row to the text; the destructive answer is never the default. |
| Settings dialog | `ui/settings_dialog.cpp`: `ShowSettingsDialog` on the same fixed-dialog path, with shared chrome and styled controls. It edits a copy of the values; the shell in `main.cpp` applies them. The shortcut button reads keys through a subclass while it waits. |
| Scrollshot island and preview | `ui/scrollshot_controls.cpp`: the Start/help island, the four-action island with status, and the live preview; placement in `ui/scrollshot_layout.cpp`. Capture-excluded like every surface that is visible during a capture. |
| Capture overlay, image, shade, palette and magnifier | `ui/overlay.cpp`: `OverlayController::Show`, `OverlayWindow::Create`/`Destroy` and their window procedures. Region clipping and layered-image submission here are intentional overlay behavior. |
| Editor tools, context, brush and native text host | `editor/screenshot_editor.cpp`: `CreateToolbarControls`, `SyncContext`, `PlaceContext`, `SetFrameInteracting`, `WindowProc`, and the native EDIT/text-host creation path. Preserve temporary tool hiding during capture-frame interaction. |
| Recording configuration/active/paused/finalizing island and boundary HWNDs | `ui/recording_controls.cpp`: `RecordingControls::ShowConfiguring`, `RetireWorkspace`, nested `Window::Create`, `UpdateForState`, `Layout`, `HandlePanelMessage`, `Destroy` |
| Preview process and native/still children | `media/preview_process.cpp`: `ProcessMediaPreview::UpdateVideo`, `SendResize`, `Poll`, `Close`; `media/preview_host_main.cpp`: host creation/resize dispatch; `media/media_preview.cpp`: `MfPlayPreview::UpdateVideo`, `NativeVideoWindowProc`, `StillFrameWindowProc`, `ShowStillFrame`, `PresentStillFrame` |
| Timeline input and preview health | `ui/trim_window.cpp`: `HandleTimelineMessage`, `UpdateTimelineFromPoint`, `SeekTo`, `SetAccessibleTimelineValue`; `media/preview_transport.cpp`: `PreviewTransport::TakeNext`/`Complete`, `PreviewLiveness::Observe`; `media/media_preview.cpp`: `MfPlayPreview::DriveTransport`, `CheckLiveness`, `RequestRecovery` |
| Session retirement and shutdown | `app/session_state.cpp`: `CaptureCoordinator::EndSession`, `OnDeferredCleanup`, `Shutdown`; check owner relationships before changing teardown order. |

Implement in this order: choose the existing surface type and owner; establish
its initial monitor/DPI and native styles; bind the native procedure; create and
style actual controls; install fonts and complete measured layout before first
exposure; initialize asynchronous work at its existing boundary; then show the
surface and validate its first usable state. Treat creation failure, hide,
owner-driven destruction and repeated destruction as part of that same path.
The hidden notification-area message host in `main.cpp` and OS-owned save
dialogs/menu windows do not become custom resizable content windows.

## 2. Preserve the native resize frame

The reference is `WindowChrome::Layout` / `ChromeParentProc` in
`apps/easnap-desktop/src/ui/native_controls.cpp`.

- Let the default native handler calculate left, right and bottom nonclient
  insets. Customize only the caption allocation. When maximized, retain the
  DPI-scaled top resize inset used by the shared handler.
- Keep the proposed `WM_NCCALCSIZE` rectangle in its native coordinate flow.
  Do not replace an intermediate rectangle with the monitor work area.
- Retain the shared zero return from `WM_NCCALCSIZE`. Do not restore the former
  top-level `WVR_REDRAW` or forced `SWP_NOCOPYBITS` policy: it displaced child
  surfaces when the left/top origin changed. Top-level and child-layout flags
  have different purposes; do not remove the tested child flags globally.
- Preserve native nonclient paint/activation for resizable windows. Let DWM
  provide supported corner treatment; do not call `SetWindowRgn` on each resize.
- Forward move, minimize, maximize, restore and close through the native system
  behavior. Keep hit testing and accessible button actions on the shared path.

Removing the entire native frame, transplanting work-area coordinates, or
copying a right/bottom-only workaround can reintroduce jumping and duplicated
controls when the opposite edge moves.

## 3. Compute layout before painting

Use `TrimWindow::LayoutControls`, `CalculateTrimWindowLayout` and `FitTrimVideo`
as the content-window reference in `src/ui/trim_window.cpp` and
`src/ui/trim_layout.cpp` (paths relative to `apps/easnap-desktop`).

1. Measure text using a memory DC with the actual UI font selected. Release it
   promptly. Do not acquire a window DC just to calculate layout.
2. Work in client coordinates. Scale DIPs once using the window's DPI, then
   calculate all control rectangles before moving any child. Do not shrink
   fonts or input targets to make a small window fit.
3. Include native frame dimensions when converting a client minimum to outer
   tracking dimensions. Trim's accepted minimum is 480 x 400 client DIPs,
   capped to the monitor work area; derive a new window's minimum from its own
   content. Use the destination monitor for a suggested DPI-change rectangle.
4. Publish child geometry with the existing deferred-position batch and its
   fallback. Preserve the tested child `SWP_NOREDRAW` / `SWP_NOCOPYBITS` policy;
   perform explicit invalidation after geometry is complete.
5. Complete owner-thread paints through the shared resize path. Preserve
   parent/video clipping. Do not synchronously recurse into a foreign helper.

Keep status/progress allocation content-driven, video aspect-correct and all
actions reachable at minimum size. Rebuild fonts on DPI changes before layout,
not on every mouse movement. Use themed backing pixels for newly exposed areas;
do not hide controls or add transparency/layered-window hacks to conceal damage.

## 4. Keep one owner for each set of pixels

- Use `PaintBuffer`, styled native controls and the shared theme painters.
  A paint pass must initialize its whole buffer and present it before releasing
  the target DC. Honor clipping and use the existing complete erase handlers;
  do not claim an erase succeeded while leaving exposed pixels undefined.
- Pair every `BeginPaint`/`EndPaint` and `GetDC`/`ReleaseDC` promptly. Do not retain
  a window DC across timer ticks, input waits or the native sizing loop. Do not
  add `CS_OWNDC` to the composited content-window path.
- Keep real native input, focus, keyboard and accessibility semantics. Use the
  existing targeted redraw guard for styled controls; blanket `WM_SETREDRAW`
  around input or an entire resize can change visibility and break behavior.
- Annotation EDIT has a separate accepted contract, owned by
  [ADR-022](../decisions/ADR-022-text-input-and-content-sized-trim.md): the
  visible HWND keeps input, the source-backed painter owns the pixels. Reuse
  it; do not hide/recreate EDIT while typing.
- Reuse `theme::DrawMaterial`: bounded per-thread geometry and colored-raster
  caches, complete rounded ends and a stretched constant center. Keep palette,
  dimensions and animated depth in cache identity. Do not recompute Gaussian
  shadows for every pixel of every newly sized button or reuse stale colors.
- Invalidate only affected surfaces for ordinary state/animation changes.
  Preserve theme-switch, high-contrast and reduced-motion behavior.

## 5. Keep media work and shutdown out of the resize loop

The reference is `ProcessMediaPreview` in `src/media/preview_process.cpp`,
`TrimWindow` and [ADR-010](../decisions/ADR-010-trim-view-ownership-and-orchestration.md),
whose decision 4 states the mechanism behind each rule below.

- Preserve the asynchronous, coalesced preview pipe: at most one unacknowledged
  resize request; newer dimensions replace local pending state. The helper owns
  its native video window. `SWP_ASYNCWINDOWPOS` alone is not a substitute for
  this process boundary.
- Size changes schedule at most one private owner-thread poll message, because
  WM_TIMER may be deferred during continuous input. Keep the timer as fallback;
  do not recursively poll from layout, repeatedly self-post while waiting, or
  increase pipe credit. `UpdateVideo` / `OwnerProcedure` in `preview_process.cpp`
  and `ResizeWhileTimersAreDeferred` in `test_preview_backpressure.cpp` establish
  the repeated-progress case. This is a scheduling rule; it does not address
  the open visual resize limitation.
- Never synchronously call foreign `SetWindowPos`, `UpdateWindow`, recursive
  `RedrawWindow` or native playback from the parent UI thread. Do not add sleeps,
  busy waits or encoder/decoder shutdown to resize/paint callbacks.
- Keep acknowledgement, heartbeat, opening and recovery deadlines bounded,
  including the three-second refresh grace after delayed owner polling.
  Preserve explicit failure and generation checks.
- Scope timeline pointer requests to one gesture: `timeline_seek_position_`
  records the requested target, not the live playback cursor, and is cleared on
  each new press, release, capture loss, Hide and Destroy. Keyboard and UIA
  requests keep their direct path.
- Keep accepted input and issued native commands apart in
  `PreviewLiveness::Observe`. Change that boundary only together with the
  deterministic tests in `test_trim_workflow.cpp` and the real helper stress;
  it is not permission to hide a decoder stall or widen the recovery timeout.
- Preview opening must present the first decoded frame paused at zero,
  including a one-frame source. Retain the dedicated startup test.
- At completion, retain the final frame and logical Out until the next action;
  replay starts at In. Keep real desktop pixel checks for the transition, held
  picture and replay.
- Keep the retained still on the helper's opaque layered child, submitted
  complete before native video hides and again on WM_SIZE, with the helper's
  Windows 10 manifest and sibling clipping. Do not extend layered presentation
  to the top-level trim, dialogs or other controls. Ordinary playback and
  paused images stay native; do not paint a future endpoint into them.
- Hide/retire UI callbacks before deferred teardown. Close/cancel must preserve
  source media and must not destroy an owner while its callback is executing.

For owner lifetime, review `CaptureCoordinator::EndSession`,
`OnDeferredCleanup` and `Shutdown` in `src/app/session_state.cpp`, together with
`RecordingControls::ShowConfiguring`, `RetireWorkspace` and
`RecordingControls::Window::HandlePanelMessage` in `src/ui/recording_controls.cpp`.
Paths are relative to `apps/easnap-desktop`. The configuring recording island
and screenshot editor can be owned by overlay HWNDs: destroy those dependent
views first, then the overlay. Active recording explicitly detaches the owner
before the workspace retires. Windows also destroys owned HWNDs when their owner
is destroyed ([Microsoft ownership contract](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#owned-windows));
the corresponding C++ view must clear panel/control handles in
`WM_NCDESTROY` and recreate them before reuse.
`test_ui_modernization --window-lifetime`, part of `ui_modernization_native`,
destroys a real owner and reopens the island twelve times; its measured object
counts, and what they do not establish, are in
[ADR-010](../decisions/ADR-010-trim-view-ownership-and-orchestration.md#measured).

## 6. Validate the new surface before handoff

Use production code and actual HWNDs. Extend the relevant existing tests rather
than testing a copied layout algorithm or a replacement rendering stub.

| Check | Required coverage for a resizable content window |
|---|---|
| Geometry | All four edges and four corners, especially left/top; shrink and grow; content minimum and very large sizes |
| Native transitions | Rapid repeated resizing, maximize/restore, minimize/restore, caption buttons and native hit tests |
| Appearance | Light/Dark at 125% scaling; real DPI/work-area changes where available; readable text and retained actions |
| Active states | Paused/playing preview and visible progress/status/error states where applicable |
| Responsiveness | Input, close/cancel and helper recovery during repeated resizing; no UI-thread media waits |
| Actual presentation | Independently captured desktop frames plus interactive review; compare with the accepted implementation |
| Unrelated contracts | Shared callers, focus/keyboard/UIA, text editing, capture exclusion, content/export and theme invariants |

Expected outcomes are explicit: throughout motion, captions/readouts/actions
must form one coherent layout with no stale copies, clipped glyphs, green
backdrop holes or blank video transitions. At the actual native minimum, text
and input targets retain the intended DPI size and all actions remain usable.
After input stops, no resize work may continue accumulating. The existing
dispatch watchdog is one second; it is a responsiveness bound, never permission
for a visible one-second glitch. Preview refresh after delayed polling remains
bounded by the three-second protocol grace described above. Verify held Out,
exclusive endpoint selection, replay from In and paused-first-frame opening
alongside the visual checks, on the same production build.

Tests that use the real pointer, focus or capture run one at a time on the
visible desktop, with the test window in the foreground
(`interactive_desktop_tests` in `tests/CMakeLists.txt`); message-driven window
tests run in parallel on a private desktop. See [testing](../testing.md). A
test whose drag did not start or whose window was
obscured is not successful coverage. `PrintWindow`/GDI child snapshots cannot
prove what a foreign Direct3D video presenter displayed; use actual desktop
capture for that claim. Record the environment and actual result, and distinguish
manual acceptance from an automated assertion.

Readiness predicates can consume evidence. `WaitFor` in
`tests/test_ui_modernization.cpp` returns on the first true result; it must not
call a successful frame-consuming predicate again for its return value. A second
call can consume an empty asynchronous frame queue and falsely turn successful
preparation into failure. Preserve the timeout and continue to fail when no
authenticated frame arrives. For timeline tests that mix sent messages and real
input, align the physical pointer with the message coordinates and authenticate
foreground ownership before capture. Confirm actual capture ownership within
the existing bounded wait; injected input alone is not proof of a received click.

How a requested physical display reaches a test (`scripts/test.ps1 -Monitor`,
`EASNAP_TEST_MONITOR`, `tests/native_test_monitor.h`) is in
[testing](../testing.md#keeping-tests-away-from-your-work); the per-monitor v2
rule for fixtures and the tool window of the preview pixel check are under
[writing tests](../testing.md#writing-tests). The visible-desktop tests are
pending in daily runs and run in the Full tier in the
[lab](../testing.md#lab); hosted CI runs none of them.

Useful existing checks in `apps/easnap-desktop/tests`:

- `ui_modernization_native`: shared controls, production trim, DPI/layout and
  rapid native resizing while playing/exporting.
- `test_ui_modernization --resize-edges <evidence-directory>`: the opt-in
  strict resize gate, with real drags from eight directions, caption movement
  and maximize/restore observed in independent monitor frames;
  `--resize-controls` applies it to visible status, progress and Cancel. Their
  slow variant, the diagnostic subsets and how the observer judges a frame are
  in [testing](../testing.md#diagnostics-that-are-not-registered-tests). A
  subset or diagnostic never replaces the unchanged `--resize-edges` gate, and
  PNG review remains necessary.
- `test_ui_modernization --window-lifetime`: real recording-owner destruction,
  stale-handle checks, reopening and repeated explicit destruction. Also included
  in the default native UI test.
- `ui_material_raster_parity`: 768 material cases; keep pixel parity when
  optimizing the shared renderer.
- `mfplay_preview_startup` / `mfplay_preview_backpressure`: real presented first
  frames and delayed owner polling. Keep startup separate from transport stress.
- `trim_timeline_seek_stress`: registered serial desktop check with 24 clicks
  per case; `trim_timeline_seek_soak` repeats it with 72 in the Soak tier. Also
  available as `test_ui_modernization --seek-stress <evidence-directory>
  [click-count]`; CTest runs it with the DXGI observer
  (`EASNAP_TEST_CAPTURE=duplication`), so set that variable when running the
  command directly ([testing](../testing.md#writing-tests) says why).
  It exercises 6 s/10 s media in both themes, held clicks, identical move/release
  coordinates, new clicks at the same point and changed drag targets. Continuous
  random native clicks keep Play requested without a test-inserted pause or
  restart. Require enabled controls, the same helper, visible moving video and
  bounded input processing; missing or obscured capture is unavailable evidence.
- `mfplay_preview_continuous_seek`: one deterministic real H.264/AAC backend
  seed for 15 seconds; `mfplay_preview_continuous_seek_soak_0` to `_2` run
  three seeds for 30 seconds each in the Soak tier. Also available as
  `test_media_preview --continuous-seek [seed-index] [seconds]`. Confirm that frequent backward seeks do not cause
  false recovery. Unit `TestPreviewLivenessCompletedSeeksBetweenPolls` models
  complete native command cycles between watchdog samples and retains genuine
  stall/missing-callback deadlines. A backend pass alone does not validate
  timeline message ownership or actual desktop presentation.
- `trim_endpoint_presentation`: the complete production trim HWND and helper,
  maximize/restore before playback, both themes, natural end and replay. Monitor
  captures must show moving video without duplicated timeline/panel strips or
  black frames. This complements the standalone preview pixel checks.
- `capture_workspace_production` / `annotation_editing_native`: shared-window,
  source-image, DPI, native input and editing regressions.

Build Release and the affected Debug targets with the existing warnings policy;
run the applicable checks. Shared rendering/lifetime changes require the full
Release suite and affected Debug tests. Pair `Easnap.exe` with the matching
`EasnapPreviewHost.exe` when packaging. Write actual validation and the resulting
status to project status; record a changed architectural contract in an ADR.
A passing CTest suite does not include the opt-in resize gate and cannot close
its failure. Changes to timeline input, preview health or retained
presentation also require the timeline seek stress, the continuous backend
seek test (all three seeds with `-Tier Soak`), startup, backpressure and
endpoint checks on the same production build. Keep the
applicable resize matrix alongside them; passing seeking alone is insufficient.

## Sources and precedence

The production code and ADR-024 supersede the earlier resize experiments.
Microsoft documents [custom frames](https://learn.microsoft.com/en-us/windows/win32/dwm/customframe),
[client preservation](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-nccalcsize),
[positioning flags](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos),
[composited DC lifetime](https://devblogs.microsoft.com/oldnewthing/20171018-00/?p=97245),
and [GDI/DirectX window separation](https://learn.microsoft.com/en-us/windows/win32/dwm/bestpractices-ovw).
The DXGI observer follows the
[Desktop Duplication API](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api),
while playback completion follows
[MFPlay's asynchronous callback contract](https://learn.microsoft.com/en-us/windows/win32/medfound/getting-started-with-mfplay).
Consult these contracts when changing the shared path, and validate against the
accepted Easnap baseline instead of applying a generic flicker workaround.
