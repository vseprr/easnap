# ADR-014 — Unified recording configuration and final-format completion

**Status:** Accepted  
**Date:** 2026-09-03

## Context

GIF is Easnap's first recording output beyond MP4. Treating it as a separate
dialog, capture loop, pause clock, trim window, or completion controller would
duplicate accepted production behavior and couple the future custom UI to media
implementation details. GIF has no audio and always requires conversion from the
finalized working recording, including for a full-range selection.

## Decision

1. `RecordingConfiguration` is the application-level, presentation-independent
   source of truth. It contains `RecordingFormat`, the retained MP4 audio
   preference, cursor inclusion, and product-level GIF export configuration.
2. `RecordingCapabilities` determines which semantic controls are valid. MP4
   supports all four accepted audio modes and cursor capture. GIF supports cursor
   capture, GIF quality selection and final media conversion, but not audio. The
   native Win32 surface consumes this policy: MP4 hides GIF quality, while GIF
   hides audio and shows High quality/Low size radio choices.
3. Switching formats never destroys the MP4 audio preference or GIF preset. The
   effective GIF audio source is always `None`; invalid or corrupt persisted
   format/preset values fall back independently. `GifExportPreset` is a product
   enum with `HighQuality` as the default and `LowSize` as the alternative.
   Legacy technical GIF settings are not reinterpreted as the product enum;
   absence of the new preset key migrates unambiguously to High quality.
4. MP4 and GIF use the same `RecordingSession`, session states, region capture,
   active-time pause/resume semantics, finalizer, preview and `[in, out)` range.
   The session carries the selected format but always writes the accepted H.264
   MP4 working artifact. GIF configurations with a non-None effective audio source
   are rejected, and no WASAPI provider is created for GIF.
5. The final format selects one existing `ITrimExporter`: MP4 trim export or the
   Phase 3.1 GIF exporter. MP4 exact-full-range Save/Copy keeps its existing reuse
   fast path. GIF exact-full-range and interior selections both invoke exactly one
   GIF export because the source is MP4.
6. Save exports a new GIF directly through its sibling staging file to the
   approved permanent `.gif` destination. Copy exports directly to a prepared
   `.gif` clipboard-publication path, applies the established current-user and
   LocalSystem DACL, and publishes Unicode `CF_HDROP`; raw bytes are never placed
   on the clipboard.
7. `RecordingCompletionController` generalizes file validation, staging, move,
   persistence and clipboard lifetime by final format. Its retry, cancellation,
   source preservation, clipboard replacement and next-launch cleanup ownership
   are unchanged.
8. Phase 3.2 exposes no technical GIF encoder controls. High quality preserves
   the shipped behavior: original dimensions, a 15 fps maximum, adaptive global
   palette, infinite looping, `Balanced` ordered dithering, opaque deltas and the
   existing timing. Low size maps to original dimensions, cumulative distributed
   12 fps cadence, the fixed 226-color palette with `Balanced` dithering and
   the 8/255 temporal deadband defined in ADR-013, transparent sparse deltas
   with safe opaque fallback, and infinite looping. Encoder policies remain
   internal rather than overloading the product preset meaning.
9. Terminal export failure is a durable structured contract, not a boolean-only
   UI boundary. `ITrimExporter` reports a stage, technical detail and HRESULT
   when available; asynchronous trim snapshots retain it for format-specific UI
   messaging and diagnostic logging. Cancellation remains a separate outcome.
   This does not change MP4 export behavior or source/result ownership.

## Consequences

- The session state graph is unchanged; format changes media behavior, not
  lifecycle structure.
- The current conservative Win32 panel can later be replaced while preserving
  the configuration, capability, capture, export and completion contracts.
- GIF inherits the tested recording timeline and preview behavior and pays one
  required conversion at terminal completion: two decoding passes for High
  quality, one for Low size (ADR-013).
- GIF remains subject to the inherent 256-color and centisecond-timing limits
  documented by ADR-013.
- A failed GIF terminal action can identify source open/validation, media-type
  negotiation, decode/frame access, second-pass reset, palette/encoding,
  staging/finalization, or publication while retaining the canonical MP4 for a
  retry.
