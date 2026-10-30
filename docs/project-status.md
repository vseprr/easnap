# Project status

**Updated:** 2026-10-30

The current state of the implementation: what is built, what was actually
validated, what is open and the next slice. Read it with the
[roadmap](../EASNAP_ROADMAP.md) before implementation work. Where and how tests
run is in [testing](testing.md), durable decisions are in the
[ADRs](README.md#decisions), what each release contained is in its release
notes under `docs/releases/`, and the individual validation runs of past
releases are in the [validation records](releases/validation-history.md).

## Baseline

- Latest stable release: v1.0.2. Latest prerelease:
  [v1.1.0-beta.2](https://github.com/vseprr/easnap/releases/tag/v1.1.0-beta.2),
  published on 2026-10-30 from `main` (tag on the merge of pull request #13),
  with the MP4 writer change for software H.264 encoders and the beta feedback
  corrections, both described below
  ([release notes](releases/v1.1.0-beta.2.md)). The isolated test lab is a
  development aid and ships nothing.
- The Release workflow built, tested and attested the packages on the hosted
  runner; the draft's SHA-256 files and the installer's attestation were
  verified and both executables report 1.1.0-beta.2 before it was published.

## Beta feedback corrections: v1.1.0-beta.2 (2026-10-30)

The owner's report on the first beta, worked through in one slice and
released as v1.1.0-beta.2:

- **"Saved" notice cut off.** The Copied/Saved notices had a fixed width of
  250 DIP; the Turkish "Saved" text needs 385 px at 125 % and was cut. The
  notices now size themselves to their text (one line while the work area
  allows, otherwise wrapped, never smaller than before). The notices are in
  `text_fits_in_every_language`; with the old width that check fails on the
  Turkish text.
- **Tools island jumped above the frame.** Choosing a tool fixes the
  screenshot and shows the property island below the tools island; when the
  two no longer fitted below the frame, the group was placed above it. The
  islands are now placed around the frame with room for the tallest property
  island any tool shows, so choosing a tool moves nothing. An island the user
  moved keeps its own size at the work-area edge. `capture_island_placement`
  checks that choosing a tool and drawing move no island.
- **Right-click in a locked Scrollshot** now works like Esc: it requests
  Discard, which asks first when there is a scrolled image
  ([ADR-028](decisions/ADR-028-scrollshot-capture-workspace.md)).
- **`trim_timeline_seek_stress` instability** came from the test, not the
  product: a pointer move that Windows reports late. The test now waits for
  it. A second cause was the lab's "new network" flyout, now turned off in
  provisioning. See
  [ADR-037](decisions/ADR-037-isolated-test-lab.md#why-trim_timeline_seek_stress-failed-its-pointer-check-2026-10-30).
- **Black boxes in pictures where an island was.** A window excluded from
  capture leaves a black hole of its shape when it is shown again or moved
  over content that is not redrawn. This put the tools island into a
  screenshot whose frame had been dragged under it and the recording panel
  into a GIF. Every excluded island is now a layered window at full opacity
  (`ui::ExcludeFromCapture`), which never leaves the hole, and tooltips, which
  Windows did not exclude at all, are excluded each time they are shown.
  `island_capture_exclusion` measures both; see
  [ADR-008](decisions/ADR-008-recording-ui-and-safe-finalization.md#capture-exclusion-of-the-islands-2026-10-30).
- **Installer language.** Choosing English in the installer and getting a
  Turkish Easnap is the documented behaviour: the installer's language applies
  to its own pages; Easnap follows Windows (here Turkish) until a language is
  chosen in Settings. The installed copy and a development build share
  `%LOCALAPPDATA%\Easnap\settings.ini`.

Validated: the Full tier in the lab passed 48 of 48 with no retry on the final
tree (2026-10-30, run `20261030-230104-full`; `trim_timeline_seek_stress`
reported 133 pointer moves, none late). The Full run before it, on the same
sources, failed only `mfplay_preview_audio_transport` at "Close retires a
suspended native host within 1500 ms"; that test builds none of the changed
sources and passed in both probe variants alone, so it is a timing check that
can miss its limit under load. An earlier Full run failed
`annotation_editing_native` twice: the room kept for the property island also
held back an island the user had moved to the bottom edge; corrected. The
private-desktop tests `capture_island_placement`,
`scrollshot_workspace_production`, `text_fits_in_every_language`,
`capture_overlay_interaction`, `capture_frame_interaction` and
`data_safety_questions` passed on the development PC. `island_capture_exclusion`
ran on the development PC's SAMSUNG display with the owner's permission
(68 of 68) before its still-desktop part existed; with that part it ran in the
lab only (108 of 108).
Not validated: the corrections in the running application by hand, the
still-desktop part of `island_capture_exclusion` on the development PC, and a
Full run on the host.

## Isolated test lab (2026-10-19)

Tests run in the Hyper-V VM `easnap-lab`, driven by winlab, configured by
`testenv.json` and called through `scripts/lab.ps1`; how to use it is in
[testing](testing.md#lab), the design and every measured figure in
[ADR-037](decisions/ADR-037-isolated-test-lab.md).

Implemented: `lab.ps1 run`, `probe`, `parity` and `verify`; the verdict and
its Turkish summary; `build.ps1 -TestMode Build` and the direct build
`build.ps1 -Trust Lab`; `test.ps1 -HostOnly`; capture-path reporting;
`recording_pause_stop_stress` skips without an audio source; Windows Update
and a conservative debloat during provisioning (`testenv.json`); a
"LABORATUVAR" section in the Desktop menu.

Validated on 2026-10-19 and 2026-10-20:

- The lab's first runs found a product defect: without a hardware H.264
  encoder the MP4 writer stamped the first video frame one frame late, which
  the trim preview refused. The writer now asks the encoder for no B-frames
  and low latency; the four Media Foundation background tests still pass on
  the development PC with the NVIDIA encoder.
- On the corrected tree: probe 10 of 10 in both observer variants (Desktop
  Duplication works in the VM, no test is host-only), Standard 39 of 39 in
  about 5 min, Full 47 of 47 in about 9.5 min. `trim_timeline_seek_stress`
  was unstable in the VM as on the host (cause found and corrected on
  2026-10-30, see above). `real_desktop_recording_session`
  reported the GDI fallback on the still desktop, as the provider documents,
  and the verdict marked it `VM-ok-but-fallback`.
- After the guest's Windows update step (golden checkpoint of 2026-10-20,
  build 19045.6456), a Full run passed 47 of 47 with
  `trim_timeline_seek_stress` passing on its retry.

Known limitations: one Windows 10 22H2 image with one display, a virtual
audio cable and no GPU; `lab.ps1 parity` lists the differences from the host.
A green verdict does not cover what those differences touch, and it is a
local aid, not a release gate.

## The v1.1 beta: v1.1.0-beta.1 and what it contains

The branch `feature/v1.1-beta` was merged into `main` and published on
2026-10-13 as the prerelease
[v1.1.0-beta.1](https://github.com/vseprr/easnap/releases/tag/v1.1.0-beta.1).
Everything below also ships in v1.1.0-beta.2.

### Implemented in the beta

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
- **Islands around the frame.** The tools island, the recording panel and the
  mode island follow one placement rule: outside the frame while anything
  outside has room, over its bottom otherwise, and never over the mode island,
  which moves along the top edge when the frame takes its place and hides
  while the frame is dragged
  ([ADR-036](decisions/ADR-036-island-placement-around-the-frame.md)). With
  a frame of full height the tools island used to land on the mode island.
- **Text box caret.** While typing into a text annotation, the native caret
  could leave a one-pixel inverted column at the position the native control
  computes, until the next paint. The caret is now hidden for the whole
  message.

### Validated on the branch

On one development PC (Windows 10 build 19045, NVIDIA GPU, two 1920x1080
displays at 125 %) on 2026-10-13: the Release x64 build with warnings as
errors; Standard 39 of 39 and Full 46 of 47 on the final binaries, with
`ui_modernization_native` passing on its retry (the open resize limitation
below); the new tests' checks; the running application driven by hand through
Scrollshot, data safety, Settings, saving without a dialog, a recording with
Media Foundation loaded on first use, library loading, island placement and a
recording without a preview; 30 tests under AddressSanitizer; packaging and
manifests. The individual runs and what each covered are in the
[validation records](releases/validation-history.md#validation-of-v110-beta1).

### Not validated on the branch

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
- **The installer, by an agent.** It was compiled, not run by an agent:
  installing would replace the owner's copy. The owner has since installed the
  beta on the development PC with the English pages and used it (reported
  2026-10-30); its Turkish pages, the uninstall question and the removal of
  the sign-in entry remain unseen.
- The manifests against `winget validate` or Scoop; nothing was submitted to
  a package manager.
- The published packages themselves. The Release workflow built them from the
  tag, checked their SHA-256 files, attested them and created the prerelease
  draft with the installer, the ZIP and the symbol files; nobody downloaded
  or ran them before publication.
- On the hosted runner the CodeQL workflow and the AddressSanitizer job pass,
  and `wasapi_audio_capture` is reported as skipped. In one
  AddressSanitizer run `incremental_test_selection`, which takes 5 to 7 s
  there, ran into its 30 s timeout and passed when the job was repeated; the
  cause is not known.
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
- On the host, tests share the user's clipboard: `capture_workspace_production`
  and `annotation_editing_native` replace its content.
- The log receives one line per audio packet for the first packets of every
  recording; these lines are diagnostic and carry no content.
- A top-docked taskbar still overlaps the mode island.

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

- Strict resize diagnostics remain failed (2026-09-30): 220 cases, 61,095
  desktop frames, 59 visual-failing cases and 12 minimum-width assertions; all
  4,185 settled samples were clean. Transient video/control overlap and
  duplicated caption pixels occur during motion/maximize. No cause is
  established. These diagnostics are opt-in modes, not registered tests; a
  green CTest run does not close them. The per-run figures are in
  [ADR-024](decisions/ADR-024-native-trim-frame.md#known-limitation-transient-artifacts-while-resizing).
- Private memory of the trim window's process and of the preview helper grew
  by about 22 MiB each during the slow resize run; whether in the product or
  in the observer is not known, so freedom from leaks during long resize
  sessions is not established (figures in ADR-024).
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
- Mixed-DPI drags, HDR outputs, L-shaped monitor layouts and the
  configuration island at 200 % on a 768-row display are unverified or known
  to be imperfect.
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

v1.1.0-beta.2 is published. The betas' feedback decides the work before
v1.1.0; the checks listed above as not validated (Windows 11, an N edition,
Scrollshot on real applications, other scale factors, the installer) come
first. The package manager submissions remain the owner's step
([distribution](distribution.md)).

v1.1.0-beta.2 carries the beta feedback corrections. The owner checked the
black-box fix by hand on the development PC; the installer of beta.2 and its
uninstall questions are checked by hand after publication.

Tests run in the lab: `scripts/lab.ps1 run -Tier Standard|Full`, see
[testing](testing.md#lab). The host desktop is used only when the owner asks;
the host Full run for the MP4 writer change is still open on that basis.

Preserve the native frame, shared rendering/control path and asynchronous
preview boundaries. See [ADR-024](decisions/ADR-024-native-trim-frame.md),
[architecture](architecture.md) and the
[window guide](ui/window-implementation-guide.md).