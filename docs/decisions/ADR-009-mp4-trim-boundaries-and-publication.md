# ADR-009 — MP4 trim boundaries, timeline normalization, and publication

**Status:** Accepted  
**Date:** 2026-08-31

## Context

Easnap recordings are finalized temporary MP4 files containing H.264 video and,
optionally, AAC audio. A production trim backend must accept arbitrary in/out
bounds, preserve A/V synchronization, avoid recording-duration-dependent memory,
and never damage the source or expose a partial destination as success.

Compressed-sample remuxing alone cannot guarantee an arbitrary requested video
start because H.264 decoding depends on earlier keyframes. AAC packets also have
codec-frame granularity. The trim UI and durable destination workflow consume
these stable backend boundary semantics through ADR-010/011.

## Decision

1. `ITrimExporter` interprets MP4 bounds as the half-open interval
   `[start, end)`. Bounds must be non-negative, non-empty, and contained by the
   source presentation duration.
2. `Mp4TrimExporter` scans incrementally from source time zero with a Media
   Foundation source reader. Its primary Windows 10 path supplies one shared
   `IMFDXGIDeviceManager` to the source reader and sink writer, negotiates NV12,
   and forwards bounded decoded samples directly to H.264 encoding. DXGI-backed
   samples remain on D3D11 surfaces. If D3D/native-NV12 negotiation is
   unavailable, it first accepts system-memory NV12 and finally retains the
   bounded RGB32/BGRA production fallback. AAC remains one bounded float-PCM
   sample at a time. The exporter does not depend on `RecordingSession`.
3. A decoded video frame is retained when its source sample interval overlaps
   `[start, end)`. Its output timestamp and duration are clipped to that
   interval. This makes the frame covering `start` begin at output time zero and
   allows arbitrary starts without claiming compressed-keyframe precision.
4. Decoded audio is clipped at whole PCM-frame boundaries. Frames beginning
   before `start` or at/after `end` are removed; the first retained frame can be
   later than zero by less than one PCM frame. Re-encoding may add normal AAC
   codec-frame padding/granularity. Tests allow at most 100 ms for final A/V
   endpoint alignment, which is deliberately wider than one 1024-frame AAC
   interval at 44.1/48 kHz.
5. Both streams use the requested `start` as their common output origin. Source
   gaps are preserved, timestamp rounding is kept monotonic, and the MP4 is a
   standalone timeline rather than a segment that still begins at source time.
6. Stream presence, source dimensions, and source frame rate are preserved.
   `RecordingOptions` supplies output encoding settings; its audio routing value
   does not remove an audio stream that is already present in the source.
7. Output is finalized to a unique sibling staging `.mp4`. Only after
   finalization and the final progress callback succeed is it moved to the
   destination. Replacement is allowed only when the terminal Save flow passes
   explicit overwrite approval; otherwise an existing destination is rejected.
   Failure, cancellation, or a publication race removes the staging file. The
   source is never opened for writing.
8. The synchronous exporter boundary performs COM and source-reader work on a
   joined MTA worker, while `MediaFoundationWriter` retains its own MTA worker.
   `TrimExportController` calls the exporter through background orchestration
   (ADR-010) so the Win32 UI thread does not wait for the synchronous operation.
9. `MfD3dDevice` is the small reusable media-layer owner for the D3D11 device and
   DXGI device manager. The trim exporter owns selection/fallback policy; future
   media features may reuse the device capability without depending on trim UI.
   Privacy-safe diagnostics report the selected path and adapter plus source
   open, decode/process/encode, finalize, publication, and total durations.

## Consequences

- Arbitrary trim bounds are correct without relying on source keyframe placement.
- Video-only and H.264/AAC Easnap recordings share the same implementation and
  preserve their stream topology.
- Memory usage is proportional to frame/sample size, not recording duration.
- The D3D11/DXGI path removes RGB32 materialization, per-frame BGRA allocation
  and copying, alpha normalization, a second NV12 allocation, and scalar
  BGRA-to-NV12 conversion. The fallback retains the prior correctness path.
- A trim near the end still scans and decodes earlier media. This is a bounded-
  memory correctness choice; seek/keyframe optimization may be added later only
  if it preserves these semantics.
- MP4 size estimation returns `std::nullopt` until a reliable estimate is needed.
- Progress callbacks run on the export worker; returning `false` cancels and
  prevents destination publication.
