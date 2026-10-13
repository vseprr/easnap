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
- **Full tier: 45 of 45 in one run, none retried, none skipped** (2026-10-13,
  visible-desktop tests on the secondary display). After that run only two
  source comments changed; the Standard tier passed again on that build. Two
  earlier Full runs of the same day: 45 of 45 before the symbol files and the
  change to the trim window; then 44, where `trim_timeline_seek_stress`
  stopped at its pointer check (the real pointer was not where the test had
  put it; the PC was in use) and passed when it was run again alone.
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
  - Library loading: with unrelated DLL files named `mfplat.dll` and
    `mfreadwrite.dll` beside `Easnap.exe`, a recording loaded both libraries
    from the Windows system directory and worked.
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

## Earlier releases

v1.0.1 and v1.0.2 were maintenance releases on top of v1.0.0: defect fixes,
security hardening, display-change robustness, a per-user installer beside the
portable ZIP, the tiered test suite, incremental redaction rendering and a fix
for MP4 trims of pictures the decoder pads. What changed is in the release
notes ([v1.0.0](releases/v1.0.0.md), [v1.0.1](releases/v1.0.1.md),
[v1.0.2](releases/v1.0.2.md)); the individual validation runs, including the
ones that failed, are in the
[validation records](releases/validation-history.md).

## Known limitations and unresolved risks

The limitations of the v1.1 work are listed
[above](#known-limitations-of-the-new-work).

Open since v1.0.0:

- Strict resize diagnostics remain failed: 220 cases, 61,095 desktop frames,
  59 visual-failing cases and 12 minimum-width assertions; all 4,185 settled
  samples were clean. Transient video/control overlap and duplicated caption
  pixels occur during motion/maximize. No cause is established. These
  diagnostics are opt-in modes, not registered tests; a green CTest run does
  not close them. See [ADR-024](decisions/ADR-024-native-trim-frame.md).
  Per run on 2026-09-30 (secondary display, 1920x1080 at 125%, both themes):

  | Run | Cases | Desktop frames | Visual-failing cases | Minimum-width assertions | Settled samples, all clean |
  |---|---:|---:|---:|---:|---:|
  | Release, fast | 60 | 14,726 | 22 | 5 | 1,193 |
  | Debug, fast | 60 | 12,854 | 16 | 1 | 1,060 |
  | Release, slow | 60 | 23,870 | 10 | 4 | 1,149 |
  | Status, progress and Cancel visible | 40 | 9,645 | 11 | 2 | 783 |

  The longest measured resize dispatch was 47 ms.
- During the slow resize run, private memory rose from about 65.1 to 86.6 MiB
  in the process that owns the trim window (the test executable, which also
  runs the capture observer) and from 68.4 to 90.6 MiB in the preview helper
  (2026-09-30). The run does not show whether the growth is in the product or
  in the observer; freedom from leaks during long resize sessions is not
  established.
- Native startup/pause/drain can wait inside Windows; full-range publication
  is synchronous; undo history is uncapped; late MP4 trims decode from source
  start; codec endpoints and GIF timing/palette have granularity. Low size GIF
  can omit tiny visual changes.

Open since the v1.0.1 review:

- The capture path still allocates a frame image and an NV12 buffer per frame.
- The repacking of a locked hardware surface was exercised only on the hosted
  runner's WARP adapter, not on a physical GPU with a software encoder.
- Editor pixel effects still repaint the whole image for a spotlight preview
  and for moving any annotation, although only the changed rectangle is
  recomputed.
- `recording_pause_stop_stress` exceeded its 100 ms duration limit once, during
  the validation of v1.0.2.
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

Files made with older versions:

- Up to v1.0.1, an MP4 trim of a recording whose height is not a multiple of
  16 is wrong when exported on a machine without a hardware H.264 encoder
  that reads Direct3D surfaces. The recording itself is intact; export it
  again with v1.0.2 or later.

Camera, Mouse Effects, device pickers and OCR are not implemented. OCR remains
post-v1 without a promised date.

## Next slice

The branch is prepared for v1.1.0-beta.1 and nothing has been published.
Publishing is the owner's step: merge or tag `v1.1.0-beta.1`, run the Release
workflow, review the draft ([distribution](distribution.md)). After that the
beta's feedback decides the work before v1.1.0; the checks listed above as not
validated (Windows 11, an N edition, Scrollshot on real applications, other
scale factors, the installer) come first.
Preserve the native frame, shared rendering/control path and asynchronous
preview boundaries. See [ADR-024](decisions/ADR-024-native-trim-frame.md),
[architecture](architecture.md) and the
[window guide](ui/window-implementation-guide.md).