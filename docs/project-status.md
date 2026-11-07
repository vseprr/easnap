# Project status

**Updated:** 2026-11-07

The current state of the implementation: what is built, what was actually
validated, what is open and the next slice. Read it with the
[roadmap](../EASNAP_ROADMAP.md) before implementation work. Tests are described
in [testing](testing.md), decisions in the [ADRs](README.md#decisions), each
release in its release notes (listed in the [documentation index](README.md)),
and the individual runs in the [validation records](releases/validation-history.md).

## Baseline

- Latest stable release: v1.0.2. Latest prerelease:
  [v1.1.0-beta.2](https://github.com/vseprr/easnap/releases/tag/v1.1.0-beta.2),
  published on 2026-10-30 from `main`, tag on the merge of pull request #13
  ([release notes](releases/v1.1.0-beta.2.md)).
- The Release workflow built, tested and attested its packages on the hosted
  runner; the draft was verified before publication.
- Stabilization toward v1.1.0 runs on the branch `stabilize/v1.1.0`, created
  on 2026-11-07 from `main` at `42b3efa`; it is not merged yet.

## Validated

- v1.1.0-beta.2 (2026-10-30): the Full tier passed 48 of 48 with no retry in
  the lab; the owner checked the black-box fix by hand on the development PC.
- Stabilization baseline on the beta.2 sources (2026-11-07, lab): Standard 39
  of 39 and Soak 54 of 54, no retry; `real_desktop_recording_session` passed
  on the GDI fallback (`VM-ok-but-fallback`).
- The runs, including the ones that failed, and those of earlier versions are
  in the [validation records](releases/validation-history.md#validation-of-v110-beta2).

## Not validated

- **By hand:** of the beta.2 corrections, only the black-box fix (owner,
  development PC). The still-desktop part of `island_capture_exclusion` ran in
  the lab only.
- **On the host:** a Full run, including the MP4 writer change for software
  H.264 encoders, and the Soak tier, which ran only in the lab (2026-11-07).
  Locally, Debug was built and run only for the stitching engine and the
  diagnostics test (beta.1).
- **Other systems:** a Windows N edition, any Windows 11, any build other than
  19045. The refusal below build 19041 and the Record message on an N edition
  were not seen on such a system.
- **Scrollshot** on real applications (browsers, editors, lists that load as
  they scroll), at a scale other than 125 %, on mixed-DPI or portrait monitors,
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
  as chosen (reported 2026-11-07). Not seen: the removal of the sign-in entry,
  and an upgrade over v1.0.2.
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
- A top-docked taskbar still overlaps the mode island.
- The log gets one line per audio packet for the first packets of every
  recording; these lines are diagnostic and carry no content.
- On the host, tests share the user's clipboard: `capture_workspace_production`
  and `annotation_editing_native` replace its content.
- `mfplay_preview_audio_transport` can miss its 1500 ms limit for retiring a
  suspended native host when the machine is under load.
- On the hosted runner the Build workflow failed on `main` on 2026-10-30, at
  `a5293c8` in `diagnostics_log_and_crash_reports` (Debug and AddressSanitizer
  jobs) and at `42b3efa` in `mp4_trim_export_backend` (Release job, an export
  with audio); the runs are in the
  [validation records](releases/validation-history.md#validation-of-v110-beta2).
  Triage (2026-11-07): the trim export with audio failed in 2 of 10 Release
  runs since the MP4 writer change of 2026-10-19 and in none of 26 before, on
  the runner's software encoder; the exporter's reason is not visible because
  the test shows no log. The crash-report test failed in 2 of 40 jobs: a report
  name has one-second resolution and the process ID, so a reused ID within the
  same second overwrites a report.
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

Stabilization toward v1.1.0 on `stabilize/v1.1.0`, in this order: the
documentation pass; a formatting and static-analysis pass; the code findings
of the 2026-11-07 audit (a recovery promise that can be false when marking the
recording fails, a save-failure message that promises a later offer, capture
exclusion of dialogs and drop-down lists through `ui::ExcludeFromCapture`,
stale planning comments) and the hosted-runner Build failures above. Then
v1.1.0-rc.1, with Full and Soak runs in the lab and the owner's manual checks,
and v1.1.0 after the owner's results with rc.1. The package-manager
submissions remain the owner's step ([distribution](distribution.md)).
