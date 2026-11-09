# Project status

**Updated:** 2026-11-09

The current state of the implementation: what is built, what was actually
validated, what is open and the next slice. Read it with the
[roadmap](../EASNAP_ROADMAP.md) before implementation work. Tests are described
in [testing](testing.md), decisions in the [ADRs](README.md#decisions), each
release in its release notes (listed in the [documentation index](README.md)),
and the individual runs in the [validation records](releases/validation-history.md).

## Baseline

- Latest stable release:
  [v1.3.0](https://github.com/vseprr/easnap/releases/tag/v1.3.0), published on
  2026-11-09 from `main`, tag on the merge of pull request #23
  ([release notes](releases/v1.3.0.md)). The stable releases before it were
  [v1.2.0](releases/v1.2.0.md) and [v1.1.0](releases/v1.1.0.md), both
  published on 2026-11-08.
- The Release workflow built, tested and attested the v1.3.0 packages on the
  hosted runner at the first attempt (run `42651302863`). Before publication
  the draft's three packages matched their SHA-256 files, the installer's and
  the ZIP's attestations verified, and both executables and the installer
  reported 1.3.0.
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

## v1.3.0

Work toward v1.3 ran on the branch `feature/v1.3` (S2 of the coordinator
split, merged by pull request #21) and then on `feature/v1.3-findings` (the
2026-11-07 audit findings, the external video feature and the preview clock
check, merged by pull request #22 on 2026-11-09). No release candidate was
packaged: the owner checked the branch's build by hand on 2026-11-09 and asked
for a stable release. v1.3.0 was published on 2026-11-09 from `main` at
`2284b19`. Its notes are [v1.3.0](releases/v1.3.0.md).

Work toward v1.3 runs on the branch `feature/v1.3`, created on 2026-11-08 from
`main` at `68e34c5`.

- **The trim flow is a class of its own** (S2 of
  [ADR-039](decisions/ADR-039-coordinator-trim-flow.md)): `TrimFlow`
  (`app/trim_flow`) holds the trim window, the export controller, the source
  path and the latest temporary result of one recording; `CaptureCoordinator`
  owns it and keeps the recording session, which it releases when the flow
  asks. The functions were moved, not rewritten; ADR-039 lists the three body
  changes. The coordinator went from 2,332 to 1,669 lines. Decision 1 of
  [ADR-010](decisions/ADR-010-trim-view-ownership-and-orchestration.md) is
  amended accordingly.
- **`trim_flow_production`** (Standard tier, private desktop) drives the
  production flow, window, MP4 exporter, completion controller and temporary
  sessions with messages: Copy of the whole recording and of a part, cancelling
  an export, keeping or discarding when closing, discarding during an export
  and destroying the flow during an export. The clipboard publication is
  recorded instead of performed, so the test leaves the clipboard alone.
- Validated: lab Full on the uncommitted branch (2026-11-08, run
  `20261108-201007-full`), 51 of 51 with no retry, including the new test;
  `real_desktop_recording_session` passed on the GDI fallback as before
  (`VM-ok-but-fallback`). On the development PC: the Release build without
  warnings, the Fast tier 23 of 23, clang-tidy without findings on the changed
  sources, and six runs of the new test, 132 of 132 checks each, leaving
  nothing in the temporary folder. Lab Soak on the same tree (run
  `20261108-202202-soak`): 57 of 57, the new test included; one test,
  `trim_timeline_seek_stress`, passed only on the runner's retry. Its first
  attempt failed the pointer check: the pointer landed on the window caption
  instead of the timeline. That test does not compile any source S2 changed.
- By hand: the owner ran the S2 checklist of ADR-039 in the running
  application on the development PC (2026-11-08) without a problem: MP4 and
  GIF Save and Copy of the whole recording and of a part, cancelling an
  export, discarding during an export, saving a recording without a preview,
  Exit while trimming and while exporting, sign-out while trimming with the
  recording offered at the next start, a display change while trimming and the
  shortcut bringing the trim view forward. No automated test runs Save (its
  file dialog), GIF, Exit, sign-out or a display change while trimming.
- **A screenshot is read with the shading split.** With the workspace on
  screen, `PrepareWorkspaceScreenshot` splits the shading into four windows
  around the frame for its one picture, as Scrollshot does
  (`OverlayController::ExposeFrame`, renamed from `ExposeScrollshotFrame`), so
  no value comes back one level off. `scrollshot_real_desktop` now measures
  such a read first. On the development PC (SAMSUNG display, 2026-11-08):
  1,572 pixels of the frame one level off under the single shading, 0 with the
  shading split. The lab VM shows no shift either way, so there it only shows
  that nothing else changed: lab Full (run `20261108-234453-full`), 51 of 51
  with no retry, `scrollshot_real_desktop` on Desktop Duplication as before.
- **Copying and saving an image no longer freeze the window**
  ([ADR-031](decisions/ADR-031-unsaved-work-survives.md#copying-and-saving-a-finished-image-amended-2026-11-09)):
  the editor, pins and the Scrollshot island encode and write on a worker
  (`platform/image_export`), one export per process; the window stays busy
  and refuses changes until the result arrives. Exit is refused and a sign-out
  held back while one runs; at session end a save is waited for and a copy
  cancelled. Validated on the development PC (2026-11-09): the Release build
  without warnings, the Fast tier, and the Standard tests
  `capture_workspace_production`, `capture_export_responsiveness`,
  `scrollshot_workspace_production`, `pinned_images_native`,
  `data_safety_questions`, `text_fits_in_every_language` on private desktops.
  The responsiveness test copied 1920 x 12,000 pixels of noise in 3.1 to 3.9 s
  with the slowest reply of the window at 31 to 47 ms. Not run: the lab, the
  desktop tiers, Exit and sign-out during an export, Scrollshot Copy and Save
  (no test compiles the coordinator) and the Save dialog paths.
- **A video file opens in the trim view**
  ([ADR-040](decisions/ADR-040-external-video-in-the-trim-view.md)): the
  media layer (one support table, the probe, both exporters widened) and the
  flow. The tray has **Open video...**: a file dialog, the probe on a worker
  thread (`app/external_video`), then the trim view or one sentence that says
  why the file cannot open. `TrimFlow::Begin` takes a `TrimSource`; for an
  opened video the flow works in a session of its own that is never marked or
  kept, never moves, overwrites or secures the file (asserted by
  `trim_flow_production`), refuses to save over it by file identity, asks MP4
  or GIF per action and closes without a question unless it exports. A
  file inside Easnap's own data folder is refused before the probe: Easnap
  deletes what is there. A recording's flow is unchanged. `Idle -> RecordingTrimming` is a new edge and
  `PendingWorkFor` takes the source kind. Validated on the development PC
  (2026-11-09, private desktops): the Release build without warnings, the Fast
  tier 27 of 27, the Standard tests listed in ADR-040's validation, clang-tidy
  without findings on the changed sources. Not validated: the lab, the tray
  entry and its dialogs in the running application, Exit and sign-out during
  an opened video's export (no test compiles the coordinator).
- **A video file opens from Explorer and the command line** (ADR-040, entry
  points): real argument parsing (`app/command_line`) replaces the
  `--startup` substring check, so a path containing `--startup` opens the
  file instead of starting quietly; `--open <file>` and a bare existing file
  open it. A second `Easnap.exe` with a file hands the path to the running one
  with `WM_COPYDATA` (`app/video_handover`); the receiver checks it, keeps
  one request, posts itself and opens it only while idle with no dialog or
  menu open, and otherwise drops it with a log line. The second start says
  why nothing opened (the probe's refusal for a path the receiver refuses, or
  the new "Easnap is busy" message). The installer offers Easnap under "Open
  with" for the table's extensions (HKCU only, never the default, removed at
  uninstall). Validated on the development PC (2026-11-09): see ADR-040's
  validation. Not validated: the installer itself (not compiled or run),
  Explorer's "Open with", a real second `Easnap.exe`, and the lab.
- **Recording stays in the coordinator**, decided after S2: its island
  belongs to the workspace while the region is configured, and what can fail
  is already in classes with tests
  ([ADR-039](decisions/ADR-039-coordinator-trim-flow.md#recording-decided-after-s2)).
- **Settings that cannot be saved are told**
  ([ADR-033](decisions/ADR-033-settings-shortcut-and-launch-behaviour.md#settings-that-cannot-be-saved)):
  once per run, in a notification, never during a capture or while a dialog
  or menu of Easnap is open. OK in Settings says it in its own message, and
  its message about "Start with Windows" no longer says the other settings
  were saved when they were not. The log line names the Win32 error. With it,
  small clean-ups: a file where a folder should be no longer counts as that
  folder (`EnsureDirectory`; "Open data folder" then opens nothing);
  `CaptureCoordinator::Initialize` cannot fail and the dead check after it is
  gone; a failed `GetMessage` ends through `WM_DESTROY`; failures of the DLL
  search restriction and of the tray icon's version, tooltip and notification
  calls are logged; the unused `EASNAP_VERSION_STRING` definition is removed.
- Validated on the development PC (2026-11-09): the Release build without
  warnings; the Fast tier 23 of 23, with new checks in
  `session_state_transitions` (a read-only file and a file where the folder
  should be fail and keep the saved values; when the notice is due) and
  `storage_cleanup_containment` (`EnsureDirectory`); `strings_catalog`,
  `text_fits_in_every_language`, `capture_workspace_production` and
  `trim_flow_production`. In the first run of those four in parallel,
  `trim_flow_production` reached its 180 s limit while cancelling an export;
  it passed alone and in a second parallel run. clang-tidy without findings
  on the changed sources. Not validated: the notification itself, which no
  test can see, and the flow with an unwritable `settings.ini` in the running
  application; no lab run.
- **A move in the editor repaints what it changed**
  ([ADR-029](decisions/ADR-029-scrolled-and-zoomed-image-editing.md#a-move-repaints-what-it-changed-amended-2026-11-09)):
  dragging an annotation and a spotlight preview invalidate the annotation's
  old and new extent; the paint that recomputes the pixel effects invalidates
  whatever else they changed, so a second paint shows it, and effects are
  never recomputed during input. A watermark draws only the tiles the target
  shows. In a 1,920 × 8,400 image on the development PC (125 %), dragging a
  rectangle went from about 3 ms and the whole viewport per move to about
  0.5 ms and 4 % of it, with a watermark from 7.5–13.5 ms to 0.5–0.9 ms. A
  cancelled spotlight preview used to leave the rest of the picture darkened
  on screen, and a pixelating brush drawn up or left showed old blocks until
  released; both are now repainted. `scrollshot_edit_performance` (Standard)
  measures the gestures at 98.4 %, 67 % and 10 % and compares the partly
  repainted picture with a full repaint in the middle of each gesture: equal
  at 10 % except for watermark glyphs; at the other zooms Direct2D's own
  drawing variance (up to 7 levels) is allowed and nothing stale is found.
  Exports have the same fingerprints before and after. Validated on the
  development PC (Release build without warnings, the Fast tier, and the
  Standard tests `scrollshot_edit_performance`,
  `scrollshot_workspace_production`, `capture_workspace_production` and
  `capture_frame_interaction` on a private desktop) and in the lab runs
  below, Full and Soak, the 25,000-row soak variant included (in the VM:
  rectangle drag 0.69 ms, first spotlight frame 72 ms, blur closure 136 ms
  per paint). The first CI run of the branch found two assumptions of the
  test, not of the product: on the hosted runner's 1024-pixel display the
  image fits at 52 %, below the 67 % step the test expected to reach by
  zooming out, so the test now climbs the zoom ladder in either direction;
  and in the Debug build the blur closure costs about 1.8 s per paint, so the
  Debug and sanitizer CI jobs leave the test out (it measures in Release,
  55 s on the runner).
- **A recording asks Desktop Duplication until its first composed frame**
  ([ADR-004](decisions/ADR-004-per-output-capture-fallback.md),
  [ADR-028](decisions/ADR-028-scrollshot-capture-workspace.md)): a recording
  that starts on a still desktop no longer stays on GDI for its whole length.
  The lab's earlier `VM-ok-but-fallback` mark came from the probe, which ran
  before COM was set up; in the lab runs below every capture test reports
  Desktop Duplication (`trim_endpoint_presentation` reports Windows Graphics
  Capture).
- **A crash report says why it failed and is retried**
  ([ADR-030](decisions/ADR-030-log-file-and-crash-reports.md)): the handler
  records the stage and the error code, retries the dump (then a smaller one)
  and has a second channel through `OutputDebugString`; the crash-report test
  prints each child's crash line and adds a child with a removed folder. Debug
  passed 10 of 10 on the development PC. The cause of the missing reports on
  the hosted runner is still unconfirmed.
- **The trim window's minimum width is 512 DIP**
  ([ADR-023](decisions/ADR-023-trim-resize-and-text-click-continuations.md)):
  at 480 DIP the Turkish "cancel export" caption needed 186 px of a 184 px
  cell at 125 %. The trim window is now in `text_fits_in_every_language`
  (three states, at the old and the new size). The change also broke an
  assumption of `ui_modernization_native` (a notice of 24 lines no longer
  wraps at 512 DIP), which was corrected in the test.
- **Behaviour-neutral clean-ups:** the UI test source is renamed
  `test_native_ui.cpp` (CTest names unchanged); the long editor and audio
  functions are split into smaller ones, with the audio gate and overlap rules
  as pure functions in `audio_timeline`.
- **The Scrollshot engine is tested with a generated realistic application**
  ([ADR-041](decisions/ADR-041-scrollshot-realistic-test-application.md)):
  `scrollshot_realistic_stitch` (Standard) and `scrollshot_realistic_desktop`
  (Full, lab only). Text, pictures, eased scrolling, late content, a growing
  list, a fixed bar, caret and spinner are stitched exactly. In the lab (run
  `20261109-051443-full`, Desktop Duplication): 124 pictures, 101 submitted,
  60 of them mid-animation, 0 refused, 0 misplaced, height 4,539 rows as
  expected, 0 differing rows, the shadow engine equal to the session, the
  observer 4.33 ms mean and 14.52 ms worst, 9.0 s. Finding: with fractional
  scroll offsets 59 of 154 pictures were refused and 68 of 90 placements were
  more than one row off (worst 3.07); two further expected failures (a
  textured fixed background, a bar that changes every frame) are confirmed.
  Fixing them needs an engine change and an amendment of ADR-027; not done.
  Torn pictures in the lab: in the Soak run `20261109-103322-soak` the
  desktop test showed pictures composed of two presents (picture 55 between
  offsets 1,868 and 2,068; on the retry picture 62 between 1,859 and 1,868,
  followed by placements 66 rows too low and a height of 4,605 against
  4,539), collected as PNG. The engine accepts such a picture on the
  agreement of the whole picture: a known weakness, recorded in
  [ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md#known-weakness-2026-11-09),
  whose fix is the next Scrollshot slice. The desktop test therefore keeps
  every exactness check on clean input, and when every picture with no match
  is torn it prints `KNOWN_WEAKNESS` with the figures and asserts only the
  exact bar, the shadow engine equal to the session, the session following
  to the end and at most three GDI pictures; a picture with no match that is
  not torn keeps the whole contract. The conditional contract has not run in
  the lab yet.
- **Recorded frames reuse their pictures**
  ([ADR-042](decisions/ADR-042-recording-frame-buffer-cost.md), Accepted):
  measured by `frame_buffer_cost` and `recording_frame_cost`; the lab Soak
  run before the change gave about 2,160 page faults per frame and a 6.7 ms
  region copy at 1080p30 on Desktop Duplication, with 15 of 286 pacing slots
  missed. The recording worker now reads each frame into a picture from its
  own pool (at most seven kept, returned by the `shared_ptr` deleter, safe
  after Stop) through `CaptureRegionImage` with the same options as before,
  and a readback that overwrites every pixel copies it once, made opaque,
  without a fill (`ResizeForOverwrite`, `CopyOpaqueFrom`). On the development
  PC a 1080p readback went from about 1.7 ms (new picture, fill, copy,
  `MakeOpaque`) to 0.15 ms. Validated in the lab Soak run
  `20261109-103322-soak`: page faults per frame 862 (before about 2,160),
  two pictures allocated per recording (before one per frame), the region
  copy 5.7 to 6.1 ms (medians; before 6.7 ms on average), process CPU time
  106 to 708 µs per frame (before 667 to 1,311), 6 to 14 pacing slots of
  about 290 missed (before 14 to 20), the writer unchanged at 5.8 to 6.0 ms;
  in the VM a copy into a reused picture takes 156 µs against 1,854 µs into
  a new one. The NV12 buffer is still new per frame: tracked samples are
  deferred until a hardware-encoder run on a physical GPU.
- **An external video in the trim view: the media layer**
  ([ADR-040](decisions/ADR-040-external-video-in-the-trim-view.md), Proposed):
  the support table, the probe with its classifier and the exporters, which
  accept every table entry, are implemented and tested with files the writer
  made, including a WMV. The flow, the window and the entry points followed
  (above).
- **Documentation tidy-up:** the ADR headers follow one style and the
  `test.ps1` parameters are in a table in [testing](testing.md).
- **The trim preview checks its clock against the seek**
  ([ADR-010](decisions/ADR-010-trim-view-ownership-and-orchestration.md),
  amended 2026-11-09), for the race under Known limitations. After a seek,
  `PreviewTransport` records when the command that resumed playback was
  dispatched; for 3 s after it, a native clock further ahead of the target
  than the time since then plus 500 ms is not shown as the position, and the
  helper's monitor seeks the target again, twice at most, before it asks the
  parent for a new helper. The 3 s window keeps the audio device's clock
  drift over long playback out of the check. The helper trace marks such a
  clock (`clock-contradicted=1`), and the `mfplay_preview_stress_*` tests
  print any such helper line that reaches them (best effort: a snapshot
  carries only the last 2,048 trace characters). Validated: the Fast tier on
  the development PC (30 of 30), where `trim_workflow_orchestration` replays
  the transport side of the lab trace and fails 38 assertions with the check
  disabled. In the lab with the check: Full (run `20261109-151309-full`) 66
  of 66 with no retry, Soak (run `20261109-171521-soak`) 74 of 74, every
  `mfplay_preview_*` and `trim_*` test passed, no helper was replaced and no
  Play was lost (`helper_changed=0`, `lost_play=0` in every seek stress
  line). That shows only that the check does not misfire. MFPlay cannot be
  made to ignore a seek on demand, so the re-seek and recovery are validated
  by the Fast tests alone until the race recurs; with one failure in 30 runs,
  about 90 clean runs would be needed before claiming the race is gone, and
  no such claim is made. The two Soak retries were the known ones: the
  pointer landing on the trim window's caption in `trim_timeline_seek_stress`
  and torn pictures in `scrollshot_realistic_desktop` (five torn and one
  with six differing rows, so the test did not relax).

## v1.2.0

Work toward v1.2 ran on the branch `feature/v1.2`, created on 2026-11-08 from
`main` at `df838d4` and merged by pull request #19. The owner chose on
2026-11-08 to release v1.2 with the slices below and to leave the trim flow
(S2 of ADR-039) and the larger candidates to v1.3. The release candidate
v1.2.0-rc.1 was packaged on the development PC, installed over v1.1.0 by the
owner and used for a second round without a problem; it is not published.
v1.2.0 is its code with an empty prerelease label, published on 2026-11-08.
Its notes are [v1.2.0](releases/v1.2.0.md).

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
  promises worked, then used rc.1 for a second round (several pins and Close
  all among them) without a problem.
- Not validated: a pin moved between monitors of different scale (the PC has
  one display), long use, and many or very large pins at once. The native test
  reads the pin's picture through `WM_PRINTCLIENT`, not from the screen.
- **Audio capture threads have an exception boundary:** the WASAPI capture
  thread, its consumer and the mixer of both sources catch what escapes them
  (in practice running out of memory or threads). The recording's audio then
  fails through its usual path, and Start and a drain are released, instead of
  Easnap ending. Validated: lab Full (2026-11-08, run `20261108-151811-full`),
  50 of 50 with no retry, including `wasapi_audio_capture` and the recording
  tests. The failure path itself is not exercised: no test runs out of memory.
- **Undo keeps 200 steps** ([ADR-017](decisions/ADR-017-neumorphic-editor-and-editable-annotations.md));
  older steps are dropped. Covered in `capture_workspace_production`; same lab
  run.
- **Copy prepares both clipboard formats first:** the DIB and the encoded PNG
  are ready before the clipboard is opened, which is now held only to hand
  them over. Covered in `capture_workspace_production` and
  `scrollshot_workspace_production` (host runs).
- **The coordinator's state is held by `SessionStateMachine`**, the first
  slice of [ADR-039](decisions/ADR-039-coordinator-trim-flow.md); the log
  lines of transitions are unchanged. `session_state_transitions` checks that
  the machine allows exactly the pairs the rules allow, from every state. In
  the owner's round with rc.1, a screenshot, a recording with trim and a
  Scrollshot ran from start to Idle as before. The trim flow (S2) is next, in
  v1.3.
- The clipboard change and the state machine, with the shorter WASAPI start-up
  code, passed lab Full 50 of 50 (run `20261108-153432-full`, without the
  WASAPI change) and Soak 56 of 56 (run `20261108-154503-soak`, all three) on
  2026-11-08, with no retry and the usual GDI fallback of
  `real_desktop_recording_session`.

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
- v1.2.0, final sources (2026-11-08, lab, tree `1667af96576c`): Soak 56 of 56
  at 125 % and Full 50 of 50 at 150 % on the second run, no retry. In the
  first run at 150 % `real_desktop_recording_session` passed only on the
  runner's retry: one frame-count check on the GDI fallback at 30 fps. The
  owner used the release candidate for a second round without a problem.
- v1.3 on `feature/v1.3-findings` (2026-11-09, lab): Standard 42 of 42 on
  the starting tree `eedf148` (run `20261109-020041-standard`). Full of the
  first batch failed 50 of 51 (`20261109-030303-full`): `ui_modernization_native`
  assumed a notice would wrap after the trim window's 512 DIP minimum; the
  product was right, the test was corrected in `4862bc7`. Full 51 of 51 at
  `4862bc7` (`20261109-044659-full`) and 56 of 56 at `dd78926`
  (`20261109-051443-full`, which adds `scrollshot_realistic_desktop`,
  `scrollshot_realistic_stitch`, `capture_export_responsiveness`,
  `image_export_job` and `frame_buffer_cost`), both with no retry and no
  fallback: every capture test on Desktop Duplication, `trim_endpoint_presentation`
  on Windows Graphics Capture.
- v1.3 on `feature/v1.3-findings`, later runs (2026-11-09, lab): Soak 67 of
  68 at `b9b3f77` (`20261109-054120-soak`) failed only
  `scrollshot_realistic_desktop`, which led to the fixes of its oracle
  ([ADR-041](decisions/ADR-041-scrollshot-realistic-test-application.md#validation)).
  Full 62 of 62 at `e048f3e` (`20261109-095512-full`), no retry, includes the
  media layer of ADR-040 and the editor's damage rectangles;
  `scrollshot_real_desktop` read through GDI that time (`VM-ok-but-fallback`).
  Full 64 of 66 at `c0ae38a` (`20261109-101913-full`): `mfplay_preview_stress_0`
  failed once on the preview-transport race (Known limitations), and
  `scrollshot_realistic_desktop` failed before its oracle fix. Soak 73 of 74
  at `bd10347` (`20261109-103322-soak`): only `scrollshot_realistic_desktop`
  failed, on pictures torn between two presents, with the evidence collected;
  `mfplay_preview_stress_0`, `_1` and `_2` passed, and `recording_frame_cost`
  measured the picture pool (ADR-042).
- The runs, including the ones that failed, and those of earlier versions are
  in the [validation records](releases/validation-history.md#validation-of-v120).

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
- **v1.3:** the owner ran the checklist of this branch in the running
  application on the development PC (2026-11-09: the tall Scrollshot copy and
  save, tall image editing, the settings notice with an unwritable
  `settings.ini`, the trim window at its minimum width, a recording started
  on a still desktop, and the external video feature: tray entry, "Open
  with" while Easnap runs and while it does not, the reason dialog, Close and
  Exit during an export, Save and Copy as MP4 and GIF, the source untouched)
  and found no problem. The conditional contract of
  `scrollshot_realistic_desktop` ran in the lab (Full `20261109-151309-full`
  passed; Soak `20261109-171521-soak` first attempt failed on five torn
  pictures and one with six differing rows, retry passed).

## Known limitations and unresolved risks

- Scrollshot is manual and vertical; the image only grows downwards. Copying
  and saving run on a worker, but the composite of an annotated image and
  stopping a Scrollshot capture still run on the UI thread, unmeasured for a
  tall image. A copy reaches the clipboard when its worker has finished and
  replaces what was copied meanwhile. At the 48-million-pixel limit, editing
  holds two buffers of about 190 MB and an export a third and the clipboard
  memory.
- A recording that is still being written when Easnap stops cannot be
  recovered; a recovered recording is the untrimmed MP4. A recording that
  starts on a still desktop reads its first picture through GDI and asks
  Desktop Duplication again from the next frame, as Scrollshot does
  (2026-11-09; the lab tests `real_desktop_recording_session` and
  `recording_pause_stop_stress` passed on Desktop Duplication in run
  `20261109-044659-full`). The earlier `VM-ok-but-fallback` mark of
  `real_desktop_recording_session` came from the test's own probe, which ran
  before COM was started and so always read GDI; the recordings of run
  `20261108-234453-full` asked Desktop Duplication on every frame. The probe
  now reads as a recording does.
- Scrollshot with fractional scroll offsets (content not snapped to device
  pixels, as some browsers do at some zoom levels) is not stitched correctly:
  in `scrollshot_realistic_stitch` 59 of 154 pictures were refused and 68 of
  90 placements were more than one row off (worst 3.07 rows), so the image
  came out three rows short. A fixed textured background and a bar that
  changes on every frame also fail, the first silently. These need an engine
  change and an amendment of
  [ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md); not in v1.3 so
  far ([ADR-041](decisions/ADR-041-scrollshot-realistic-test-application.md)).
- Desktop Duplication can hand Scrollshot a picture composed of two presents
  (rows from two scroll positions). The engine places it when the whole
  picture agrees with the canvas by 55 %, writes the rows that disagree, and
  later placements can shift by a constant distance, so the image comes out
  too tall; nothing in the session's progress shows it. Shown in the lab
  Soak run `20261109-103322-soak`: pictures torn between offsets 1,868 and
  2,068 (no placement shifted, 278 rows unlike the page) and between 1,859
  and 1,868 (later placements 66 rows too low, 4,605 rows against 4,539).
  The fix is an amendment of
  [ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md#known-weakness-2026-11-09)
  (agreement in each part of the overlap, appended rows provisional until a
  second picture agrees, a contiguous contradiction refused as `Ambiguous`),
  not made ([ADR-041](decisions/ADR-041-scrollshot-realistic-test-application.md#validation)).
- After the trim preview stops at the Out point, a quick Play, Pause and Play
  could resume at that old pause point instead of the In point and stop again
  at once (`mfplay_preview_stress_0`, run `20261109-101913-full`, once in 30
  lab runs). MFPlay reported its clock at In while paused, then reported
  playing from its clock before the seek; why is unknown, because the helper
  sends only the last 2,048 characters of its trace. The helper now checks
  the clock against the seek and seeks again (v1.3 above); the lab Full and
  Soak runs with it were green, which shows only that the check does not
  misfire. It does not detect a contradiction under 500 ms, one that
  appears more than 3 s after playback resumed, or a clock left behind the
  target.
- A window that is open when the language changes keeps its text; only the
  Settings dialog can be open then, and it closes.
- The mode island's home now starts below a taskbar docked at the top
  (2026-11-07); this was not seen with a top taskbar.
- The log gets two lines per audio packet for the first eight packets of
  every recording, from each source when both are recorded; these lines are
  diagnostic and carry no content.
- On the host, `annotation_editing_native` shares the user's clipboard and
  replaces its content.
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
  jobs of the run for pull request #17 and of the release run of v1.1.0, and
  in the run for pull request #19. In the first attempt of the release run of
  v1.2.0 the crash-report test failed again in the Debug job: all 8 failing
  children ended with the expected code, so the handler ran, but 7 wrote no
  report. Unique names did not prevent it; the job passed when it was run
  again. The cause is still unconfirmed, but the next occurrence will name the
  step that failed (writer load, create or write) and its error code: the
  crash line now records them, a failed dump is tried again, and the test
  prints every child's line (2026-11-09).
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
  is synchronous; late MP4 trims decode from source
  start; codec endpoints and GIF timing/palette have granularity. Low size GIF
  can omit tiny visual changes.

Open since the v1.0.1 review:

- The writer still allocates an NV12 buffer per frame (about 0.3 ms at 1080p
  on the development PC); reusing it waits for a run with a hardware encoder.
  The recording's BGRA pictures are pooled; `recording_frame_cost` has not
  run in the lab with the pool yet, and the tests that read the real desktop
  were not run on the host for that change
  ([ADR-042](decisions/ADR-042-recording-frame-buffer-cost.md)).
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

On `feature/v1.3-findings`, in this order (the roadmap has the index):

1. The Scrollshot engine amendment for pictures composed of two presents
   ([ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md#known-weakness-2026-11-09)):
   agreement in each part of the overlap, appended rows provisional until a
   second picture agrees, a contiguous contradiction refused as `Ambiguous`.
   `scrollshot_realistic_desktop` then shows whether torn input still needs
   its `KNOWN_WEAKNESS` path.
2. The remaining editor costs: the first spotlight frame (about 50 ms in a
   tall image) and the blur closure (about 0.1 s per paint).

The Scrollshot fractional-offset finding waits for an engine change and its
own decision. Package-manager submissions remain the owner's step
([distribution](distribution.md)).
