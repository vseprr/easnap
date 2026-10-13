# Project status

**Updated:** 2026-10-13
Read with the [roadmap](../EASNAP_ROADMAP.md) before implementation work.

## Current slice: v1.1.0-beta.1 preparation

Work is on the branch `feature/v1.1-beta`, on top of the published v1.0.2.
Nothing from this branch has been released.

### Implemented on the branch

- **Scrollshot.** A third workspace mode joins everything scrolled through a
  locked frame into one tall image; see [user flows](user-flows.md#scrollshot).
  The stitching engine ([ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md)),
  the capture session and the workspace integration
  ([ADR-028](decisions/ADR-028-scrollshot-capture-workspace.md)) are in the
  application: mode button, Start/help island, scissors button on the tools
  island, the four-action island with status and live preview, Copy, Save,
  Edit and Discard, and survival of a display change.
- **Editing a stitched image.** The screenshot editor opens an image larger
  than the screen, scrolls and zooms it, and exports the whole image
  ([ADR-029](decisions/ADR-029-scrolled-and-zoomed-image-editing.md)).
- **Log file and crash reports**, both local
  ([ADR-030](decisions/ADR-030-log-file-and-crash-reports.md)).
- **Unsaved work survives.** Exit and sign-out ask or are held back, a
  finished recording is kept and offered at the next start, discarding asks
  when there is something to lose, and failure messages say what is true
  ([ADR-031](decisions/ADR-031-unsaved-work-survives.md)).
- **English and Turkish.** All text the user reads comes from two string
  tables; the language follows Windows or the choice in Settings
  ([ADR-032](decisions/ADR-032-interface-text-in-string-tables.md)). Controls
  whose Turkish text is longer size themselves to it.
- **Settings and the shell.** A Settings dialog (language, appearance
  including "same as Windows", shortcut, start with Windows, cursor, folders,
  saving screenshots without a dialog), About with details for a bug report,
  "Check for updates", a taken shortcut reported in a notification instead of
  a dialog, a second start that captures, a quiet start at sign-in and a
  refusal to start below Windows 10 build 19041
  ([ADR-033](decisions/ADR-033-settings-shortcut-and-launch-behaviour.md)).
- **Windows N editions.** `Easnap.exe` no longer needs Media Foundation to
  start; Record explains what is missing
  ([ADR-034](decisions/ADR-034-media-foundation-on-first-use.md)).
- **Version text.** The build is 1.1.0-beta.1: `EASNAP_VERSION_PRERELEASE` in
  the root `CMakeLists.txt` adds the label to the numeric version, and the
  version resource carries it together with the prerelease flag.
- **Release and distribution.** Package names, checks and the Release
  workflow follow the version text; a tag with a label becomes a prerelease.
  Release builds keep symbol files, which are packaged and published beside
  the packages. The installer is in English and Turkish, requires build
  19041, removes the sign-in entry and asks about settings, log and crash
  reports when uninstalling. `scripts/package-manifests.ps1` writes winget
  and Scoop manifests. See
  [ADR-035](decisions/ADR-035-prerelease-versions-and-distribution.md),
  [distribution](distribution.md) and the
  [release notes](releases/v1.1.0-beta.1.md).
- **A recording whose preview fails can be saved** as it is, and a missing
  preview helper is named
  ([ADR-031](decisions/ADR-031-unsaved-work-survives.md)).
- **CI and community files.** A CodeQL workflow for the application sources,
  an AddressSanitizer job beside the build (`EASNAP_SANITIZE_ADDRESS`; not a
  required check yet), issue forms and a pull-request template. A test that
  cannot run where it is counts as skipped, in CTest and in `test.ps1`,
  instead of as passed: `wasapi_audio_capture` says so when the machine has no
  audio endpoint.
- The capture provider can return a plain image of a region and wait for the
  desktop to change (`CaptureRegionImage`).
- An image copied to the clipboard is written bottom-up; .NET applications
  could not read the top-down bitmap Easnap wrote before.
- **Text box caret.** While typing into a text annotation, the native caret
  could leave a one-pixel inverted column at the position the native control
  computes, until the next paint. The caret is now hidden for the whole
  message.

### Validated on the branch

One development PC, Windows 10 build 19045, NVIDIA GPU, two 1920x1080 displays
at 125%; visible-desktop tests and manual checks ran on the secondary display.

- Release x64 with warnings as errors builds. On 2026-10-13 the Standard tier
  passed 37 of 37 on the final binaries in one run.
- **Full tier, two runs on 2026-10-13.** The first passed 45 of 45 without a
  retry. The second, on the binaries with symbol files and the change to the
  trim window, passed 44; `trim_timeline_seek_stress` stopped at its pointer
  check (the real pointer was not where the test had put it; the PC was in
  use) and passed when it was run again alone.
- **`annotation_editing_native` is no longer intermittent, for two reasons
  that were found.** A build that failed it deterministically (24 checks, four
  runs in a row) showed a one-pixel column at the native caret position: the
  product defect described above. The occasional failure on a reopened text
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

Not validated on the branch:

- Scrollshot on real applications (browsers, editors, lists that load as they
  scroll), at a scale other than 125%, on mixed-DPI or portrait monitors, on a
  second GPU, with wheel routing by focus, and through a display change during
  capture. The engine's behaviour on such content is covered by synthetic
  documents only.
- **A Windows N edition, any Windows 11, any build other than 19045.** The
  refusal below build 19041 and the Record message on an N edition have not
  been seen on such a system.
- **"Start with Windows" in the running application.** The registry code is
  tested against a key of its own; the box was not ticked in the real
  application, because that writes the user's `Run` key. The quiet start at
  sign-in was therefore not seen either.
- "Same as Windows" appearance following a change of the Windows setting, and
  high contrast: both need a change to Windows settings.
- The folder picker of Settings, "Check for updates" and "Website": they open
  a system dialog or the browser and were not driven.
- **The installer.** It was compiled, not run: installing would replace the
  copy of the person who owns the development PC. Its Turkish pages, the
  uninstall question and the removal of the sign-in entry are therefore
  unseen.
- The Release workflow with a prerelease tag, and the manifests against
  `winget validate` or Scoop. Nothing was pushed, tagged or dispatched.
- The CodeQL workflow and the AddressSanitizer job on a hosted runner: neither
  has run. The skipped result of `wasapi_audio_capture` was not seen either;
  this PC has audio endpoints.
- The look of the Settings dialog on screen. Its windows are excluded from
  capture, so no picture was taken; its text is measured by the text-fit test.
- A real crash of the application: the handler is exercised by a test process
  that links the same module, not by a failing Easnap.
- A real sign-out or shutdown: the two session messages were sent to the
  application by hand.
- The Soak tier. Debug was built and run only for the stitching engine and the
  diagnostics test.
- Turkish text has been read in the running application; it has not been
  reviewed by a second reader.

### Known limitations of the new work

- Scrollshot is manual and vertical; the image only grows downwards.
- Copying and saving a very tall image run on the UI thread.
- At the 48-million-pixel limit, editing holds two buffers of about 190 MB
  and an export allocates a third.
- A recording that is still being written when Easnap stops cannot be
  recovered. A recovered recording is the untrimmed MP4.
- A screenshot taken with the workspace visible can differ from the screen by
  one level at a few values (measured: 47 as 46, 70 as 69): Windows changes
  them under the translucent, capture-excluded shading window. Scrollshot
  avoids it; screenshots do not yet.
- A recording that starts on a still desktop is read through GDI for its whole
  length. Scrollshot no longer is; the same remedy has not been applied to
  recording.
- A window that is open when the language changes keeps its text; only the
  Settings dialog can be open then, and it closes.
- HDR displays are unverified: no HDR output was available.
- Tests share the user's clipboard: `capture_workspace_production` and
  `annotation_editing_native` replace its content.
- The log receives one line per audio packet for the first packets of every
  recording; these lines are diagnostic and carry no content.
## Previous slice: v1.0.2 maintenance and hardening

v1.0.1 followed the published v1.0.0 with fixes, hardening and test tooling.
v1.0.2 adds the two changes listed under [v1.0.2](#v102) below.
No feature was added and no phase was started. Distribution is a per-user
installer and a portable ZIP built from the same two executables, both
unsigned, each with a SHA-256 file and a build provenance attestation.

### Implemented in v1.0.1

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

Tests and tooling ([testing](testing.md),
[ADR-026](decisions/ADR-026-test-tiers-and-desktop-isolation.md)):

- Tiers: fast, integration, desktop, soak. Repeated stress seeds, 30-second
  seek runs and the 72-click timeline run are soak; no assertion was removed.
- Three message-driven window tests run on a private, non-input desktop.
- Visible-desktop tests run last on a chosen display with a warning,
  countdown, pointer restore and one reported retry.
- The pass cache hashes only `EASNAP_*` environment variables.
- New table-driven tests cover trim layout across 10 work areas and 9 scale
  factors, and selection hit areas across scales and negative origins.
- Per-user installer (`packaging/easnap.iss`) built by `package-release.ps1`.

### v1.0.2

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
## Validation

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
    branch; see [the current slice](#validated-on-the-branch).
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

## Known limitations and unresolved risks

Carried over from v1.0.0:

- Strict resize diagnostics remain failed: 220 cases, 61,095 desktop frames,
  59 visual-failing cases and 12 minimum-width assertions; all 4,185 settled
  samples were clean. Transient video/control overlap and duplicated caption
  pixels occur during motion/maximize. No cause is established. These
  diagnostics are opt-in modes, not registered tests; a green CTest run does
  not close them. See the
  [Trim validation report](trim-endpoint-artifact-validation.md).
- Native startup/pause/drain can wait inside Windows; full-range publication
  is synchronous; undo history is uncapped; late MP4 trims decode from source
  start; codec endpoints and GIF timing/palette have granularity. Low size GIF
  can omit tiny visual changes.

Found in the v1.0.1 review and not fixed:

- The capture path still allocates a frame image and an NV12 buffer per frame.
- Up to v1.0.1, an MP4 trim of a recording whose height is not a multiple of
  16 is wrong when exported on a machine without a hardware H.264 encoder
  that reads Direct3D surfaces. The recording itself is intact; export again
  with v1.0.2 or later.
- The repacking of a locked hardware surface was exercised only on the hosted
  runner's WARP adapter, not on a physical GPU with a software encoder.
- Editor pixel effects still repaint the whole image for a spotlight preview
  and for moving any annotation, although only the changed rectangle is
  recomputed.
- `recording_pause_stop_stress` exceeded its 100 ms duration limit once.
- The application's UI thread writes acknowledgements to the helper pipe
  synchronously; a helper that never reads could block it. The helper is the
  application's own process, so this needs a faulty helper.
- `CreateSession` follows a reparse-point storage root that deletion would
  refuse. A session lease that cannot be reacquired after a failed delete is
  only logged.
- Mixed-DPI drags, HDR outputs, L-shaped monitor layouts, a top-docked taskbar
  over the mode toolbar and the configuration island at 200 % on a 768-row
  display are unverified or known to be imperfect.
- Not every visible-desktop test follows the chosen display.
- `capture_overlay_interaction` fails three checks on the hosted runner's
  small desktop. Whether that is only a test assumption or also a layout
  problem at low resolution is not established.
- CI is pinned to `windows-2022` because the project uses the Visual Studio
  17 2022 generator; moving to a newer toolchain is pending.

Camera, Mouse Effects, device pickers and OCR are not implemented. OCR remains
post-v1 without a promised date.

## Next slice

For v1.1.0-beta.1, in the order of the [roadmap](../EASNAP_ROADMAP.md): the
documentation pass and the final validation.
Preserve the native frame, shared rendering/control path and asynchronous
preview boundaries. See [ADR-024](decisions/ADR-024-native-trim-frame.md),
[architecture](architecture.md) and the
[window guide](ui/window-implementation-guide.md).