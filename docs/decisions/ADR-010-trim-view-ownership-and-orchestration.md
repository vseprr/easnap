# ADR-010 — Trim view ownership and background orchestration

**Status:** Accepted  
**Date:** 2026-08-31  
**Updated:** 2026-09-30

Known limitation: transient redraw artifacts while the trim window is resized or
maximized are recorded in [project status](../project-status.md) and
[ADR-024](ADR-024-native-trim-frame.md). They also occur when no preview helper
is created, so the preview described below is not a necessary cause.

## Context

After safe recording finalization, Easnap must preview a disk-backed MP4, edit
one exact trim interval, and call the synchronous `ITrimExporter` without
blocking the Win32 UI thread. Preview callbacks, export completion, cancellation,
window closure and process shutdown can overlap. Durable Save/Copy/Discard is
defined by ADR-011, with shared MP4/GIF configuration and completion in ADR-014.

Two MFPlay failures observed on 2026-09-10 shape the preview decision. On an
H.264/AAC file whose container duration (12.0106458 s) exceeds its last video
timestamp (11.9666666 s) because of an AAC tail, `SetPosition(duration)`
returned `S_OK` and never delivered `POSITION_SET`; the seek stayed owned for
7,218 ms and blocked Play. A paused interior seek also lost its callback and
was still pending after 2,125 ms; in-process recovery then deadlocked inside
synchronous MFPlay `Shutdown`, in MFCORE/decoder locks. An in-process timeout
could therefore detect a missing callback but not retire the player.

## Decision

1. `CaptureCoordinator` remains the sole owner of `SessionState`, the finalized
   source path, the latest successful temporary result path, recording options,
   and `TrimExportController`. `TrimWindow` owns only native controls, range
   interaction and preview resources; callbacks are commands to the coordinator.
2. The source `RecordingSession` remains alive throughout trimming in protected
   temporary storage. Terminal destinations and prepared clipboard publications
   use the staged lifetime/publication rules in ADR-009/011/014. A temporary
   result alone is not durable Save success.
3. `TrimRangeModel` is the single range policy. It initializes to the full source
   duration, clamps the playback cursor, maps pixels to media time, and accepts
   only non-empty half-open `[in, out)` bounds compatible with ADR-009.
4. Preview uses MFPlay over the finalized file. It preserves aspect ratio and
   decodes playback on demand. The helper retains at most two BGRA stills: the
   source endpoint and the current selected boundary/restart frame. Each image
   is dimension/stride validated and capped at 256 MiB; decoder working memory
   is additional. No thumbnail cache grows with recording duration.
   `CreateMfPlayPreview()` creates a proxy on the caller's UI thread. It starts
   the adjacent `EasnapPreviewHost.exe` in an owned kill-on-close job, with only
   two explicitly inherited private pipe handles. The source path is sent in
   the bounded Open packet, never placed on the command line or in diagnostics.
   The helper owns native calls, metadata reads, MFPlay's default STA callbacks
   and the video child under the caller's video surface. The child retains
   MFPlay's `WM_PAINT`/resize `UpdateVideo` contract; the parent coalesces resize
   requests through the pipe with one resize outstanding until acknowledged.
   Size changes also coalesce one private posted poll on the owner's message
   window. This prevents continuous input from starving acknowledgement reads
   behind the 100 ms WM_TIMER fallback. Polling stays outside layout calls; it
   does not wait for the helper or repost itself while awaiting a reply. Close
   retires the message window and clears the pending marker. With timers
   deferred, 40 resize requests produced one helper size change before this
   change and 16 after it, through 80 posted messages and no timer dispatch
   (`ResizeWhileTimersAreDeferred`, measured during the 2026-09-29/30 repair);
   the 3.6 s no-poll recovery check still passed. This scheduling does not
   address the trim window's resize artifact (ADR-024).
   Only the helper positions its native video child. Caller lifetime and
   mutating operations remain on the creator
   UI thread with its message loop running. No detached playback worker exists.
   `PreviewTransport` reconciles latest desired Play/Pause and position against
   observed state, with at most one native Play, Pause or SetPosition owned at
   a time. Every seek while Playing first waits for an owned Pause, then the
   real Seek completion, then Play only if it remains the newest intent.
   Unissued positions coalesce. Only the owned terminal callback or
   synchronous call failure releases that command in a live generation;
   STOP/PLAYBACK_ENDED never retires an unrelated operation. Command numbers are
   diagnostic identities: MFPlay supplies no caller-defined transport token.
   Only `MF_E_SESSION_PAUSEWHILESTOPPED` for the owned Pause is reconciled as
   non-playing even when newer intent requests Play. Other native failures are
   explicit, and callbacks cannot restore a Failed preview.
   Current `GetState` determines observed playback; historical event headers
   identify completion and supply diagnostics. A late STOP/ENDED cannot terminate
   newer native playback. Natural end with no outstanding restart clears Play
   intent, retains logical duration and does not loop. The last decoded picture
   remains visible. Restart from Ended or selected Out remains an In
   seek followed by Play after its real completion. Play/Pause toggles the latest
   intent, including while restart is pending, and uses one coherent snapshot
   for the playback state and position instead of the last displayed UI sample.
   Selected intervals remain half-open `[in, out)`. Automatic Out handling uses
   the same PauseAt reconciler as user intent; the timer excludes pending seeks
   and also reconciles a late natural Ended snapshot against selected Out.
   Active timeline clicks outside the selected interval remain ignored.
   `TrimWindow::HandleTimelineMessage` and `UpdateTimelineFromPoint` treat
   unchanged seek coordinates within one captured pointer gesture as one
   request. Playback advancing during a held click must not turn its release
   into a second seek. Changed targets during dragging still seek immediately;
   a new click at the same point is a new request. The gesture target clears on
   button down/up, capture loss, Hide and Destroy. Keyboard and UIA requests
   retain their direct `SeekTo` path and do not share pointer deduplication.
   During Opening, `ReadPreviewVideoEndpoint` selects the native compressed
   video format, seeks to presentation duration, and reads the final GOP to EOS
   to find the greatest video presentation timestamp. It neither decodes nor
   falls back to a whole-source scan. Scanning has a 4,096-event limit and a
   two-second deadline checked between synchronous calls. Missing or invalid
   metadata fails Open explicitly. The result is cached for this source and
   retained for that helper lifetime. A replacement reopens the same source.
   Every native seek is capped at that exact final video timestamp. Container
   duration, selected In/Out, and the logical requested target remain unchanged,
   including any valid AAC tail after video ends. An exclusive Out seek selects
   the greatest actual decoded video PTS strictly before Out; ordinary paused
   Seek and replay use the frame covering their inclusive target. The logical
   cursor remains at Out until another action. Its command stays
   pending until a real POSITION_SET arrives; no epsilon, frame-rate estimate,
   or synthetic seek completion substitutes for native acknowledgment.
   `DecodePreviewFrame` seeks a software RGB32 Source Reader to the requested
   boundary, reads the surrounding GOP, then copies the selected sample into a
   validated still. The decoded buffer's display aperture is checked against
   native dimensions through the same helper used by GIF decoding; codec
   alignment rows/columns are never presented as picture content. The case
   observed on 2026-09-23 was a 1920 × 1088 buffer from the software decoder
   for a 1920 × 1080 picture, whose eight extra rows were green. It shares the 4,096-event/two-second between-call bounds;
   blocked synchronous calls remain contained by the supervised helper. Open
   predecodes the measured final frame. A helper-owned high-resolution waitable
   timer (high resolution on Windows 10 1803+, regular waitable timer on older
   Windows 10) schedules its
   presentation from the actual final PTS and rechecks the native clock before
   showing it, covering EVR before EOS clears its surface. Its threadpool wait
   only posts to the owner HWND; all native state/painting stays on the owner
   thread, and Close cancels and drains the wait before destroying that HWND.
   Unlike WM_TIMER, this deadline has no 10 ms minimum or low-priority paint
   starvation; it does not alter the system-wide timer resolution. It does not stop
   transport/audio, change logical position or manufacture completion. Single-
   frame media retain that same image during playback. Replay presents the
   selected In still during the native seek and through that decoded sample's
   actual presentation interval after genuine Play. A PLAY callback alone is
   not evidence that EVR has presented the first picture.
   Native video and the retained still own separate sibling HWNDs. The still
   uses an opaque layered child surface, retaining WS_CLIPSIBLINGS. Its existing
   GDI aspect-fit drawing uses shared PaintBuffer; PresentLayered submits the
   complete bitmap with UpdateLayeredWindow before native video is hidden.
   This isolates the still from stale pixels in the composited parent's backing.
   Suppressing redraw or prepainting that shared backing alone did not reliably
   prevent duplicated trim controls after maximize/restore. Two other designs
   were rejected on 2026-09-29: continuous parent-backed painting showed a
   future endpoint during paused resize, and converting every settled pause to
   a still made resizing worse.
   The helper explicitly presents on handoff and WM_SIZE: an uninitialized
   layered child does not bootstrap this through WM_PAINT. Its Windows 10
   compatibility manifest enables layered child support; omitting it caused
   HRESULT 0x80070490 during validation. Failed presentation is reported as a
   preview failure. Ordinary playback and paused images remain native.
   Play reveals the native child behind the held In frame before starting
   transport. After In's real sample interval, the still is hidden. There is no
   additional decoded-frame cache or synchronous cross-process paint call.
   Only this still child uses layered presentation; the top-level frame,
   capture overlays and ordinary controls keep their existing paths. Only the native child's
   own BeginPaint/EndPaint handler calls MFPlay UpdateVideo. Parent paint never
   unconditionally resizes/invalidates its child; this
   avoids a recursive composited paint loop during live window movement.
   Still painting and positioning touch only helper-owned HWNDs; parent resize
   remains asynchronous. Private protocol version 4 carries the exclusive-end
   distinction and rejects mismatched parent/helper versions.
   A coherent snapshot exposes desired/native playback, logical/native position,
   pending position, owned command, generation and diagnostic history. A paused
   acknowledged logical target survives native clock resets until playback
   resumes. If a logical start lies after the final video frame, its position
   remains a floor while the native clock catches up from the capped frame;
   playback cannot move the logical cursor before selected In. Subsequent
   position advancement uses the native clock, and a newer seek replaces the
   floor.
   The helper's 100 ms monitor detects a command outstanding for two seconds,
   requested playback without native clock progress, or stable unwanted Play.
   `PreviewLiveness::Observe` consumes ordered observations from the current
   generation. Its command sequence identifies actual native dispatch, not
   accepted user intent. A Pause/Seek/Play cycle may finish between two samples;
   a changed sequence with no command outstanding starts a new playback-clock
   baseline. Otherwise a legitimate backward seek could be measured against
   the old higher position and falsely request recovery. Coalesced input,
   logical cursor changes and unsolicited backward native timestamps do not
   extend an owned command's deadline. After the last completed command,
   missing native progress still triggers the existing two-second bound.
   It requests recovery without clearing ambiguous operation ownership. Only
   the parent owns replacement and its retry budget. The parent polls every
   100 ms and independently bounds absent heartbeat and unacknowledged intent
   to three seconds, and Opening to five seconds. Publication timestamps
   identify delayed snapshots. The accepted ADR-024 correction preserves a
   bounded refresh grace across polls after returning delayed snapshot credit:
   a fresh reply ends it; another old packet cannot extend it; the maximum is
   three seconds. Pipe/process failures and native recovery requests remain
   immediate. This prevents premature replacement while a fresh acknowledgement
   is in flight without allowing indefinite stale-snapshot liveness.
   IPC acceptance is distinct from native command completion. At most one small
   intent packet is unacknowledged; newer input replaces local desired state.
   Resize acknowledgment has an independent three-second deadline. Direct
   cross-process SetWindowPos is avoided: SWP_ASYNCWINDOWPOS only posts across
   different input queues, which a parent/child relationship need not provide.
   See the [Windows SetWindowPos contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos).
   Exceptional recovery retires the entire owned process before replacement,
   independent of any blocked MFPlay/decoder/Shutdown call. Normal Close allows
   100 ms for graceful exit; termination has a 900 ms confirmation wait. If exit
   cannot be confirmed, the handle is retained, failure is explicit, and no
   replacement starts. Fresh pipes, video window and parent generation isolate
   all old callbacks/snapshots. Source, range and newest position/Play intent
   survive. Genuine source/native errors remain terminal errors.
   Recovery permits one exceptional replacement until one second of sustained
   playback with at least 500 ms of native advancement proves usability. A
   repeated stall retires the ambiguous helper and reports failure instead of
   cycling indefinitely. The editor executes no native playback call.
   Diagnostics retain bounded transition history with monotonic trace/time,
   thread, intent, player generation, owned command and age, logical/native
   position, HRESULT, callback disposition and reconciliation outcome. Liveness
   failure emits the recent history before replacement or explicit failure.
5. `TrimExportController` permits one export at a time and owns one joinable
   orchestration worker. Progress is synchronized; cancellation is atomic; all
   destruction and shutdown paths cancel and join. No detached worker may retain
   a window, coordinator or artifact path.
6. Completion messages carry a monotonic export generation. Stale queued
   messages are ignored, preventing an earlier completion from joining or
   reconciling a newer export.
7. Ordinary success, cancellation and failure transition `Exporting →
   RecordingTrimming`. Only a non-empty published destination is accepted as
   success. The source and any earlier successful result remain available for a
   retry. Confirmed Discard during export requests cancellation and transitions
   to `Idle` only after the worker has stopped. Closing the idle trim surface
   also asks for Discard confirmation under ADR-011.

## Consequences

### Preview transport ownership

| Value | Authority |
|---|---|
| Desired Play/Pause and unissued seek | Latest explicit user/range intent |
| In-flight command | Owned terminal result, or retirement of its entire native generation |
| Observed playing | Current native `GetState`, or exact owned stopped-Pause failure |
| Event header state | Historical diagnostic evidence, not current-state authority |
| Preview state | Projection of observed transport state, with explicit end/failure |
| Logical position during seek | Newest requested target; pending remains explicit |
| Acknowledged paused/stopped position | Successful seek target or natural duration until another intent |
| Advancing position | Native `GetPosition`, floored at the newest logical playback start |
| Native terminal seek target | Exact greatest video PTS, or decoded PTS before exclusive Out; logical target stays independent |
| Callback authority | Current native generation on the helper STA |
| Snapshot authority | Current owned process/pipe generation and acknowledged intent |
| Stall recovery | Parent alone confirms old process exit before replacing it |

In-process timers
could detect a missing callback but could not interrupt a blocked native
Shutdown (see Context); confirmed process retirement is therefore part of the contract.

### General

- Preview memory remains bounded with recording duration. Terminal metadata
  reads only the final compressed GOP; still decoding reads its target GOP
  within the stated work bounds. Native
  synchronous calls are confined to the helper; supervision still requires a
  responsive parent message loop and functioning OS process termination.
- Portable and installed builds require `EasnapPreviewHost.exe` beside Easnap.
  CMake builds, tests and installation carry both executables together.
- State and artifact lifetime do not depend on native-window destruction order.
- Repeated export attempts are isolated from stale worker notifications.
- MFPlay is a legacy Windows API, but it is available across Easnap's Windows 10+
  baseline and requires no extra runtime. Process containment supplies the
  bounded retirement MFPlay itself cannot guarantee. A later migration to
  `IMFMediaEngine` can remain behind
  `IMediaPreview` without changing coordinator or trim-window contracts.
- ADR-011 defines terminal Save, Copy, confirmed Discard and clipboard-file lifetime.

### Measured

- 2026-09-10, after MFPlay moved into the helper: 90 of 90 H.264/AAC audio
  stress cycles completed without a helper replacement; 32 resize updates
  against a stalled helper returned in 0 to 16 ms; closing a stalled helper
  took 141 ms.
- 2026-09-30, continuous seeking after the duplicate-seek and liveness
  corrections: a 120 s run completed 539 seeks (267 backwards, 227 requested
  while a command was owned) with a maximum native command age of 203 ms
  against the two-second deadline and no helper replacement. The timeline
  click stress completed 288 clicks per configuration with a maximum picture
  freeze of 63 ms (Release) and 78 ms (Debug). Before the corrections the
  deterministic transport/health regression failed 510 assertions and 12 held
  timeline gestures failed 24 same-coordinate move/release assertions; after
  them the regression passed 210,363 of 210,363 assertions.
