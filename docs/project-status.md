# Project status

**Updated:** 2026-11-18

The current state of the implementation: what is built, what was actually
validated, what is open and the next slice. Read it with the
[roadmap](../EASNAP_ROADMAP.md) before implementation work. Tests are described
in [testing](testing.md), decisions in the [ADRs](README.md#decisions), each
release in its release notes (listed in the [documentation index](README.md)),
and the individual runs in the [validation records](releases/validation-history.md).
This file keeps no per-release sections.

## Baseline

- Latest stable release: [v1.3.1](releases/v1.3.1.md), published on
  2026-11-17 from `main` at `efb3d25`, the merge of pull request #25. Earlier
  releases are listed in the [documentation index](README.md).
- The Release workflow passed at the first attempt and the packages were
  verified before publication
  ([records](releases/validation-history.md#validation-of-v131)).
- Since then `main` received the structure refactor below (pull request #27).

## Structure refactor (branch `refactor/structure`)

A behaviour-preserving restructuring on the branch `refactor/structure`,
created on 2026-11-17 from `main` at `38538fa` and merged by pull request #27.
The decisions are in
[ADR-043](decisions/ADR-043-structure-for-cheap-changes.md); where to make a
given change is in the [change map](ui/change-map.md).

- **Implemented.** Layer order checked by the repository hygiene check; every
  `WM_APP` message in `platform/app_messages.h` and every control, menu,
  timer and subclass identifier in `ui/control_ids.h`; `platform_utils` split
  into one file per concern; the screenshot editor split into units owned by
  `ScreenshotEditor` (document, renderer, tool records, both islands, text
  field, shortcut table, export; 3,633 → 1,688 lines); one table row per
  button on each surface and one function per icon; shared colours and
  metrics named in `ui/ui_theme.h`, annotation colours in
  `editor/annotation_palette.h`; pure coordinator decisions for the
  Scrollshot status, display changes and the image export outcome
  ([ADR-039](decisions/ADR-039-coordinator-trim-flow.md#amendment-2026-11-18-scrollshot-measured-again));
  the tray icon, menu and About in `ui/`, the tray shell in `app/tray_shell`
  (`main.cpp` 874 → 470 lines); window class registration and owner binding
  through `platform/window_class.h`; the WASAPI capture thread, the
  dual-source mixer thread and the MFPlay event callback split into named
  steps by extraction only. Per-folder static libraries were investigated and
  not adopted.
- **Validated.** Every commit: Release build, Fast tier and format check on
  the host. Lab Standard green after four phases and lab Full green after
  four, the last at `1d188c9` after the audio and media split (69/69;
  `island_capture_exclusion` passed on retry, see below). clang-tidy 0
  warnings. New guards `ui_control_raster_parity`, `coordinator_decisions`,
  `annotation_text_rendering`. The owner used the running program on
  2026-11-18 and reported no problem. Runs and reviews:
  [ADR-043](decisions/ADR-043-structure-for-cheap-changes.md#validation).
- **Limitations and risks.** No test compiles `main.cpp` or the tray shell.
  The raster test does not hash the high-contrast palette, button captions
  or menu and combo faces. Declared small differences: the tools island's
  hit test, the monitor comparison on every display change. Still large:
  `editor/screenshot_editor.cpp` (1,688 lines), `app/session_state.cpp`
  (1,876), `ui/native_controls.cpp` (971). Notifications are not turned off
  in the lab VM's provisioning (winlab), the likely source of that retry.

## Validated

- v1.3.1, final sources: lab Full `20261117-190514-full`, 66 of 66, no retry,
  no fallback; the owner confirmed the three corrections by eye.
- The latest Soak: `20261109-171521-soak`, 74 of 74 on the v1.3.0 branch
  (`19f5ab0`), with the two known retries (`trim_timeline_seek_stress`,
  pointer on the caption; `scrollshot_realistic_desktop`, torn pictures).
- The structure refactor: see above.
- Every run, including the failures:
  [validation records](releases/validation-history.md).

## Not validated

- **By hand:** of the beta.2 corrections, only the black-box fix (owner,
  development PC). The still-desktop part of `island_capture_exclusion` ran in
  the lab only.
- **On the host:** a Full run, including the MP4 writer change for software
  H.264 encoders, and the Soak tier, which ran only in the lab. Of the
  visible-desktop tests only `island_capture_exclusion` ran there, at the
  owner's request (2026-11-07). Locally, Debug was built and run only for the
  stitching engine and the diagnostics test (beta.1).
- **Other systems:** a Windows N edition, any Windows 11, any build other than
  19045. The refusal below build 19041 and the Record message on an N edition
  were not seen on such a system.
- **Scrollshot** on real applications (browsers, editors, lists that load as
  they scroll), at a scale other than the lab's 125 and 150 %, on mixed-DPI or
  portrait monitors, on a second GPU, with wheel routing by focus, and through
  a display change during capture. Such content is covered by synthetic
  documents only.
- **In the running application:** "Start with Windows" (ticking it writes the
  user's `Run` key; the registry code is tested against a key of its own), so
  the quiet start at sign-in was not seen either; "Same as Windows" following a
  change of the Windows setting, and high contrast (both need a change to
  Windows settings); the folder picker, "Check for updates" and "Website" (a
  system dialog or the browser); the look of the Settings dialog (excluded
  from capture, so no picture was taken; the text-fit test measures its text).
- **The installer:** never installed by an agent on the development PC (it
  would replace the owner's copy). An upgrade over v1.0.2 ran silently in the
  lab only. Not seen: the removal of the sign-in entry, a downgrade.
- **The published packages:** from beta.2 on, each release's draft packages
  were checked against their SHA-256 files and attestations before
  publication (for beta.1 nobody downloaded them); the packaged installer was
  not installed before v1.3.0 or v1.3.1 were published.
- **Package managers:** the generated winget manifests pass `winget validate`
  locally (2026-11-07); Scoop was not tried; nothing was submitted.
- A real crash (the handler is exercised by a test process that links the same
  module, not by a failing Easnap) and a real sign-out or shutdown (the two
  session messages were sent to the application by hand).
- A second reader of the Turkish text, which was read in the running
  application.

## Known limitations and unresolved risks

- Scrollshot is manual and vertical; the image only grows downwards. Copying
  and saving run on a worker, but the composite of an annotated image and
  stopping a Scrollshot capture still run on the UI thread, unmeasured for a
  tall image. A copy reaches the clipboard when its worker has finished and
  replaces what was copied meanwhile. At the 48-million-pixel limit, editing
  holds two buffers of about 190 MB and an export a third and the clipboard
  memory.
- A recording that is still being written when Easnap stops cannot be
  recovered; a recovered recording is the untrimmed MP4.
- Scrollshot with fractional scroll offsets (content not snapped to device
  pixels, as some browsers do at some zoom levels) is not stitched correctly
  (59 of 154 pictures refused, 68 of 90 placements more than one row off); a
  fixed textured background and a bar that changes on every frame also fail.
  An engine change and an amendment of
  [ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md) are needed; not
  done ([ADR-041](decisions/ADR-041-scrollshot-realistic-test-application.md)).
- Desktop Duplication can hand Scrollshot a picture composed of two presents.
  The engine places it when the whole picture agrees by 55 %, so later
  placements can shift and the image comes out too tall, unseen in the
  session's progress (lab Soak `20261109-103322-soak`). The fix is an
  amendment of [ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md#known-weakness-2026-11-09),
  not made ([ADR-041](decisions/ADR-041-scrollshot-realistic-test-application.md#validation)).
- After the trim preview stops at the Out point, a quick Play, Pause and Play
  could resume at the old pause point (`mfplay_preview_stress_0`, once in 30
  lab runs; cause unknown). The helper now checks the clock against the seek
  and seeks again ([ADR-010](decisions/ADR-010-trim-view-ownership-and-orchestration.md),
  amended 2026-11-09); green runs since show only that the check does not
  misfire. It does not detect a contradiction under 500 ms, one that appears
  more than 3 s after playback resumed, or a clock left behind the target.
- A window that is open when the language changes keeps its text; only the
  Settings dialog can be open then, and it closes.
- The mode island's home now starts below a taskbar docked at the top
  (2026-11-07); this was not seen with a top taskbar.
- The log gets two lines per audio packet for the first eight packets of
  every recording, from each source when both are recorded; these lines are
  diagnostic and carry no content.
- `mfplay_preview_audio_transport` can miss its 1500 ms limit for retiring a
  suspended native host when the machine is under load.
- `recording_worker_foundations` expects the capture thread never to wake
  after a frame's due time; it failed once in 17 lab runs, during a Soak run
  with tests in parallel (2026-11-07).
- `trim_flow_production` can reach its 180 s limit under parallel load on the
  host: once on 2026-11-09 and once at `bf59714` on the refactor branch
  ([ADR-043](decisions/ADR-043-structure-for-cheap-changes.md#validation));
  it passed alone and in the lab.
- On the hosted runner `mp4_trim_export_backend` (export with audio) has
  failed occasionally since the MP4 writer change of 2026-10-19, cause
  unknown; `diagnostics_log_and_crash_reports` failed in the Debug job of the
  v1.2.0 release run (7 of 8 children wrote no report), cause unconfirmed; the
  crash line now names the failing step
  ([ADR-030](decisions/ADR-030-log-file-and-crash-reports.md)). History:
  [validation records](releases/validation-history.md#validation-of-v110-beta2).
- The AddressSanitizer job is not a required check; once,
  `incremental_test_selection` hit its 30 s timeout there.
- What a green lab verdict does not cover:
  [testing](testing.md#lab) ("What a green verdict proves").
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
  is synchronous; late MP4 trims decode from source start; codec endpoints and
  GIF timing/palette have granularity. Low size GIF can omit tiny visual
  changes.
- The writer still allocates an NV12 buffer per frame (about 0.3 ms at 1080p
  on the development PC); reusing it waits for a run with a hardware encoder.
  The recording's BGRA pictures are pooled. The tests that read the real
  desktop were not run on the host for that change
  ([ADR-042](decisions/ADR-042-recording-frame-buffer-cost.md#not-validated)).
- The repacking of a locked hardware surface was exercised only on the hosted
  runner's WARP adapter, not on a physical GPU with a software encoder.
- Editor pixel effects: moving an annotation and drawing a spotlight now
  repaint only what changed (ADR-029, amended 2026-11-09), but a change of the
  spotlight darkening still recomputes the whole image, a blur or pixelation
  next to a moved effect is recomputed whole in every paint during a drag
  (about 0.1 s for a blur 6,300 rows tall), below about 20 % every repaint
  uploads the bands it shows again, and the keyboard nudge and undo repaint
  the whole viewport.
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

Open items; the owner will set their order (the roadmap has the index):

1. The Scrollshot engine amendment for pictures composed of two presents
   ([ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md#known-weakness-2026-11-09)):
   agreement in each part of the overlap, appended rows provisional until a
   second picture agrees, a contiguous contradiction refused as `Ambiguous`.
   `scrollshot_realistic_desktop` then shows whether torn input still needs
   its `KNOWN_WEAKNESS` path.
2. The remaining editor costs: the first spotlight frame (about 50 ms in a
   tall image) and the blur closure (about 0.1 s per paint).
3. The files still large after the structure refactor:
   `editor/screenshot_editor.cpp`, `app/session_state.cpp` and
   `ui/native_controls.cpp`.
4. Turning off notifications in winlab's guest provisioning.

The Scrollshot fractional-offset finding waits for an engine change and its
own decision. Package-manager submissions remain the owner's step
([distribution](distribution.md)).
