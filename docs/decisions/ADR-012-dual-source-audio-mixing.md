# ADR-012 — Dual-source audio mixing

**Status:** Accepted  
**Date:** 2026-09-02

## Context

`AudioSource::Both` must capture System Audio loopback and Microphone at the
same time and submit one coherent PCM stream to the existing single AAC writer
input. The endpoints can expose different sample rates, channel layouts, sample
representations, packet cadences, and independent QPC epochs. Arrival order is
therefore not a valid alignment clock.

The current MP4 configuration exposes Both through the shared capability model
in ADR-014.

## Decision

1. The public audio-provider factory routes single-source requests to the
   unchanged WASAPI endpoint provider. A composed dual provider owns two such
   providers for `Both`, preserving the tested single-source implementation.
2. Each endpoint captures its native WASAPI mix format and keeps its own QPC
   epoch. Every precise packet timestamp is translated through that epoch into
   the dual provider's common QPC domain. Fallback timestamps advance from
   source sample cadence.
3. Each native stream is converted independently by the Windows Media
   Foundation Audio Resampler DSP at quality 60 into 48 kHz, stereo, 32-bit
   IEEE-float PCM with stereo channel mask `0x3`. This is an AAC-friendly rate,
   represents headroom safely during mixing, and gives the existing writer one
   stable input format regardless of endpoint formats.
4. A dedicated MTA worker places converted packets on the canonical 48 kHz
   frame grid. Within an established continuous run for each source, sample-count
   continuity is preserved across packet boundaries so normal OS packet scheduling
   and QPC jitter cannot create audible micro-gaps, duplicated samples, or clicks.
   Gaps exceeding tolerance (240 frames, 5 ms at 48 kHz) and explicit discontinuities re-anchor cleanly
   to the observed QPC timestamp. A 100 ms look-behind absorbs normal endpoint
   scheduling skew without sleeping for alignment or mixing by arrival order.
   Missing source frames are silence, including late endpoint startup and
   temporary source silence. Packets arriving entirely after the look-behind
   output frontier are discarded as obsolete late data without failing the mixer.
5. The mixed value is `0.5 * system + 0.5 * microphone`, clamped to `[-1, 1]`.
   This fixed -6 dB contribution per source prevents two full-scale correlated
   inputs from clipping while keeping the result deterministic.
6. Pause first drains both endpoint deliveries, stops accepting new input, then
   drains the native queues, both DSPs, and every complete pre-pause mix chunk.
   The mixer is reset before delivery resumes. `RecordingSession` establishes a
   new active-time segment on resume, so differing resumed QPC positions and
   paused wall time cannot advance the encoded timeline.
7. `Both` is transactional. If either source is unavailable or fails to start,
   startup rolls back and reports the named source. A mid-session failure makes
   the composite provider unhealthy and records source-specific diagnostics.
   It never silently degrades to one source. Temporary silence without a
   provider failure is mixed as silence.
8. Native queues are capped at 500 ms per endpoint, the dual provider's native
   staging queues are capped at 500 ms per source, and the canonical mixer ring
   is capped at 500 ms. Output is emitted in 10 ms chunks. The existing
   throttled writer remains disk-backed; no recording-duration-sized memory
   buffer is introduced.
9. The writer receives exactly one already-mixed PCM stream and therefore
   creates exactly one AAC track.

## Consequences

- Different endpoint formats and independent clocks are supported without
  changing System Audio-only or Microphone-only behavior.
- Within continuous runs, sample-count continuity eliminates packet-boundary
  jitter, pops, and crackle. Timing discrepancies larger than tolerance
  re-anchor to QPC, preventing unbounded clock drift.
- The 100 ms look-behind is intentional recording latency, not UI-thread work;
  capture callbacks only enqueue bounded data and mixing runs on its own worker.
  Continuous arrivals cannot starve mixed output draining.
- Source-specific failures are actionable while shutdown and finalization retain
  a deterministic ownership and drain order. Late packet discards are tracked as
  reconciliation rather than fatal capture failures.
