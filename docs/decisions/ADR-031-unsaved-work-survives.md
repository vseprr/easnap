# ADR-031 — Unsaved work survives exit, sign-out and crashes

**Status:** Accepted
**Date:** 2026-10-13
**Amended:** 2026-11-09 — copying and saving a finished image run on a worker;
what Exit and a sign-out do while one runs

## Context

Before this decision Easnap lost work in ways the user could not see coming.

- Tray Exit destroyed everything at once: a running recording was finalized
  and then deleted, a finished recording in the trim window was deleted, and
  an annotated screenshot was dropped. Nothing asked.
- A sign-out or shutdown did the same, and Windows was never told there was a
  reason to wait.
- A finished recording that survived a crash was deleted at the next start,
  although the installer notes and the portable readme said recoverable
  recordings are never deleted silently.
- Esc closed an annotated screenshot without a question.
- Exit could arrive while a Save dialog was open. The dialog's caller was
  still on the stack when the window it belonged to was destroyed.
- Three messages said that temporary media "was preserved" at a path that
  Easnap deleted when it exited.

## Decision

### What would be lost

`PendingWorkFor(state, image_at_risk)` in `session_transitions` is the one
table: a running, paused or finalizing recording; a finished recording that is
open and unsaved; an export in progress; a held image (an annotated
screenshot, a Scrollshot being captured or reviewed, a stitched image being
edited). A plain screenshot without annotations can be taken again and does
not count.

### Exit and close

Tray Exit and `WM_CLOSE` call `CaptureCoordinator::ConfirmExit`.

- While one of the application's dialogs or menus runs its own message loop,
  the request is refused with a sound. `platform::ModalLoopScope` marks the
  message dialog and both file dialogs; menus already had `PopupScope`.
- When work would be lost, a question names it. Its buttons are named after
  what they do, "Exit anyway" and "Stay", and "Stay" is the default.
- For the same reason retired windows are not destroyed while such a loop is
  running; the deferred cleanup tries again a moment later.

### Sign-out and shutdown

`WM_QUERYENDSESSION` answers no while work would be lost and gives Windows the
reason to show (`ShutdownBlockReasonCreate`). If the user ends the session
anyway, `WM_ENDSESSION` runs `PrepareForSessionEnd`: a running recording is
finalized, every finished recording is kept for the next start, the settings
are written. No window is destroyed and the application stays usable should
the session not end after all.

### Finished recordings are kept until the user decides

When a recording has been finalized and the trim view is about to open, its
session directory is marked recoverable. From then on only a decision of the
user removes it: Save, Copy, Discard, or "Exit anyway".

- The stale-session sweep at start skips a marked directory whose recording
  exists. Unmarked leftovers are swept as before.
- `AdoptRecoverableSessions` takes such directories over. The start offers
  each one: **Save...**, **Delete** or **Not now**. Save copies the working
  MP4 to a place the user chooses; "Not now" keeps it for the following start.
- A directory without a lease file is one that is being created, or what a
  deletion left behind. After an hour it can only be the latter and is
  removed; before, it could never be collected.

The recovered file is the untrimmed H.264 working recording, also when GIF had
been chosen.

### Discarding asks when there is something to lose

Every way out of the editor (Esc, the Discard button, a close request) goes
through `RequestDiscard`. It asks when the screenshot has annotations or the
image is a stitched one. The Scrollshot island asks when the capture has grown
beyond its first picture. The keeping answer is the default. Nothing is asked
for a screenshot nobody has drawn on.

### Copying and saving a finished image (amended 2026-11-09)

Encoding a very tall image takes seconds. On the UI thread it froze the
window that asked for it, and Windows marked Easnap "Not responding". The
editor, a pin and the Scrollshot island now copy and save through
`platform::ImageExportJob` (`platform/image_export`):

- The composite and the destination stay on the UI thread: the editor draws
  the annotations into a new image, and quick save or the Save dialog chooses
  the file (`ui::ChooseImageDestination`). The worker then writes the file in
  bands of rows, or prepares the clipboard memory (CF_DIB and the optional
  PNG), with a COM apartment and a WIC factory of its own: the shared graphics
  environment belongs to the UI thread. The window that asked is told with
  `kMsgImageExportCompleted` (`WM_APP + 15`, the job's generation in WPARAM);
  it puts the memory on the clipboard in one attempt, reports Copied or Saved
  and closes. A failure keeps the image with the notices as before; a quick
  save that fails opens the Save dialog.
- One export runs in the process: two saves started in the same second would
  choose the same file name, and two exports of the largest stitched image
  hold about a gigabyte. A second Copy or Save is refused: with a sound in
  the editor and a pin, silently on the Scrollshot island, whose buttons are
  off while it is busy.
- While its image is exported, the editor refuses every change and every way
  out (Esc, Discard, Pin, a close request) with a sound, turns Copy, Save and
  Pin off and shows the busy cursor; "Copying..." or "Saving..." appears when
  the work lasts longer than 300 ms. A pin is busy as with its menu: a close
  asked for meanwhile happens when the export has finished. The Scrollshot
  island stays busy through the dialog and the write.
- A cancel of the workspace (Esc on the mode island, a right-click) during an
  export of the session is refused with a sound. A display change waits for
  the export and is looked at again when it ended without closing the
  session.
- **Exit** is refused with a sound while an export runs, as while a dialog is
  open. **Sign-out and shutdown:** a running export counts as an image at risk
  (`PendingWorkFor` has a third input), so `WM_QUERYENDSESSION` answers no with
  the image reason unless a recording matters more. If the session ends anyway,
  `PrepareForSessionEnd` waits for a file being written, so it lands complete
  and no staged `.easnap-*.tmp` is left, and cancels a copy, as nothing will
  read the clipboard. Nothing is published there and no window is destroyed;
  the results arrive as messages should the session go on. Ending a session or
  destroying a window during an export (only Shutdown does) cancels and waits.
- A copy is put on the clipboard when its worker has finished. Anything copied
  to the clipboard in the meantime, by Easnap's text fields or another
  application, is replaced by it.
- What stays on the UI thread: the composite of an annotated image and
  stopping a Scrollshot capture (`StopScrollshotCapture` waits for the stitch
  worker). The composite of a stitched image without annotations took 12 to
  25 ms for 1920 x 12,000 pixels on the development PC; an annotated one and
  the stop have not been measured.

### Messages say what is true

- A recording that fails to start names the part that failed and the next
  step: audio source, encoder and region size, temporary folder, screen
  capture. Its directory is removed at once.
- A region no H.264 encoder may accept (more than 36,864 macroblocks or 8,688
  pixels on a side) is reported in the recording island while the frame can
  still be made smaller.
- A recording that cannot be completed says that its unfinished file stays
  until Easnap exits, and where.
- When the trim view cannot open, the recording is kept and offered again at
  the next start, and the message says so.
- A screenshot of a display that could not be read is a failed capture with a
  retry notice, not an image with a black part.

### A recording that cannot be previewed

With a failed preview the trim window turns Save and Copy off: nothing can be
trimmed. The recording itself is complete. Closing the window in that state
asks a different question (`CaptureCoordinator::SaveUntrimmedRecording`):
**Save...** copies the untrimmed MP4 to a place the user chooses, **Delete**
removes it, **Keep recording** returns to the window. Until v1.1 the only ways
out of that window were deleting the recording or ending the process. The
preview is a separate, unsigned executable; a security product that removes
it would otherwise make every recording unsavable.

## Consequences

- There is one more question in a few flows, always with the keeping answer
  as the default.
- A postponed recording stays on disk and is offered at every start until the
  user decides.
- A recording that is still being written when the process dies cannot be
  recovered; see [ADR-030](ADR-030-log-file-and-crash-reports.md).
- The trimmed or converted result of an export that was interrupted is not
  kept; its source recording is.

## Validation

- `session_state_transitions`: the pending-work table for all thirteen states,
  with and without an image export running.
- `image_export_job` (unit): the result and its generation through a
  message-only window, a PNG and a JPEG that decode back to the image, no
  staged file after a failed or cancelled save, a cancel within a second
  (29 to 63 ms in a 1920 x 16,000 encode on the development PC), a result
  that stays after `Wait`, a save waited for and a copy cancelled at session
  end.
- `capture_export_responsiveness` (private desktop): the editor copies a
  1920 x 12,000 image of noise while a thread that shares no input queue with
  it sends `WM_NULL` with a 500 ms deadline. On the development PC
  (2026-11-09, five recorded runs) the copy took 3.1 to 3.9 s and the slowest
  of about 100 replies 31 to 47 ms, none missed; a second Ctrl+C and a close
  request were refused. The clipboard then holds the whole image as a DIB and
  a PNG.
  `capture_workspace_production` and `pinned_images_native` wait for their
  copies and cover a quick save from the editor and from a pin into a folder
  of the test.
- `recording_completion_workflow`: marking, the sweep, adoption by a later
  instance, postponing, deleting, a mark without a recording, and a lease-less
  directory before and after it is an hour old, against the real store.
- `data_safety_questions` (private desktop): named buttons, their order,
  default and Esc answers, labels that fit, the nested-loop mark, and the
  editor's question on every way out, with drawing and repeated requests
  ignored while it is open.
- In the running application, by hand: the discard question on the Scrollshot
  island, the exit question and its refusal of a second Exit, the sign-out
  block while an image is held and while recording, a recording kept through
  `WM_ENDSESSION` and through an abrupt end of the process, "Not now",
  Save and Delete at the following starts.
