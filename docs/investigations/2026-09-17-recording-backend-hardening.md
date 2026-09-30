# Recording backend follow-up validation

Workspace availability (2026-09-23): historical build-tree evidence referenced here
was absent before this task; see [current status](../project-status.md). Recorded results below
remain historical.

Date: 2026-09-17. Windows 10 19045, MSVC 19.44, Windows SDK 10.0.26100,
NVIDIA GeForce RTX 4060 Ti. Separate build: `build-backend-hardening`,
RelWithDebInfo. Full-suite Release/Debug results are recorded separately in the
current project status; this report records the targeted investigation.
Durable invariants: [ADR-021](../decisions/ADR-021-recording-startup-timeline-and-session-leases.md).

## Reproduced defects and changes

### MP4 initialization and native encoder lifetime

The user's existing Release executable reproduced an A/V interior trim failure.
A fresh symbol-bearing baseline then crashed with CTest `SEGFAULT` on the 15th
repeat (14 successful iterations first). This was not dismissed as a test issue.

`MediaFoundationWriter` started its thread in the `worker_` member initializer.
Members declared after that thread—including command flags, startup HRESULT and
sink state—had not finished construction when `ThreadMain` could access them.
This is a data race and access to unconstructed state; a completed MF startup
could also be overwritten by the later `E_PENDING` initializer. The thread now
starts in the constructor body, after all members exist.

The constructor correction passed **30 consecutive iterations** (86.17 s), but
a subsequent full Release suite crashed again. That run disproved the initial
bounded result as sufficient evidence for the reported crash.

Windows Error Reporting retained the actual latest crash dump, locally at
`%LOCALAPPDATA%\CrashDumps\test_mp4_trim_exporter.exe.32620.dmp`.
An offline DbgEng reader captured its stack in
`build-backend-hardening/actual-crash-stack.log`. The exception is access
violation `0xC0000005`: `ntdll!RtlWaitOnAddress` / `RtlEnterCriticalSection`
is called by `nvEncMFTH264x.dll` on an `RTWorkQ` asynchronous callback while the
main thread is in `MFShutdown` / `RtwqShutdown`. All three September 17 WER
records have the same fault offset. Private NVIDIA function symbols are not
available, so nearby export names in the stack are not treated as function
identities. The dumps stay local and are not uploaded.

Two further native lifetime defects were corrected:

- Cancellation previously released the sink without completing pending encoder
  work. Abort now calls synchronous `IMFSinkWriter::Finalize` on its MTA worker
  before releasing the sink and shutting down MF. The output remains a discarded
  staging file and is never published or reported as a successful export.
  Begun ownership is recorded before any metadata allocation can throw.
- Native trim created a new sample containing only the decoded sample's buffers.
  A decoder can recycle its GPU pool when the original `IMFTrackedSample` is
  released, even while a copied buffer is still referenced. The writer now
  rebases and submits the original decoded sample, preserving its allocator
  identity until asynchronous encoding releases it. The internal API documents
  this timing mutation; the source file remains immutable.

These contracts follow Microsoft's [synchronous Finalize documentation](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-finalize)
and [D3D11 decoder sample allocation/lifetime guidance](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation).
The crash stack establishes a native encoder teardown fault; it does not expose
enough private driver state to assign the crash exclusively to one of these two
lifetime defects. Both are fixed rather than adding delays or automatic retries.

The MP4 CTest now performs 12 additional in-flight cancellations across video
and A/V at the first active frame, 15% and 60% progress. Each checks cancellation
was reached, destination/staging cleanup, and source-file fingerprints.

An isolated baseline replay compiled the original native lifetime code with the
constructor initialization correction, against this strengthened regression. It
crashed on process **10** after nine **138/138** passes, exit `0xC0000005`.
The new WER dump has the identical NVENC/critical-section fault. Symbolized main
thread frames locate it in the new cancellation loop at test line 626, waiting
for the exporter worker. This reproduces the remaining defect independently of
the already-corrected constructor race. See
`build-backend-hardening/baseline-replay-crash-stack.log` and
`baseline-replay-source-location.log`.

With both native lifetime corrections, **50 separate processes** completed
under the live debugger with no first- or second-chance access violation.
The final normal CTest then passed **30 consecutive processes** in **151.20 s**,
each with **138/138 checks**, including the strengthened cancellation regression.
The native DXGI/NV12 path remained active on the RTX 4060 Ti. Together these runs
exercise 440 cancelled exports (50 debugger cases plus 13 per normal process),
as well as normal complete exports, source integrity and output validation.

### Immediate and repeated Pause/Stop

The added real recording stress exposed two further production defects:

- Immediate Start → Pause → Stop could reach MP4 finalization with no video
  samples (`0xC00D36B2` / `0xC00D4A44`). Start now waits for the first capture
  attempt and its queue submission to complete before a lifecycle operation can
  overtake it. An exception moves the worker to Stopping and releases this
  barrier; the regression verifies failed startup and clean Stop.
- Repeated short active segments reset capture pacing to “capture now” on each
  Resume while continuing the previous frame index. This rounded each segment
  upward and could place the last video PTS beyond accumulated active time,
  causing “Writing a captured video frame failed” at Stop. Resume now preserves
  the remaining interval on the same active-time frame grid.

The new `recording_pause_stop_stress` CTest runs **32 back-to-back real sessions**
at 15/30 fps across None, System Audio, Microphone and Both. It exercises **104
pause boundaries**, active/paused finalization, active/paused abort, immediate
pause/stop, idempotent Stop/Abort, worker shutdown, bounded queues and session
cleanup while paused. It reads the actual encoded MP4 samples and checks video
and audio PTS monotonicity, positive durations, frame accounting and less than
100 ms presentation/active-time difference (including AAC endpoint granularity).
The targeted final run passed **1240/1240 checks**; all four sources were present.

The first-capture barrier uses the existing provider call and its bounded DXGI
acquisition/fallback. This does not introduce a means to interrupt a permanently
hung OS graphics driver or GDI call; Stop already had that underlying limitation.

The existing full-session test originally started its external stopwatch only
after Start returned. With the first-capture barrier this omitted roughly
30–60 ms of genuine active capture, producing four external-duration assertion
failures even though sample endpoints and precise session/output duration checks
passed. The test now takes one initial active offset at Start return, checks it
is inside the externally measured Start-call duration, and independently times
all subsequent intervals with `steady_clock`. The final production duration is
not substituted for the external elapsed time; existing tolerances are unchanged.
The corrected full-session test passed **1008/1008 checks** in the targeted
RelWithDebInfo run (`build-backend-hardening/real-desktop-anchor.log`).

### Live temporary-session ownership

Startup cleanup previously considered every directory absent from its own
in-memory ownership vector stale. Another live store or process could therefore
remove a paused recording's files. Each new private session now holds an
exclusive `.session-lock` file handle for its recording/trim/clipboard lifetime.
Cleanup must acquire that lease before collecting a session. Windows releases
the handle on process exit/crash, enabling later collection. Normal deletion and
explicit clipboard-lifetime release close it intentionally.

The user/SYSTEM-only protected DACL is unchanged. Cleanup fails closed for live
leases, unknown/legacy directories without a marker and reparse directories.
Consequently, old unmarked session folders are preserved rather than guessed
stale. The regression uses two real stores and protects all unrelated directories
while checking that a live recording survives, a foreign store cannot explicitly
delete it, and a released session is collected. It also runs during each pause
boundary of the recording stress test.

The exact one-off “cannot write to temp” dialog was not supplied with an HRESULT
or log, so it cannot be conclusively attributed to this lifecycle defect.

### Previously failing worker and WASAPI tests

The worker test failure was reproduced at `frames.size() >= 8`: its fixture
announced capture entry before the production worker accepted the fourth frame.
Pause was permitted to discard that in-flight frame, leaving seven accepted
frames. The test now waits on the actual acceptance boundary before asserting
four frames per segment. It passed 30 repeated runs after that correction; the
final pacing/rapid-pause version passed a further **20 repeated runs** (34.61 s).
The final one-shot worker run including startup-exception coverage passed
**212/212 checks**.

WASAPI's historical failure detail was absent from the supplied logs. The
current System Audio, Microphone, Both, delayed-tone, timestamp, queue and
repeated-lifecycle test passed **3 consecutive runs** (35.22 s). The repeat log
contains CTest pass summaries; the accompanying full Release suite also verified
**14/14 WASAPI checks with no endpoint skips**.
No unproven device-side change or lowered assertion was made. A failure on a
different endpoint or with different mixer/mute conditions still needs its
actual failure line and diagnostics.

## Evidence

All paths below are relative to the repository root:

- `build-backend-hardening/trim-before-stress.log` — baseline segmentation fault.
- `build-backend-hardening/trim-debug.log` — native MF diagnostic capture from
  a successful symbol-bearing run; not a captured crash stack.
- `build-backend-hardening/trim-after-construction.log` — 30 export iterations.
- `build-backend-hardening/actual-crash-stack.log` — actual WER dump analysis
  from the subsequent Release crash, including all thread stacks.
- `build-backend-hardening/trim-live-debugger.log` — post-lifetime-fix process
  exits and any first-/second-chance access violations seen by the debugger.
- `build-backend-hardening/trim-lifetime-baseline-replay.log` — isolated original
  lifetime code with constructor fix; new cancellation regression crashes on 10.
- `build-backend-hardening/trim-lifetime-final-normal.log` — 30 final normal
  processes, strengthened cancellation coverage, no crash or assertion failure.
- `build-backend-hardening/pause-stop-immediate-before.log` — empty-MP4 failures.
- `build-backend-hardening/pause-stop-after-start-barrier.log` — reproduced
  rapid-resume timestamp/final-write failure before pacing correction.
- `build-backend-hardening/pause-stop-final-stress.log` — 32-cycle final run.
- `build-backend-hardening/worker-after.log` and `worker-pacing-final.log` — repeats.
- `build-backend-hardening/wasapi-stress.log` — three endpoint test passes.
- `build-backend-hardening/session-lease.log` — protected session lifecycle check.

This is bounded stress on one Windows/GPU/audio setup, not an indefinite soak or
cross-device certification. Protected storage and real encoder/device failures
remain visible; no fake media path, retries that hide a failure, or blanket skip
was introduced.
