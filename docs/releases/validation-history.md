# Validation records

The release notes of each version (listed in the
[documentation index](../README.md)) say what changed and summarize how it was
checked. This file keeps the detail behind those summaries: the individual
runs, the ones that failed, and what was not validated. The records of v1.0.1
and v1.0.2 were part of the project status until 2026-10-13, those of
[v1.1.0-beta.1](#validation-of-v110-beta1) until 2026-10-30 and those of
[v1.1.0-beta.2](#validation-of-v110-beta2) until 2026-11-07. This file is a
record, not a task list. The current state is in
[project status](../project-status.md).

## Scope

v1.0.1 followed the published v1.0.0 with fixes, hardening and test tooling.
v1.0.2 adds the two changes listed under [v1.0.2](#v102) below.
No feature was added and no phase was started. Distribution is a per-user
installer and a portable ZIP built from the same two executables, both
unsigned, each with a SHA-256 file and a build provenance attestation.

## Implemented in v1.0.1

Defects fixed:

- Both-source audio health is read directly from the endpoints instead of
  waiting for the mixer thread's poll; a failed Both session cannot be
  restarted before `Stop()`. This removed the intermittent
  `wasapi_audio_capture` failure.
- A failed WASAPI `GetBuffer` is classified before the empty-buffer exit, so
  endpoint loss is recorded as a capture failure.
- The recording audio gate fails the session after 10 s without video progress
  instead of blocking Pause/Stop/Abort; `Failed` is no longer overwritten by
  Pause, Resume or Stop.
- A black frame after real content no longer demotes Desktop Duplication to
  the GDI blit; the region staging texture is reused; the cursor is composited
  through a cursor-sized surface and skipped when outside the region.
- Editor: stale selection after Esc/Delete/undo no longer indexes past the
  annotation list; property controls commit only transactions they opened.
- `SaveImage` stages beside the destination and replaces it only on success.
- The tray menu ignores the duplicate right-click message; the idle trim
  timeline is not repainted every tick.

Displays and scaling:

- Both executables declare per-monitor v2 in their manifests. `VS_DPI_AWARE`
  had embedded per-monitor v1, which the run-time request could not raise, so
  the v1.0.0 binaries ran in a different DPI mode than the tests.
- A work-area-only change no longer cancels screenshot editing; a recording
  whose region left the desktop ends through the Stop path; a maximized or
  minimized trim window is not resized on display changes; the recording
  island adopts the suggested rectangle on a DPI change; the editor context
  island is re-fonted; selection-handle hit areas scale.

Security:

- Deletion verifies that each opened entry still resolves to a direct child of
  the directory it was listed in (junction race).
- Library search is restricted to System32 at run time and for static imports
  (`/DEPENDENTLOADFLAG:0x800`); `/CETCOMPAT` and `/HIGHENTROPYVA` are enabled.
- The helper's stderr no longer shares the protocol pipe; reported snapshot
  values are range-checked.
- CI pins third-party actions to commits, does not persist the token, and
  attests release packages.

Tests and tooling ([testing](../testing.md),
[ADR-026](../decisions/ADR-026-test-tiers-and-desktop-isolation.md)):

- Tiers: fast, integration, desktop, soak. Repeated stress seeds, 30-second
  seek runs and the 72-click timeline run are soak; no assertion was removed.
- Three message-driven window tests run on a private, non-input desktop.
- Visible-desktop tests run last on a chosen display with a warning,
  countdown, pointer restore and one reported retry.
- The pass cache hashes only `EASNAP_*` environment variables.
- New table-driven tests cover trim layout across 10 work areas and 9 scale
  factors, and selection hit areas across scales and negative origins.
- Per-user installer (`packaging/easnap.iss`) built by `package-release.ps1`.

## v1.0.2

- Editor: derived Blur/Pixelate/Spotlight pixels are kept by
  `editor/redaction_renderer`. An edit recomputes only the rectangle it can
  influence (the changed effects, plus every effect that reads those pixels)
  and uploads only that rectangle to the preview bitmap. A change of the
  shared spotlight dim, or the first/last spotlight, still covers the image.
- MP4 trim: the decoder announces its padded buffer (160x128 for a 160x120
  picture) with the first sample. The exporter ignored that change. Whenever
  the encoder received the frame as one contiguous buffer it read chroma from
  the wrong offset, and the trimmed video was visibly wrong for any height
  that is not a multiple of 16. That happens on the system-memory NV12 path
  and on the D3D path with an encoder that locks surfaces instead of reading
  them. The exporter now re-reads the decoded format, resolves the display
  aperture, crops on the BGRA path and repacks the picture for those
  encoders. The writer reports whether its encoder is Direct3D-aware; only
  then is a padded texture forwarded unchanged, which was already correct.

Validation of these two changes:

- `redaction_renderer_logic` (new, fast tier): 12 seeded sequences of 60
  random edits each compare the incremental image with a rebuild after every
  edit and check that nothing outside the reported rectangle changed; with
  the dependency closure disabled the test fails. A small drag on a 1200x800
  image stays inside its neighbourhood.
- `mp4_trim_export_backend` now decodes the trimmed file and compares the
  picture with the source pattern. Mean channel error was 1.11 on the local
  hardware path (NVIDIA decoder and encoder). With D3D disabled by a
  temporary local switch (not committed) it was 37.89 before the fix (121.22
  in the bottom rows) and 1.11 after. The hosted runner (WARP adapter,
  software encoder) failed the same check with 34.89 on the D3D path until
  surfaces were repacked for encoders that lock them, and passes since. No
  committed test selects a path: each machine exercises the one it has.
- Full tier, visible-desktop tests on the primary display while the PC was in
  use, two runs. First: 33 passed, `recording_pause_stop_stress` passed only
  on the automatic retry (one audio/video duration delta of 101.7 ms against
  a 100 ms limit). Second: 32 passed, `trim_timeline_seek_stress` (pointer
  authentication, the pointer was one pixel off) and
  `annotation_editing_native` (the known reopened-text-box comparison) passed
  only on the retry.

Release validation of v1.0.2 (2026-10-12, version bump and one test timeout
changed since the runs above):

- Hosted CI on `main` failed after the merge: `redaction_renderer_logic` hit
  its 30 s timeout in the Debug build. It took 23 s and 24 s in Debug on the
  two preceding pull-request runs and 3 to 4 s in Release. The timeout is now
  120 s; the test is unchanged.
- Release x64 build with warnings as errors succeeded; both executables report
  1.0.2.0.
- Standard tier with the cache ignored: 27 of 27 passed. Full tier, one run,
  visible-desktop tests on the secondary display: 34 of 34 passed, none
  retried. The six soak tests were not run.
- The installer was built locally from those binaries, installed silently
  into a scratch directory, the installed application started and reported
  1.0.2.0, and the uninstaller removed the files, the registration and the
  Start menu entry.
- Hosted CI passed on the release commit in Debug and Release;
  `redaction_renderer_logic` took 31 s in Debug.
- v1.0.2 was built by the Release workflow from the tag and published. The
  workflow's ZIP and installer were downloaded and checked before publication:
  SHA-256 files match, `gh attestation verify` succeeds for both, both
  executables report 1.0.2.0, and the installer installed, ran and uninstalled
  cleanly.

## Validation of v1.0.1

This section records the v1.0.1 release; v1.0.2 is under [v1.0.2](#v102).

Environment: one development PC, Windows 10, two 1920x1080 displays at 125 %
(144 Hz primary, 60 Hz secondary). Visible-desktop tests ran on the secondary
display while the PC was in use.

- Release x64 build with warnings as errors succeeded. Both executables report
  1.0.1.0, and a running `Easnap.exe` was confirmed per-monitor v2 through
  `GetDpiAwarenessContextForProcess`.
- **All 39 registered tests passed on the final binaries, but not in a single
  run.** The complete Soak run passed 37; `trim_timeline_seek_soak` and
  `annotation_editing_native` failed, and failed again on the automatic retry.
  After the UI test executable was relinked, the affected rerun (6 tests,
  including those two) passed 6 of 6.
  - `trim_timeline_seek_soak` failed with "pointer authentication failed": the
    real pointer was moved while the test was using it. That is the test
    working as designed, not a product failure.
  - `annotation_editing_native` was intermittent. Within this slice it failed
    in four suite runs and passed in every standalone repetition (11 of 11)
    and in the final rerun. Its failing assertions compare the presented text
    box with the source-backed renderer. The causes were found on the v1.1
    branch; see [v1.1.0-beta.1](#validation-of-v110-beta1).
- Timing on this machine: the Full tier ran 26 background tests in 91 s and 7
  visible-desktop tests in 274 s. The previous suite of 36 took 790 s. The
  Soak tier ran 31 background tests in 195 s and 8 visible-desktop tests in
  390 s. After a rebuild that changed one test executable, 6 tests ran and 33
  were reused.
- The intermittent `wasapi_audio_capture` failure was not reproduced on demand
  before the change (12 of 12 passes); after it the test passed in every run.
  Its fix is established by reading the code, not by a reproduced failure.
- The installer was built, installed silently into a scratch directory, the
  installed application started and reported 1.0.1.0, and the uninstaller
  removed the files and the registration.
- Hosted CI (`windows-2022`) built Debug and Release and passed the 20 tests
  it can execute. Hosted tests had never run for this repository before; the
  first run failed on the preview tests (the image cannot open an MFPlay
  player), on `capture_overlay_interaction` (click targets assume a larger
  desktop) and on the selection self-test (`Get-FileHash` unavailable). The
  first two are now excluded from hosted CI and remain local-only; the third
  was fixed.
- v1.0.1 was built by the Release workflow from the tag and published. The
  published ZIP and installer were downloaded again and checked: SHA-256 files
  match, `gh attestation verify` succeeds for both, both executables report
  1.0.1.0, and the published installer installed, ran and uninstalled cleanly.

Not validated:

- Endpoint loss was not exercised by unplugging a device. Display changes
  (disconnecting a monitor, changing scale or resolution during a session)
  were not exercised physically; they are covered by code review and by the
  table-driven layout tests only.
- The installer was not tried on a clean machine or another Windows version.
- No scale factor other than 125 % and no mixed-DPI layout was used physically.

Open item from validation, closed on the v1.1 branch: when the editor test
was changed to wait about 30 ms before a key press in a reopened text box, the
presented text box stayed different from the source-backed renderer for more
than 1.5 s in 4 of 5 runs. The test's double-click never released the button,
so the native control kept the mouse and followed the real pointer. With the
release added, the same wait passes 5 of 5.

## Validation of v1.1.0-beta.1

This section records the v1.1.0-beta.1 branch before its publication on
2026-10-13; the [release notes](v1.1.0-beta.1.md) summarize it. What was not
validated is still listed in
[project status](../project-status.md#not-validated).

One development PC, Windows 10 build 19045, NVIDIA GPU, two 1920x1080 displays
at 125%; visible-desktop tests and manual checks ran on the secondary display.

- Release x64 with warnings as errors builds. On 2026-10-13 the Standard tier
  passed 39 of 39 on the final binaries in one run.
- **Full tier on the final binaries: 46 passed and one passed on its retry**
  (2026-10-13, visible-desktop tests on the secondary display).
  `ui_modernization_native` failed its composed trim-window resize observation
  in four cases (`holes==0 && title_changes==0`), which is the open resize
  limitation of [ADR-024](../decisions/ADR-024-native-trim-frame.md#known-limitation-transient-artifacts-while-resizing),
  passed on the retry and passed twice more alone.
  The run before the island placement work, on otherwise the same sources,
  passed 45 of 45 with none retried. Two
  earlier Full runs of the same day: 45 of 45 before the symbol files and the
  change to the trim window; then 44, where `trim_timeline_seek_stress`
  stopped at its pointer check and passed when it was run again alone. It was
  put down to the PC being in use at the time; the later lab investigation found
  a pointer move that Windows reports late instead (see
  [ADR-037](../decisions/ADR-037-isolated-test-lab.md#why-trim_timeline_seek_stress-failed-its-pointer-check-2026-10-30)).
- **`annotation_editing_native` is no longer intermittent, for two reasons
  that were found.** A build that failed it deterministically (24 checks, four
  runs in a row) showed a one-pixel column at the native caret position, a
  product defect: the caret is now hidden for the whole message while text is
  typed into an annotation. The occasional failure on a reopened text
  box came from the test: its simulated double-click never released the
  button, the native control kept the mouse, and movement of the real pointer
  changed the selection under the comparison. With the caret corrected the
  test passed 4 of 4 standalone runs; with both corrections 3 of 3 and in the
  Full run. With a script moving the pointer continuously the text-box checks
  passed while later checks of the same test, which use the real pointer by
  design, failed as expected. The experiment recorded under v1.0.1 (a 30 ms
  wait after reopening, 4 of 5 runs failing) passes 5 of 5 now.
- `scrollshot_real_desktop`: a second process showed a 3,160-row document; six
  single wheel notches, three double notches, a click and two Page Down keys,
  then wheel notches to the end scrolled it through the open frame. The
  production session returned 720 x 2,880 pixels from 17 pictures, every pixel
  equal to what that process drew; one picture came through GDI, the rest
  through Desktop Duplication.
- `diagnostics_log_and_crash_reports` passes in Release and Debug: eight kinds
  of failure each leave one valid report.
- The stitching engine's 232 checks pass in Release and Debug; in Release the
  session's 66, the layout and viewport's 351, the workspace's 337, the
  data-safety and Settings dialog's 93 and the text-fit test's 392 (every
  production window and island in both languages).
- `island_layout_logic` (25,214 checks over five display and island sizes) and
  `capture_island_placement` (45 checks on production windows) cover the
  placement of the tools island, the recording panel and the mode island.
- `startup_entry_registry` writes, reads and removes the sign-in entry in a
  key of its own and finds the real `Run` key unchanged. `executable_imports`
  finds no Media Foundation library among the imports `Easnap.exe` needs to
  start.
- **The running application, by hand**, driven with real pointer and keyboard
  input over a test window:
  - Scrollshot and data safety: shortcut, region drag, the three mode buttons
    in Turkish, Start by button and by Enter, the scissors button, wheel
    scrolling to 720 x 1,440 and 720 x 1,680, Save through the system dialog,
    Edit with scrolling, zooming, a rectangle drawn at the pointed place and a
    full-size export, Copy by button and by Enter, the discard and exit
    questions, the sign-out block, a recording of 11 s through trim, an abrupt
    end of the process followed by the recovery offer ("Not now", then Save),
    a session end during recording followed by the offer and Delete, and the
    crash notice appearing once.
  - Settings and the shell: the dialog in Turkish, English and Turkish again
    with the language and appearance stored each time; the shortcut button
    taking Ctrl+Alt+F9 and Ctrl+Alt+Q, taking Easnap's own shortcut again,
    refusing Shift+K and keeping the dialog on Esc; no capture starting from
    the shortcut while the dialog is open and the shortcut working again
    afterwards; Cancel changing nothing; the `Run` key untouched; About and
    Copy details; a second start opening the workspace with the keyboard; a
    start with `--startup` beside a running Easnap doing nothing; an older
    `settings.ini` read and rewritten with its theme kept.
  - Saving without a dialog: two screenshots saved into a test folder under
    two names, 720 x 480 each, no dialog.
  - Recording with the delay-loaded build: neither Media Foundation library in
    the process at start, both there once the recording ran, 3 s recorded,
    trim window, Delete; a recording left by a killed process offered at the
    next start and deleted.
  - Library loading: with unrelated DLL files named `mfplat.dll` and
    `mfreadwrite.dll` beside `Easnap.exe`, a recording loaded both libraries
    from the Windows system directory and worked.
  - Islands: a frame of full height with gaps at the sides put the tools
    island over the bottom of the frame and left the mode island at its home;
    both were hidden during a resize and shown after it; a narrow frame put
    the tools island beside it; a frame over the top centre moved the mode
    island beside the frame; the recording panel went beside the frame.
    Nothing was fixed on the way.
  - A recording without a preview: with the helper executable taken away, the
    message named it, closing offered Save, Delete and Keep, Keep left the
    window, and Save wrote a 1.5 MB MP4 through the system dialog and removed
    the temporary recording.
- **AddressSanitizer, locally.** 20 of the 21 unit tests and the 10
  integration and private-desktop tests that hosted CI runs passed without a
  report. The crash-report test is excluded: the sanitizer takes its
  deliberate failures first. Preview and visible-desktop tests were not run
  under it.
- **Packaging.** `package-release.ps1` produced the ZIP, the installer, the
  symbols ZIP and their checksum files from this build, after checking that
  both executables report 1.1.0-beta.1 with the prerelease flag and name their
  symbol files without a directory. `package-manifests.ps1` wrote the winget
  and Scoop manifests from those packages.

## Validation of v1.1.0-beta.2

This section records v1.1.0-beta.2, published on 2026-10-30, and the
stabilization baseline on its sources; the [release notes](v1.1.0-beta.2.md)
summarize it. What is still not validated is listed in
[project status](../project-status.md#not-validated).

The lab VM (Windows 10 Pro 22H2, one 1920x1080 display at 125 %, no GPU) is
described in [ADR-037](../decisions/ADR-037-isolated-test-lab.md); the
development PC is the one of v1.1.0-beta.1 above.

- **Full tier in the lab: 48 of 48 with no retry** on the final sources,
  before the version number was raised (2026-10-30, run
  `20261030-230104-full`). `trim_timeline_seek_stress` reported 133 pointer
  moves, none late.
- The Full run before it, on the same sources, failed only
  `mfplay_preview_audio_transport`, at "Close retires a suspended native host
  within 1500 ms". That test builds none of the changed sources and passed
  alone in both probe variants, so it is a timing check that can miss its
  limit under load.
- An earlier Full run failed `annotation_editing_native` twice: the room kept
  for the property island also held back an island the user had moved to the
  bottom edge. Corrected.
- **"Saved" notice.** The Copied and Saved notices had a fixed width of
  250 DIP; the Turkish "Saved" text needs 385 px at 125 % and was cut. The
  notices are in `text_fits_in_every_language`; with the old width that check
  fails on the Turkish text.
- **Private-desktop tests on the development PC:** `capture_island_placement`,
  `scrollshot_workspace_production`, `text_fits_in_every_language`,
  `capture_overlay_interaction`, `capture_frame_interaction` and
  `data_safety_questions` passed.
- `island_capture_exclusion` ran on the development PC's SAMSUNG display with
  the owner's permission (68 of 68) before its still-desktop part existed;
  with that part it ran in the lab only (108 of 108). Its measurements are in
  [ADR-008](../decisions/ADR-008-recording-ui-and-safe-finalization.md#capture-exclusion-of-the-islands-2026-10-30).
- **By hand:** the owner checked the black-box fix on the development PC
  (recorded on 2026-10-30): the black box after moving the frame under the
  tools island was reproduced with the first beta and is gone with this
  version. The other corrections were not driven by hand.
- **Release workflow.** It built, tested and attested the packages on the
  hosted runner. Before publication the draft's SHA-256 files and the
  installer's attestation were verified, and both executables reported
  1.1.0-beta.2.
- **Installer, by hand.** After publication the owner installed and removed
  the published beta.2 installer; the uninstaller removed the settings as
  chosen (reported 2026-11-07).

**Hosted runner on `main`** (2026-10-30; the cause was under investigation on
2026-11-07). The Build workflow failed at `a5293c8`, the merge that carries the
beta.2 tag (run 41123963572: `diagnostics_log_and_crash_reports` in the Debug
and AddressSanitizer jobs), and at `42b3efa` (run 41126325105:
`mp4_trim_export_backend` in the Release job, in an export with audio). The
Release workflow run 41123994750 at `a5293c8` succeeded.

### Stabilization baseline (2026-11-07)

On the beta.2 sources (`42b3efa`, branch `stabilize/v1.1.0`, no uncommitted
changes), in the lab VM, by then at Windows 10 build 19045.6466:

- The Release build succeeded in both runs.
- **Standard: 39 of 39 passed with no retry** (run `20261107-145742-standard`,
  3 min 37 s for the whole run). The 9 visible-desktop tests are not in that
  tier and were not run; 6 tests are outside it.
- **Soak: 54 of 54 passed with no retry** (run `20261107-150201-soak`, 12 min
  41 s), the first Soak run since v1.0.1. `real_desktop_recording_session`
  passed on the GDI fallback and the verdict marked it `VM-ok-but-fallback`,
  the provider's documented behaviour on a still desktop.

## Validation of v1.2.0

Prepared on the branch `feature/v1.2` from 2026-11-08; the
[release notes](v1.2.0.md) summarize it. All runs in the 125 % lab image
unless noted, each on the uncommitted or committed tree of that step; in
every run `real_desktop_recording_session` passed on the GDI fallback
(`VM-ok-but-fallback`) and nothing was retried.

| Step | Run | Result |
|---|---|---|
| Pin to screen (`9cbbddb`) | `20261108-021123-full` | Full 50 of 50 |
| Audio exception boundary, undo limit (`e4904f1`, `ad9be42`) | `20261108-151811-full` | Full 50 of 50 |
| Clipboard formats first, state machine (`aa07b05`, `797d0d7`) | `20261108-153432-full` | Full 50 of 50 |
| The same with the WASAPI start-up helper (`7a9337c`) | `20261108-154503-soak` | Soak 56 of 56 |
| clang-tidy fixes (`b622ea4`) | `20261108-162016-full`, `20261108-163043-soak` | Full 50 of 50, Soak 56 of 56 |

- On the development PC (one 1920 x 1080 display at 125 %), in single runs:
  `pin_layout_logic`, `pinned_images_native` (five runs),
  `capture_workspace_production`, `scrollshot_workspace_production`,
  `data_safety_questions`, `text_fits_in_every_language` and the Fast tier.
- clang-tidy over the 51 application sources: 46 findings before the fixes, 0
  after.
- The owner tried Pin to screen by hand on the development PC on 2026-11-08:
  everything the feature promises worked.

## Validation of v1.1.0

The stable release, prepared on the branch `stabilize/v1.1.0` on 2026-11-07
from the release candidate below with the two corrections from the owner's
check and an empty prerelease label; the [release notes](v1.1.0.md)
summarize it.

- **Final sources** (tree `c8de8c4297a3`, the code of `5a5490c` with the
  label emptied):
  - **Soak at 125 %: 54 of 54 with no retry** (run `20261107-232024-soak`,
    14 min 45 s).
  - The Soak run before it on the same tree (`20261107-225336-soak`,
    15 min 13 s) passed 53 of 54: `recording_worker_foundations` failed one
    check, `worker.pacing_slots_missed() == 0`; its capture thread woke after
    a frame's due time once while tests ran in parallel. The test had passed
    in all 15 earlier lab runs that included it. Neither the test nor the
    worker changed since v1.0.0.
  - **Full at 150 %: 48 of 48 with no retry** (run `20261107-230849-full`,
    10 min 9 s).
  - In each, `real_desktop_recording_session` passed on the GDI fallback
    (`VM-ok-but-fallback`).
- **The two corrections** (`4392b5e`, `eaa65f8`): Full at 125 % 48 of 48 on
  each (`20261107-222006-full`, `20261107-223026-full`) and Full at 150 %
  48 of 48 on `eaa65f8` (`20261107-224047-full`). The probes and the runs on
  the development PC are listed under the release candidate.
- **Why the lab missed the caption:** both the development PC and the 125 %
  image run visual styles (Aero), and DWM draws the non-client area of the
  layered dialog in neither. With dark mode for apps turned on in the image,
  as on the development PC, the press on the header still sent no
  `WM_NCUAHDRAWCAPTION` (probe `20261107-225106`, guest setting set back
  afterwards). The graphics adapter and the update revision remain as
  differences ([ADR-037](../decisions/ADR-037-isolated-test-lab.md#not-validated)).
- **Hosted runner:** the Build run of pull request #17 (run `42411860509`)
  and the Release run of the tag (`42413078058`) succeeded. In both, Release,
  Debug, AddressSanitizer, Format and repository hygiene passed, and so did
  `mp4_trim_export_backend` and `diagnostics_log_and_crash_reports`, which had
  failed there before. clang-tidy, report only, again left 47 findings. CodeQL
  passed on the pull request.
- **The draft** before publication: the setup, the ZIP and the symbols ZIP
  matched their SHA-256 files, `gh attestation verify` accepted the setup and
  the ZIP, and `Easnap.exe`, `EasnapPreviewHost.exe` and the setup reported
  1.1.0. Published on 2026-11-08 as the latest release.

## Validation of v1.1.0-rc.1

This section records the release candidate prepared on the branch
`stabilize/v1.1.0` on 2026-11-07; the [release notes](v1.1.0-rc.1.md)
summarize it. What is still not validated is listed in
[project status](../project-status.md#not-validated).

The lab now has two images, both Windows 10 Pro 22H2 with one 1920x1080
display and no GPU: the one of beta.2 at 125 % and a second at 150 %
(`easnap-lab-150`, built on 2026-11-07 at build 19045.6456; see
[testing](../testing.md#lab)).

- **Final sources** (`60d6ab4` with the version label `rc.1`, the release
  notes and the two test corrections below; tree `a05d93dccc65`):
  - **Soak at 125 %: 54 of 54 with no retry** (run `20261107-193138-soak`,
    14 min 46 s).
  - **Full at 150 %: 48 of 48 with no retry** (run `20261107-193830-full`,
    10 min 7 s).
  - In both, `real_desktop_recording_session` passed on the GDI fallback
    (`VM-ok-but-fallback`), the provider's documented behaviour on a still
    desktop.
  - Three probes of the two corrected tests at 150 % (`20261107-193133`,
    `20261107-194926`, `20261107-195231`, each running them twice) passed.
    Over these six executions and the Full run, `trim_timeline_seek_stress`
    made 925 pointer moves: one landed late (30 ms), none had to be issued
    again and none was lost.
- **Runs on the way:**
  - Before the text-fit correction, at 125 %: Full 48 of 48
    (`20261107-163328-full`) and Soak 54 of 54 (`20261107-164346-soak`).
  - The first Standard run at 150 % (`20261107-163347-standard`): 38 of 39.
    `text_fits_in_every_language` failed 22 of 416 checks, the Scrollshot
    status message and the editor's value labels; corrected
    ([ADR-032](../decisions/ADR-032-interface-text-in-string-tables.md#text-has-to-fit)).
  - With that correction: Soak at 125 % 54 of 54 (`20261107-184830-soak`).
    The first Full run at 150 % (`20261107-190243-full`) passed 46 of 48:
    `annotation_editing_native` and `trim_timeline_seek_stress` failed on the
    first try and on the retry. Both failures came from assumptions in the
    tests, not from Easnap, and the tests were corrected
    ([ADR-037](../decisions/ADR-037-isolated-test-lab.md#the-image-at-150--2026-11-07)).
- **Upgrade from v1.0.2**, in the 125 % lab VM restored from its golden
  checkpoint, through PowerShell Direct: the v1.0.2 installer and then the
  rc.1 installer ran silently and both exited with 0. Afterwards Windows
  listed one Easnap entry (the same installer AppId) at version 1.1.0-rc.1,
  `Easnap.exe` and `EasnapPreviewHost.exe` reported 1.1.0-rc.1, a settings
  file written between the two installs had the same SHA-256, and the Start
  menu held one Easnap shortcut. The installer's pages, an upgrade from
  beta.2 and a return to v1.0.2 were not run there.
- **Packages,** built locally from the final sources with
  `package-release.ps1`: the portable ZIP, the symbols ZIP and the installer,
  each with a SHA-256 file. `package-manifests.ps1` wrote the winget
  manifests (identifier `vseprr.Easnap.Beta`, schema 1.28.0), which pass
  `winget validate`, and the Scoop manifest; nothing was submitted.
- **Static checks:** all 150 C++ files pass `scripts/format.ps1 -Check`.
  clang-tidy, report only, left 47 findings at `74b75d7`.
- **Hosted runner:** the branch has not been run there; that happens when
  the pull request is opened.
- **The owner's check by hand** of the first rc.1 package on the development
  PC (2026-11-07) found two problems and nothing else: two arrows next to
  Appearance in the tray menu, and a white button over the Close button of
  Settings after moving it. For both, `island_capture_exclusion` gained a
  check that reads the window as the user sees it, with its exclusion lifted,
  and compares it with Easnap's own painting. Probes in the 125 % lab VM, each
  running the test twice:
  - `20261107-215252`, on the unchanged sources: the menu's submenu item
    differed in 25 pixels at its right end, Windows' own arrow; a message
    dialog dragged by its header changed nothing in the header.
  - `20261107-221132`, with the item clipped after drawing: 0 pixels in the
    dark and the light appearance. The Exit question and Settings dragged
    while active, after another window was active and with the pointer on
    Close first, in both appearances: no pixel of the Close button differed
    from Easnap's painting in any of the seven cases. The white button was not
    reproduced.
  - On the development PC, at the owner's request, the same test: every drag
    left 845 pixels of the Close button (dark) or 520 (light) that Easnap had
    not painted, a classic caption. Windows sent the dialog
    `WM_NCUAHDRAWCAPTION` four times on the press; in the lab VM it sent none.
    With the chrome swallowing that message and `WM_NCUAHDRAWFRAME`, the test
    passed there with 225 of 225 checks
    ([ADR-008](../decisions/ADR-008-recording-ui-and-safe-finalization.md)).
