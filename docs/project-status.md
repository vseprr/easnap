# Project status

**Updated:** 2026-11-08

The current state of the implementation: what is built, what was actually
validated, what is open and the next slice. Read it with the
[roadmap](../EASNAP_ROADMAP.md) before implementation work. Tests are described
in [testing](testing.md), decisions in the [ADRs](README.md#decisions), each
release in its release notes (listed in the [documentation index](README.md)),
and the individual runs in the [validation records](releases/validation-history.md).

## Baseline

- Latest stable release:
  [v1.1.0](https://github.com/vseprr/easnap/releases/tag/v1.1.0), published on
  2026-11-08 from `main`, tag on the merge of pull request #17
  ([release notes](releases/v1.1.0.md)). The latest prerelease before it was
  v1.1.0-beta.2 (2026-10-30).
- The Release workflow built, tested and attested its packages on the hosted
  runner. Before publication the draft's three packages matched their SHA-256
  files, the installer's and the ZIP's attestations verified, and both
  executables and the installer reported 1.1.0.
- Stabilization toward v1.1.0 ran on the branch `stabilize/v1.1.0`, created
  on 2026-11-07 from `main` at `42b3efa` and merged by pull request #17. Its
  release candidate rc.1 was checked by the owner and not published
  ([record](releases/v1.1.0-rc.1.md)).
- Since beta.2 (all on that branch): the 2026-11-07 audit's code findings
  (recovery messages that promised too much, dialogs and lists excluded from
  capture the way the islands are, crash reports that could replace each
  other, the mode island under a top taskbar, failure paths that could end
  Easnap when memory ran out, stale comments), the C++ sources formatted with
  `.clang-format` and checked in CI, a report-only clang-tidy job, and a
  second lab image at 150 % (`lab.ps1 -Variant Scale150`), whose first runs
  led to the 150 % text-fit correction and to two visible-desktop tests that
  no longer assume 125 %. From the owner's check of rc.1: the tray menu's
  submenu arrow is no longer drawn twice; Windows no longer paints its own
  caption over Settings and the message dialogs when they are moved; and
  `island_capture_exclusion` drags those dialogs and reads the themed menu as
  the user sees them.

## v1.2 in progress

Work toward v1.2 runs on the branch `feature/v1.2`, created on 2026-11-08 from
`main` at `df838d4`. Nothing of it is released.

- **Pin to screen** ([ADR-038](decisions/ADR-038-pinned-images.md)):
  implemented. The tools island has a Pin command (Ctrl+T) that keeps the
  finished image above other windows as a pinned image; the island's preferred
  width grew from 832 to 870 DIPs for it. A pin moves by dragging, zooms with
  the wheel (10 to 400 %), copies, saves, returns to its original size and
  closes from the keyboard and its menu. Pins are screen content: later
  captures contain them, and they do not make acquisition hide the workspace
  or refuse Scrollshot. The editor and the pin share one Save path for quick
  save and the Save dialog.
- Validated: lab Full tier on the uncommitted branch (2026-11-08, run
  `20261108-021123-full`), 50 of 50 with no retry, including the two new tests;
  `real_desktop_recording_session` passed on the GDI fallback as before
  (`VM-ok-but-fallback`). On the host, in single runs: `pin_layout_logic` (56
  checks), `pinned_images_native` (71 checks, five runs), the new Pin checks in
  `capture_workspace_production`, and `text_fits_in_every_language`.
- By hand: the owner tried the build on the development PC (one 1920 x 1080
  display at 125 %) on 2026-11-08 and reported that everything the feature
  promises worked; longer use is still to come.
- Not validated: a pin moved between monitors of different scale (the PC has
  one display), long use, and many or very large pins at once. The native test
  reads the pin's picture through `WM_PRINTCLIENT`, not from the screen.

## Validated

- v1.1.0-beta.2 (2026-10-30): the Full tier passed 48 of 48 with no retry in
  the lab; the owner checked the black-box fix by hand on the development PC.
- Stabilization baseline on the beta.2 sources (2026-11-07, lab): Standard 39
  of 39 and Soak 54 of 54, no retry; `real_desktop_recording_session` passed
  on the GDI fallback (`VM-ok-but-fallback`).
- v1.1.0-rc.1, final sources (2026-11-07, lab): Soak 54 of 54 at 125 % and
  Full 48 of 48 in the image at 150 %, no retry, the same GDI fallback in
  both. A silent upgrade from v1.0.2 in the lab left one Easnap entry at
  1.1.0-rc.1 and the settings file unchanged.
- v1.1.0, final sources (2026-11-07, lab): Soak 54 of 54 at 125 % on the
  second run (the first failed one timing check of
  `recording_worker_foundations`) and Full 48 of 48 at 150 %, no retry. The
  owner checked the release candidate by hand; its two problems are
  corrected, and the drag test passed on the development PC with the
  correction.
- The runs, including the ones that failed, and those of earlier versions are
  in the [validation records](releases/validation-history.md#validation-of-v110).

## Not validated

- **By hand:** of the beta.2 corrections, only the black-box fix (owner,
  development PC). The still-desktop part of `island_capture_exclusion` ran in
  the lab only.
- **On the host:** a Full run, including the MP4 writer change for software
  H.264 encoders, and the Soak tier, which ran only in the lab (2026-11-07).
  Of the visible-desktop tests only `island_capture_exclusion` ran there, at
  the owner's request (2026-11-07).
  Locally, Debug was built and run only for the stitching engine and the
  diagnostics test (beta.1).
- **Other systems:** a Windows N edition, any Windows 11, any build other than
  19045. The refusal below build 19041 and the Record message on an N edition
  were not seen on such a system.
- **Scrollshot** on real applications (browsers, editors, lists that load as
  they scroll), at a scale other than the lab's 125 and 150 %, on mixed-DPI or portrait monitors,
  on a second GPU, with wheel routing by focus, and through a display change
  during capture. Such content is covered by synthetic documents only.
- **In the running application:** "Start with Windows" (ticking it writes the
  user's `Run` key; the registry code is tested against a key of its own), so
  the quiet start at sign-in was not seen either; "Same as Windows" following a
  change of the Windows setting, and high contrast (both need a change to
  Windows settings); the folder picker, "Check for updates" and "Website" (a
  system dialog or the browser); the look of the Settings dialog (excluded
  from capture, so no picture was taken; the text-fit test measures its text).
- **The installer:** not run by an agent on the development PC, since
  installing would replace the owner's copy. The owner installed beta.1 with
  the English pages and used it (reported 2026-10-30), and installed and
  removed the published beta.2 by hand, the uninstaller removing the settings
  as chosen (reported 2026-11-07). An upgrade over v1.0.2 ran silently in the
  lab only. Not seen: the removal of the sign-in entry, an upgrade over beta.2
  and a return to v1.0.2.
- **The published packages:** for beta.1 the Release workflow checked their
  SHA-256 files, attested them and created the draft, but nobody downloaded
  or ran them before publication. For beta.2 the draft's checksums and the
  installer's attestation were verified and both executables reported
  1.1.0-beta.2 before publication; the owner then installed it.
- **Package managers:** the generated winget manifests pass `winget validate`
  locally (2026-11-07); Scoop was not tried; nothing was submitted.
- A real crash (the handler is exercised by a test process that links the same
  module, not by a failing Easnap) and a real sign-out or shutdown (the two
  session messages were sent to the application by hand).
- A second reader of the Turkish text, which was read in the running
  application.

## Known limitations and unresolved risks

- Scrollshot is manual and vertical; the image only grows downwards. Copying
  and saving a very tall image run on the UI thread. At the 48-million-pixel
  limit, editing holds two buffers of about 190 MB and an export a third.
- A recording that is still being written when Easnap stops cannot be
  recovered; a recovered recording is the untrimmed MP4. A recording that
  starts on a still desktop is read through GDI for its whole length;
  Scrollshot no longer is, recording has not had the same remedy.
- With the workspace visible, a screenshot can differ from the screen by one
  level at a few values (measured: 47 as 46, 70 as 69): Windows changes them
  under the translucent, capture-excluded shading window. Scrollshot avoids
  it; screenshots do not yet.
- A window that is open when the language changes keeps its text; only the
  Settings dialog can be open then, and it closes.
- The mode island's home now starts below a taskbar docked at the top
  (2026-11-07); this was not seen with a top taskbar.
- The log gets one line per audio packet for the first packets of every
  recording; these lines are diagnostic and carry no content.
- On the host, tests share the user's clipboard: `capture_workspace_production`
  and `annotation_editing_native` replace its content.
- `mfplay_preview_audio_transport` can miss its 1500 ms limit for retiring a
  suspended native host when the machine is under load.
- `recording_worker_foundations` expects the capture thread never to wake
  after a frame's due time; it failed once in 17 lab runs, during a Soak run
  with tests in parallel (2026-11-07).
- On the hosted runner the Build workflow failed on `main` on 2026-10-30, at
  `a5293c8` in `diagnostics_log_and_crash_reports` (Debug and AddressSanitizer
  jobs) and at `42b3efa` in `mp4_trim_export_backend` (Release job, an export
  with audio); the runs are in the
  [validation records](releases/validation-history.md#validation-of-v110-beta2).
  Triage (2026-11-07): the trim export with audio failed in 2 of 10 Release
  runs since the MP4 writer change of 2026-10-19 and in none of 26 before, on
  the runner's software encoder; the exporter's reason is not visible because
  the test showed no log; it now prints it, and 50 repeated runs in the lab VM
  did not fail, so the cause is still unknown. The crash-report test failed in
  2 of 40 jobs, most likely because a report could replace an earlier one with
  the same name; report names are now unique
  ([ADR-030](decisions/ADR-030-log-file-and-crash-reports.md)). With both
  changes, the two tests passed in the Release, Debug and AddressSanitizer
  jobs of the run for pull request #17 and of the release run of v1.1.0; two
  runs do not show that the failures are gone.
- For beta.1 the CodeQL workflow and the AddressSanitizer job passed on the
  hosted runner, where `wasapi_audio_capture` is reported as skipped. In one
  AddressSanitizer run `incremental_test_selection` (5 to 7 s there) hit its
  30 s timeout and passed when the job was repeated; the cause is not known.
  The AddressSanitizer job is not a required check yet.
- The lab is one Windows 10 22H2 image with one display, a virtual audio cable
  and no GPU; `lab.ps1 parity` lists the differences from the host. A green
  verdict does not cover what they touch and is a local aid, not a release
  gate ([ADR-037](decisions/ADR-037-isolated-test-lab.md)).

Open since v1.0.0:

- Strict resize diagnostics fail (latest runs 2026-09-30): transient
  video/control overlap and duplicated caption pixels during motion and
  maximize; settled samples are clean. No cause is established. They are
  opt-in modes, not registered tests; a green CTest run does not close them.
  Figures in [ADR-024](decisions/ADR-024-native-trim-frame.md#known-limitation-transient-artifacts-while-resizing).
- Private memory of the trim window's process and of the preview helper grew
  during the slow resize run; whether in the product or in the observer is not
  known, so freedom from leaks during long resize sessions is not established
  (figures in the same section of ADR-024).
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
- The UI thread writes acknowledgements to the helper pipe synchronously; a
  helper that never reads could block it. The helper is the application's own
  process, so this needs a faulty helper.
- `CreateSession` follows a reparse-point storage root that deletion would
  refuse. A session lease that cannot be reacquired after a failed delete is
  only logged.
- Mixed-DPI drags, HDR outputs (no HDR output was available), L-shaped monitor
  layouts and the configuration island at 200 % on a 768-row display are
  unverified or known to be imperfect.
- Not every visible-desktop test follows the chosen display.
- `capture_overlay_interaction` fails three checks on the hosted runner's small
  desktop; whether that is only a test assumption or also a layout problem at
  low resolution is not established.
- CI is pinned to `windows-2022` because the project uses the Visual Studio
  17 2022 generator; moving to a newer toolchain is pending.

Files made with older versions: up to v1.0.1, an MP4 trim of a recording whose
height is not a multiple of 16 is wrong when exported on a machine without a
hardware H.264 encoder that reads Direct3D surfaces. The recording itself is
intact; export it again with v1.0.2 or later.

Camera, Mouse Effects, device pickers and OCR are not implemented. OCR remains
post-v1 without a promised date.

## Next slice

On `feature/v1.2`: the smaller slices deferred from the stabilization, then an
ADR for splitting `CaptureCoordinator` (a small state machine first, then the
trim flow with a test target of its own), then a release candidate; the order
is in the [roadmap](../EASNAP_ROADMAP.md). The
package-manager submissions for v1.1.0 remain the owner's step
([distribution](distribution.md)).
