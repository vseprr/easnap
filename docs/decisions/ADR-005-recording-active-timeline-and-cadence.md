# ADR-005 — Active-time recording cadence and static-frame reuse

**Status:** Accepted  
**Date:** 2026-08-28

## Context

Desktop Duplication produces frames when the composed desktop or pointer changes;
it is not a fixed-rate camera. The screenshot acquisition path may wait through
several 30 ms timeouts, but using that policy at 15 fps made one static recording
attempt take up to roughly 240 ms. Phase 2.3 consequently combined missed pacing
slots with queue-full rejection in one `dropped_frames` value, even when the
three-slot queue peaked at one frame.

Recording also needs a deliberate end-of-timeline rule. The last encoded sample
must cover the active Stop endpoint even when no new desktop pixels arrive close
to Stop, and paused wall-clock time must not appear in the MP4.

## Decision

1. Recording polls `AcquireNextFrame` without waiting. A timeout means
   `NoNewFrame`, not failure. The screenshot acquisition timeout/retry policy is
   unchanged.
2. The capture worker retains one immutable shared pointer to the latest valid
   region payload. A cadence slot with no new frame reuses that payload with a new
   session-relative timestamp. No BGRA pixels are copied for reuse, and the
   retained pointer does not create a second pixel allocation.
3. Cadence is indexed from `steady_clock`. Slow capture skips overdue slots in
   one bounded calculation; it never busy-spins or performs runaway catch-up.
4. Pause closes the current active interval and freezes the frame index. Resume
   starts a new active interval and pacing epoch. Timestamps therefore remain
   monotonic while paused wall time is excluded.
5. The consumer retains at most one pending sample. Its duration is set when the
   next accepted timestamp is known. After capture stops and the bounded queue is
   drained, the final pending duration ends at the measured active Stop endpoint
   before Media Foundation finalization.
6. Permanent saturating diagnostics distinguish attempts, acquired frames,
   no-new-frame results, reused frames, capture failures, pacing misses,
   queue-full rejections, accepted frames, and writer-written frames. Timing is
   aggregate-only (count/total/min/max); no per-frame or screen-content log is
   retained.

## Consequences

- A static recording keeps near-target sample cadence without treating normal
  duplication timeouts as failures or growing memory with duration.
- Queue overload and capture/pacing cost are independently measurable.
- MP4 duration represents active recording time to the Stop endpoint, subject to
  normal container time-base rounding, and excludes pauses.
- The one pending consumer sample adds one cadence interval of encode latency but
  no unbounded buffering. Shared reuse keeps the existing bound of three queued
  BGRA payloads plus capture/consumer in-flight payloads and one NV12 conversion
  buffer.
- Cross-output regions use the established GDI fallback for a consistent sample
  rather than waiting on unrelated duplication present streams.

