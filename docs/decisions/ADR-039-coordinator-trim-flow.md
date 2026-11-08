# ADR-039 — CaptureCoordinator gives trimming to a flow of its own

**Status:** Accepted; S1 implemented on `feature/v1.2`, S2 and S3 not yet
**Date:** 2026-11-08

## Context

`CaptureCoordinator` (`src/app/session_state.cpp`, 2,392 lines) owns every
capture mode. On commit `9cbbddb` it has 77 out-of-line definitions and 46 data
members. By concern:

| Group | Definitions | Lines |
|---|---:|---:|
| Shell and session lifetime (hotkey, display change, cleanup, appearance) | 19 | about 410 |
| Exit and recovery | 5 | 150 |
| Screenshot workspace and editor | 11 | about 195 |
| Recording | 13 | 301 |
| Trim and completion | 14 | 569 |
| Scrollshot | 15 | 469 |

Most state is shared. `state_`, `settings_` and `host_window_` are used by all
six groups; `editor_`, `overlay_`, `recording_session_`, `snapshot_` and
`capture_provider_` by four or five. Only 14 members belong to one group: 12 of
trim (`trim_export_generation_` to `trim_close_pending_`) and 2 of Scrollshot.

Trim's other five members are its window, its retired flag, its export
controller, its source path and its artifact. Outside trim, only the session's
lifetime code (`Shutdown`, `EndSession`, `OnDeferredCleanup`, `DialogOwner`,
the hotkey, the display change, `ProcessDialogMessage`) and
`PrepareForSessionEnd` touch them. Trim is entered from
`FinishRecordingWorkflow`; it reads the recording's options and region, and
resets the recording session when it ends.

Scrollshot, by contrast, is woven into the workspace: `EditScrollshot`
rebuilds the overlay and the editor, a display change turns an edited stitched
image back into Scrollshot review, and Scrollshot can start from the tools
island while a screenshot is being edited.

No test compiles `session_state.cpp`; only the application does. It creates
every mode's concrete objects (the recording session, the Scrollshot session,
the overlay, the editor and the trim window), so a target that compiled it
would link nearly every source of the application. Tests cover its
collaborators instead (see Validation).

A harness that drives the whole coordinator is not built:

- The coordinator is a singleton with a private constructor, so a test cannot
  start each case from a fresh one.
- `Initialize` loads the user's settings from
  `%LOCALAPPDATA%\Easnap\settings.ini` and sweeps `%LOCALAPPDATA%\Easnap\temp`.
- `BeginCaptureSession` reads the real desktop, and Desktop Duplication sees
  only the visible desktop.
- Making it testable would add parameters whose only purpose is the test.

The v1.1.0 stabilisation audit deferred the split to v1.2, with an ADR on the
target shape first. Pins ([ADR-038](ADR-038-pinned-images.md)) were added
since; the coordinator holds them but they are not session state.

## Decision

### SessionStateMachine first

`app/session_transitions` gains `SessionStateMachine`. It holds the current
state; `RequestTransition`, which logs and rejects illegal transitions as now;
`IsTransitionLegal`; and `ToString`, `IsRecordingState` and
`IsScrollshotState`, which nothing outside `session_state.cpp` uses today.

The coordinator owns one. Its public `state()` and `RequestTransition()` keep
their signatures and forward to it, so `main.cpp` does not change. Every flow
reads and changes the state through this one object; none keeps a copy.

### Then TrimFlow

`app/trim_flow` holds the trimming and completion of one finished recording.
The coordinator owns one `TrimFlow`.

- **What moves:** the functions `BeginTrimWorkflow`, `OnTrimSaveRequested`,
  `OnTrimCopyRequested`, `BeginTrimAction`, `StartTrimExport`,
  `OnTrimExportCancel`, `OnTrimWindowClosed`, `SaveUntrimmedRecording`,
  `OnTrimStatusTick`, `OnTrimExportCompleted`, `CompletePendingTrimAction`,
  `FinishTrimWorkflow` and `StopTrimForShutdown`; the members `trim_window_`,
  `trim_window_retired_`, `trim_export_`, `trim_source_path_`,
  `trim_artifact_` and the 12 that only trim uses; `PendingTrimAction` and the
  four helpers that describe export failures. `ExistingFileDataPath` and
  `RecordingSaveDialogText` are also used by recording and recovery, so they
  move to a place both can include. The dialog text stays out of
  `recording_completion.cpp`, which must not use the string tables.
- **What stays:** `OnClipboardChanged`; the completion controller and the
  temporary store, which outlive the trim session (a copied recording stays on
  the clipboard after it); the `RecordingSession`, which the coordinator
  releases when the flow asks; the recording options and region, which the flow
  copies when it begins.
- **The coordinator calls:** `Begin(source, options, region, temporary
  session)` from `FinishRecordingWorkflow`; `OnExportCompleted(generation)`
  from the public `OnTrimExportCompleted`, so `main.cpp` keeps routing
  `kMsgTrimExportCompleted`; `CancelExportAndWait()` from `Shutdown` and
  `PrepareForSessionEnd`; `RetireWindow()` from `EndSession`,
  `DestroyRetired()` from `OnDeferredCleanup` and `Destroy()` from `Shutdown`;
  and the window queries used by `DialogOwner`, the hotkey, the display change
  and `ProcessDialogMessage`.
- **The flow receives**, in a `Services` struct like the callbacks of the UI
  components: the state machine; `end_session`, `schedule_cleanup`,
  `persist_settings` and `release_recording`; the settings and a copy of the
  host window handle; the completion controller and the store, either of which
  may be null, as now. These are the coordinator's own operations, not a seam
  for tests.
- **Its own test target, `test_trim_flow`,** compiles the flow with
  `trim_workflow`, `recording_completion`, `session_transitions`, the trim
  window sources that `test_ui_modernization` already compiles and the MP4
  exporter. It writes a short MP4 with `MediaFoundationWriter` and drives the
  window with messages: Copy of the full range (no export); Copy of a part (a
  real export through Exporting and RecordingTrimming to Idle, `end_session`
  called once); cancel; close and discard during an export; `Destroy` during an
  export. Save is not covered: its file dialog cannot be driven by messages.
  Whether the preview opens on a private desktop decides the test's tier; that
  is measured first.

### Order of slices

Each slice is mechanical: bodies are moved, members renamed, behaviour kept.
Formatting goes into a separate commit, and moves are reviewed with
`git diff --color-moved`.

| Slice | Content | Gate |
|---|---|---|
| S1 | `SessionStateMachine`, tested by `session_state_transitions` | Fast; lab Full and Soak; owner checklist (S1) |
| S2 | `TrimFlow` as a move-only commit, then `test_trim_flow`, the ADR-010 amendment and the architecture text | lab Full and Soak; `test_trim_flow`; owner checklist (S2) |
| S3 | Optional: the choice of the Scrollshot status text as a pure function with a unit test | Fast; lab Full |

Whether recording becomes a flow is decided after S2.

### Not extracted in v1.2

- **Scrollshot.** It needs the overlay, the editor and the rebuild of the
  workspace, so its interface would mirror the coordinator. A test would still
  need the whole workspace, which `scrollshot_workspace_production` already
  drives.
- **The workspace and the editor.** The overlay and the editor, with the
  overlay callbacks that reach every mode, are the session's shared surface.
  Running them is the coordinator's own job.
- **Recording**, until `TrimFlow` has proven the pattern. Its finalizer worker
  holds the session by raw pointer until it is joined, and its island is
  detached from the overlay before the overlay retires.
- **Exit and recovery.** They are small and decided by the state.
- **Pins.** They are not session state and stay as ADR-038 describes.

The singleton stays: one capture session per process is a real invariant.

### What the split preserves

1. **A callback may end the session while it runs.** The trim window's Save,
   Copy and Close reach `FinishTrimWorkflow` and `EndSession` inside the
   window procedure. A flow therefore only hides its window, releases its media
   and marks it retired; it destroys the window in `DestroyRetired`, which
   `OnDeferredCleanup` calls and which waits while a modal loop or menu is open.
2. **`FinishTrimWorkflow` keeps its order:** hide, release media, then mark
   retired (closing the preview can dispatch messages, and a nested cleanup
   must not find the flag yet); the export controller before the recording
   session; then the clipboard and the temporary session; then `EndSession`.
3. **State is read again after each modal dialog,** from the state machine. An
   export can finish while a question is open and end the session.
4. **The export completion runs on the worker thread.** The generation stays
   atomic, the flow keeps its own copy of the host window handle, and `Destroy`
   cancels and waits before anything is released.
5. **`EndSession` can run twice,** because the editor's close reports back into
   it. `RetireWindow` must be idempotent.
6. **`Shutdown` and `OnDeferredCleanup` keep their order:** dependent views
   before the overlay that owns them; the trim window after the overlay; the
   export controller before the recording session; the completion controller
   before the store.
7. **`BeginTrimWorkflow` still destroys a leftover trim window at once.** That
   is safe only because it is reached from a host message or from the recording
   island's Stop, never from a trim window callback.
8. **The last video folder is still saved by the deferred cleanup at Idle;**
   two of trim's writes to it rely on that, as now.

### ADR-010, decision 1

When S2 lands, decision 1 of
[ADR-010](ADR-010-trim-view-ownership-and-orchestration.md) is amended to read:
`CaptureCoordinator` remains the sole owner of `SessionState`, through its
`SessionStateMachine`, and of the `RecordingSession`. `TrimFlow`, owned by the
coordinator, owns the finalized source path, the latest successful temporary
result, its copy of the recording options and `TrimExportController`.
`TrimWindow` owns only native controls, range interaction and preview
resources. Its callbacks are commands to `TrimFlow`, which changes the state
only through the coordinator's state machine.

## Consequences

- After S1 and S2 the coordinator has about 1,830 lines, 30 data members and
  about 65 out-of-line definitions. `OnTrimExportCompleted` stays as a
  forwarding member because `main.cpp` calls it.
- Trimming becomes the first coordinator code with a test of its own.
- S2 does not meet the rule that code is covered before it moves: the test can
  exist only after the extraction. The move-only commit and the owner checklist
  stand in for it.
- Scrollshot, the workspace and recording stay in `session_state.cpp`, which
  remains the largest file of the application.
- `git blame` shows the moved lines in `trim_flow.cpp` as new;
  `git blame -C` follows them.

## Validation

- No test compiles `session_state.cpp`. `executable_imports` only reads the
  import table of `Easnap.exe`.
- Trim's collaborators are covered by `trim_workflow_orchestration`,
  `recording_completion_workflow`, `mp4_trim_export_backend`, `gif_backend`,
  the `mfplay_preview_*` tests, and `ui_modernization_native`,
  `trim_endpoint_presentation` and the trim timeline seek tests, which drive
  the trim window with counting callbacks. The state rules are covered by
  `session_state_transitions`.
- After a slice, the lab tiers therefore show only that the collaborators and
  the build are unchanged; they do not run the moved code. Each slice also
  needs the owner's checklist in the running application, with test-created
  recordings:
  - **S1:** a screenshot, a recording with trim and a Scrollshot, each from
    start to Idle, with the same `State X -> Y` log lines as before.
  - **S2:** MP4 and GIF Save and Copy of the whole recording and of a part;
    cancelling an export; closing and discarding during an export; saving a
    recording that has no preview, untrimmed; Exit while trimming and while
    exporting; sign-out while trimming, with the recording offered at the next
    start; a display change while trimming; the shortcut bringing the trim view
    forward.
- The figures were counted on commit `9cbbddb`. S1 is implemented: the
  coordinator's state lives in a `SessionStateMachine`, and
  `session_state_transitions` checks that, from every state reachable from
  Idle, the machine allows exactly the pairs the rules allow and leaves the
  state unchanged after a refusal. The S1 owner checklist is not run yet.
