# ADR-006 — Audio clock, bounded buffering and overruns

**Status:** Accepted  
**Date:** 2026-08-28  
**Amended:** 2026-09-13 — describe the implemented timestamp fallback and Both integration

## Context

Captured audio and video need a common session-relative time domain without
turning callback scheduling delays into media timing. Device stalls and consumer
backpressure must not grow memory with recording duration.

## Decision

### Clock source

WASAPI `IAudioCaptureClient::GetBuffer` provides a QPC-correlated packet
position in **100 ns units**. Subtract the capture epoch in that same domain to
obtain session-relative `MediaTime`; do not apply the raw QPC-frequency conversion
to an already converted WASAPI value. Raw `QueryPerformanceCounter` readings are
converted separately.

The production timestamp resolver validates the packet position, timestamp-error
flag, capture time and available device-clock correlation. It uses plausible
WASAPI timing first, validated `IAudioClock` correlation next, then an explicit
capture-QPC fallback adjusted for buffered audio. Fallback source/flags and
bounded diagnostics remain observable. Wall-clock UTC is never used as media time.

Downstream active-time mapping, continuity and pause/resume semantics are defined
in ADR-007; dual-device alignment is defined in ADR-012.

### Buffering and loss

Each WASAPI source has a fixed-capacity `AudioQueue` derived from a 500 ms budget
and its buffer period, with at least four slots. At 48 kHz stereo float, 500 ms
of payload is approximately 192 KB, excluding container overhead.

When full, the producer rejects the newest packet and continues capture.
A saturating overrun counter records the loss; the next accepted packet carries
a discontinuity flag. Consumers must handle gaps through the production timeline
policy rather than silently treating the stream as contiguous.

### Formats and composition

WASAPI captures the negotiated mix format as-is. `AudioFormat` carries rate,
channels, sample representation and channel mask. Encoder integration handles
required AAC input conversion (ADR-007).

System audio and microphone may run separately or through the implemented Both
provider. Both aligns, resamples and mixes the two native sources into one
canonical stream (ADR-012); the MP4 configuration exposes it under ADR-014.
GIF creates no audio provider.

## Consequences

Audio uses the video pipeline's 100 ns time domain while retaining explicit
device-clock and fallback provenance. Queue size is independent of duration.
Loss and invalid timestamps remain diagnosable, and format conversion occurs
where the encoder or dual-source composition requires it.
