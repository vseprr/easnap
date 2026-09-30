# ADR-021: Recording startup, active-time pacing and session leases

- **Status:** Accepted for implementation
- **Date:** 2026-09-17
- **Scope:** Existing recording/MP4 pipeline correctness

## Context

The follow-up investigation reproduced intermittent MP4 trim failure/crash,
empty-media finalization after immediate Pause/Stop, and a final video timestamp
beyond active duration after repeated short recording segments. Temporary
cleanup also had no cross-process indication that another store still owned a
recording. See the [investigation and validation report](../investigations/2026-09-17-recording-backend-hardening.md).

## Decisions

1. **Publish fully initialized worker state.** The Media Foundation writer starts
   its dedicated thread only from the constructor body after every member is
   constructed. Capture Start completes its first capture attempt and submission
   before returning, so immediate lifecycle input cannot discard the recording's
   only initial frame. Worker failure wakes the startup barrier and remains a
   failure; this does not create substitute media or suppress encoder errors.

2. **Resume the existing active-time frame grid.** Pausing excludes wall time but
   does not create a fresh video frame interval. On Resume, pacing waits for the
   remainder of the next nominal interval using accumulated active time and the
   current frame index. Accepted frame PTS therefore remains within the active
   recording duration even after many short segments. The existing final-frame
   duration and continuous AAC timeline contracts remain.

3. **Require a real session lease before cleanup.** A private session holds an
   exclusive `.session-lock` file handle through recording, trim and owned
   clipboard publication. Cleanup first acquires this lease; a live holder makes
   the session ineligible. The current-user/SYSTEM protected DACL remains, and
   explicit deletion is restricted to a store's owned sessions. Normal deletion
   closes the lease, intentional release transfers responsibility to the existing
   external clipboard lifetime, and process exit/crash releases the Windows
   handle automatically. Cleanup then permits collection unless an external
   protected-directory owner still names that session.

4. **Keep native sample and encoder lifetimes intact through cancellation.**
   Native trim submits the original decoded sample after rebasing its timing,
   retaining its `IMFTrackedSample` pool ownership until the sink releases it.
   Abort completes pending sink work with synchronous Finalize on the MTA worker
   before releasing the sink or MF runtime. This does not publish the aborted
   staging file. A begun sink is owned before any subsequent allocating metadata
   copy, so exception cleanup follows the same lifecycle.

## Consequences and limits

- Startup can include the initial capture cost; the production DXGI acquisition
  is bounded and retains its existing GDI fallback. No new mechanism interrupts
  a permanently hung driver or GDI call.
- Sessions without a lease marker, including directories from older builds,
  are preserved. Cleanup also rejects reparse directories. Failure to establish
  ownership does not justify deleting an unknown folder.
- Existing capture, encoding, trim staging/publication and source-immutability
  architecture stays intact. The real regression suite covers initialization,
  rapid and immediate Pause/Stop, active/paused abort, monotonic decoded PTS,
  worker shutdown and two-store cleanup using actual Windows file sharing.
- Abort can wait for native encoder drain on the worker, as finalization already
  does. No arbitrary sleep, successful-export fallback or retry hides a failure;
  permanently hung native components remain an underlying platform limitation.
- The actual WER stack places the reproduced crash in NVIDIA's asynchronous
  encoder callback during MFShutdown. Its private driver state does not isolate
  which native lifetime violation caused it. The user's one-off temp-write
  incident remains unattributed without its original HRESULT; passing stress is
  bounded evidence.
