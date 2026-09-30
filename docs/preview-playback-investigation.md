# Trim preview playback liveness investigation

Workspace availability (2026-09-23): historical build-tree evidence referenced here
was absent before this task; see [current status](project-status.md). Recorded results below
remain historical.

Date: 2026-09-10. Scope: preview transport, its UI policy, native tests and
deployment. Recording, audio capture and export implementations were unchanged
by that investigation. This is historical evidence; use
[project status](project-status.md) for the current baseline and validation.

## Reproduced failure

The generated H.264/AAC fixture reproduces the reported loss of playback with
Out at file end. Its presentation duration is 12.0106458 seconds, its final
video timestamp is 11.9666666 seconds, and its last video sample ends at
11.9999999 seconds. The container's valid AAC tail extends beyond video.

The instrumented original adapter captured this exact ordering:

| Trace sequence | Transition and evidence |
|---|---|
| 12–13 | Play command 2 completes; native state Playing, no operation pending |
| 14–19 | `PauseAt(duration)` requests Pause; command 3 completes normally |
| 20–22 | Seek command 4 targets 120,106,458 ticks; `SetPosition` returns S_OK |
| 23–24 | About 31 ms later, `PlayFrom(In)` requests 10,000,000 ticks; native position is Out, Seek 4 still owned |
| 25–28 | Pause then Play supersede desired state; neither can retire Seek 4 |
| Final snapshot | After 7,218 ms, desired Play is true, native state is Paused, logical position is In, native position is Out, Seek 4 remains outstanding |

There is no `POSITION_SET` for command 4. The timeline's logical target remains
editable because it is separate from the actual native clock. Position at In is
valid restart preparation; the defect is the indefinitely owned native Seek
preventing subsequent playback.

**Violated invariant:** an accepted native operation must complete, fail, or
have its entire ambiguous owner safely retired within a bounded interval.
Serialization alone provided safety but no liveness.

Original executable, instrumented source and full traces are retained locally
under `build-remediation/preview-investigation/`, particularly
`original-endpoint.stderr.log` and `original-audio-isolated.stderr.log`.

## Additional reachable failure

Correcting only the endpoint exposed another failure during longer audio
stress. Active Seek 283 to 1.5 seconds completed; Pause 284 completed at roughly
1.72 seconds; paused Seek 285 to 2 seconds returned S_OK without its callback.
The watchdog observed the same operation still pending after 2,125 ms.

Attempted in-process recovery then deadlocked inside synchronous MFPlay
Shutdown. Captured native stacks locate the block in MFCORE/decoder locks
reached from `RetirePlayer`, not the adapter's state mutex. Evidence is retained
in `exact-frame-hang-ring.txt` and `exact-frame-hang-stacks.txt`. A separate
mapped relink resolved the release stack addresses.

This established why merely adding an in-process timeout and calling Shutdown
could still block the editor. Native retirement needs an independent owner.

## Implemented correction

- Opening reads the compressed final video GOP to its EOS and measures the
  greatest video PTS. Native seeks cap at that actual frame; logical targets,
  source duration and half-open selected bounds retain the AAC tail. A real
  POSITION_SET remains mandatory. Invalid endpoint metadata fails explicitly.
- Every active seek waits for Pause, then Seek, then Play if still desired.
  Pending user intents coalesce and never overlap native transport operations.
  The exact-frame correction plus paused seeks passed the initial 90-cycle
  audio matrix without routine recovery.
- The UI uses a coherent current snapshot for toggle/restart policy. Logical
  seek targets, native position and actual playback remain distinct. A logical
  start after the last video frame stays a floor until the native clock catches
  up, preserving selected In.
- The public preview proxy owns `EasnapPreviewHost.exe` in a kill-on-close job.
  MFPlay, decoder calls, metadata reads and native Shutdown run only in that
  helper. Private inherited pipes carry complete desired intent and timestamped
  snapshots; the embedded video window handles native repaint in the helper.
  Resize requests also use the pipe, with at most one outstanding resize and
  an acknowledgment deadline. The parent never directly positions the native
  child: Windows only guarantees SWP_ASYNCWINDOWPOS posting across different
  input queues. See the [API contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos).
- Native timeout detection requests recovery; only the parent may replace the
  process. It confirms old-process exit before launching a fresh generation,
  preserving newest seek and Play/Pause. Old callbacks and pipe data cannot
  reach the replacement. One retry is allowed until sustained measured native
  progression proves health; repeated failure is explicit and reusable by Open.

The command deadline is two seconds. Parent heartbeat/intent-acceptance
deadlines are three seconds, and Opening is bounded at five seconds, checked
on a 100 ms parent timer. Termination confirmation is bounded to 900 ms;
normal Close additionally grants 100 ms for graceful exit. If termination
cannot be confirmed, the owned process handle is retained and replacement is
refused. No pending native operation is cleared to fabricate completion.

## Verification

These are the 2026-09-10 investigation results; newer automated results and
manual acceptance are recorded in [project status](project-status.md).
That investigation's final Release suite passed **16/16 in 323.21 seconds**,
including real desktop recording and all 14 WASAPI cases without skips. The
separate audio matrix passed **90/90 cycles with zero ordinary replacements**:

| Seed | Cycles | Automatic Out stops | Inputs while pending | Seconds |
|---|---:|---:|---:|---:|
| `0x13579bdf` | 30/30 | 35 | 817 | 129.08 |
| `0x6d2b79f5` | 30/30 | 31 | 811 | 129.22 |
| `0xc001d00d` | 30/30 | 40 | 815 | 131.46 |

All three processes returned exit code 0, all stderr logs were empty, and no
owned test/helper process remained. Every seed also passed the exact endpoint
restart, six pending Close/Open cycles, forced-stall recovery and retry
exhaustion, and subsequent reuse. Thirty-two resize updates while stalled
returned in 0–16 ms; stalled Close took 141 ms. Healthy resize bursts reached
the final child dimensions without replacement. Evidence and fixed binaries
are retained under `build-remediation/preview-investigation/final-process/`.

To repeat the audio matrix:

```powershell
foreach ($seed in @('0x13579bdf', '0x6d2b79f5', '0xc001d00d')) {
    & .\build-remediation\bin\Release\test_media_preview.exe --focus-audio $seed 30
    if ($LASTEXITCODE -ne 0) { throw "Preview stress failed for $seed" }
}
```

Coverage includes:

- 205,769 deterministic checks of real production policy, including 512 × 128
  generated event schedules and 64 × 9 independently timed stress regions;
- the exact missing-callback ordering, terminal logical/native position,
  delayed and stale events, pause-before-seek, and last-intent convergence;
- generated video-only and H.264/AAC native fixtures, 10–300 ms user timing,
  callbacks between actions, and the same selected-Out policy used by the UI;
- final Play with measured advancement of both clocks, final Pause with stable
  position, and continued Pause/Seek/Play reuse after each stress cycle;
- suspended owned-helper faults, confirmed retirement before replacement,
  latest-intent preservation, retry exhaustion, bounded resize/Close and same-object
  reopen. Normal stress rejects any helper replacement.

These are automated native results. The user subsequently accepted the app and
closed Phase 3.3 on 2026-09-14 with no issues reported. Broader DPI/GPU/audio-device
coverage belongs to Phase 4 release validation. Supervision assumes the parent
message loop and OS process termination remain responsive. Portable deployments
must keep the matching helper beside `Easnap.exe`; CMake builds and installs
them together.

The durable ownership decision is in
[ADR-010](decisions/ADR-010-trim-view-ownership-and-orchestration.md).
