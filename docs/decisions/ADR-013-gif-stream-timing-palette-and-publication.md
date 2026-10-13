# ADR-013 — GIF stream, timing, palette, delta, and publication contracts

**Status:** Accepted  
**Date:** 2026-09-03

## Context

Phase 3 introduces animated GIF as a second recording output. GIF stores frame
delay in integer centiseconds, has at most 256 palette entries, and provides no
audio stream. A production backend must preserve active media time without
duration-dependent memory, avoid palette flicker, omit redundant full-canvas
frames, and never expose a partial destination.

The safe recording artifact already produced by Easnap is a finalized temporary
H.264 MP4. The shared workflow uses it as the GIF export source (ADR-014), while
keeping palette, timing and GIF container logic independent of Media Foundation
and recording windows. The second format therefore shares the capture architecture.

Windows Imaging Component was evaluated as the encoder. Its decoder is valuable
for independent validation, but delegating GIF encoding to WIC does not give the
backend sufficiently explicit control over one temporally stable palette,
cumulative delay quantization, delta rectangle choice, LZW output, and disposal
metadata. A heavyweight third-party codec is not justified for this bounded
format writer.

## Decision

1. `IGifFrameSource` is the rewindable source boundary. It yields sequential
   immutable `VideoFrame` values in Easnap's existing opaque BGRA format and
   100-nanosecond active-time domain. `gif_encoder` depends on this contract,
   never on MP4, capture sessions, HWNDs, or UI state.
2. GIF export uses deterministic sequential passes and never retains the
   animation or all decoded frames. High quality builds a bounded
   5-bit-per-channel histogram, sampling at most 65,536 pixels per adapted
   frame, and derives one global palette with weighted median-cut boxes. Low
   size uses the fixed global palette described below. Since 2026-09-23 it skips
   histogram analysis and validates/adapts/encodes in one source pass. Adaptive
   High quality still needs two passes. Low progress spans its one encoding pass;
   a late invalid frame discards staging before any destination replacement.
3. The global palette remains fixed for the entire animation, preventing
   palette remapping and flicker between frames. `Sharp` nearest-color mapping
   favors UI/text edges. High quality's `Balanced` policy adds deterministic
   4×4 canvas-anchored ordered dithering. Low size uses a deterministic
   fixed 6x6x6 RGB cube (channel steps of 51), extended with the ten
   nonduplicate neutral levels from a 16-step grayscale ramp: 226 colors in a
   256-slot global table. Its restrained canvas-anchored Balanced dither keeps
   grays neutral and reduces pattern entropy. The legacy RGB332 policy remains
   available internally for explicit callers and regression comparisons.
   Since the 2026-09-23 approved repair, Low Size also keeps one bounded BGRA
   anchor canvas. Each pixel updates only when at least one source channel
   differs from its last accepted value by more than 8/255. Small H.264 noise
   therefore does not repeatedly flip palette indices. Changes accumulate
   against the accepted anchor, not against the immediately preceding input,
   so slow fades eventually update. This is an explicit Low Size fidelity
   tradeoff; changes entirely within that deadband can be suppressed. High
   Quality has no temporal deadband and retains its previous mapping/output.
4. Output dimensions, maximum frame rate, palette size, and quantization policy
   are backend options. A zero dimension preserves or derives aspect ratio; two
   explicit dimensions are an explicit output-size contract. Frame-rate values
   are limited to 1–100 because broadly interoperable GIF playback cannot
   represent positive sub-centisecond frame intervals reliably.
5. Delay conversion rounds cumulative active media time to the nearest
   centisecond and subtracts already emitted centiseconds. It never rounds each
   frame independently, so error does not accumulate. High quality retains its
   original 15 fps coalescing policy. Low size distributes source intervals
   against cumulative 12 fps deadlines, preserving total time and the first and,
   where representable, final visuals. Delays above 65,535 centiseconds are
   split into bounded duration-only 1×1 frames.
6. The first encoded frame covers the full logical canvas. Later frames use the
   smallest rectangle whose global-palette indices changed. Quantized-identical
   frames are omitted and their elapsed duration is carried by the pending
   frame. High quality retains opaque deltas byte-for-byte. Low size selects,
   per delta, a palette index unused by the genuinely changed visible pixels;
   unchanged positions inside the bounding rectangle use that index and the
   frame declares it transparent. If all palette indices are required by changes,
   Low falls back to the correct opaque rectangle. The first frame is always a
   complete opaque canvas. All frames use disposal method 1 (leave in place),
   so transparent positions preserve the reconstructed prior canvas and changed
   pixels remain visible. Low duration-only physical frames are 1×1 transparent
   frames and therefore do not alter the canvas.
7. Easnap writes GIF89a directly: logical screen descriptor, global color table,
   `NETSCAPE2.0` loop extension (loop count zero means infinite), graphic
   controls, image descriptors, bounded 12-bit GIF LZW sub-blocks, and a trailer.
   WIC independently decodes produced files in automated tests.
8. Encoder memory is bounded by the fixed 32,768-bin histogram, palette/lookup
   tables, a small fixed number of scaled/indexed frames, one pending delta, and
   a 4,096-entry LZW dictionary. It is proportional to output dimensions, not
   recording duration.
9. `ExportGif` writes a unique sibling `.partial.gif`, flushes and closes it,
   observes final cancellation, and only then publishes with `MoveFileExW`.
   Invalid input, source/quantizer/encoder/write failure, cancellation, or a
   publication race removes staging and preserves both source and any existing
   unapproved destination. Replacement requires explicit approval.
10. `CreateGifExporter()` adapts finalized H.264 MP4 intervals to
    `IGifFrameSource` through a bounded Media Foundation Source Reader RGB32
    path. Audio streams are not selected. The reader is reopened for each pass
    and retains only pending/current decoded frames. The adapter runs on a joined
    MTA worker and exposes the existing `ITrimExporter` progress/cancellation
    contract.
11. GIF quantization is CPU work and requires BGRA pixels, so the MP4 adapter
    does not force the NV12 DXGI zero-copy path used by MP4 re-encoding.
    `IGifFrameSource` keeps decode/scale replaceable: a later measured GPU
    conversion optimization may reuse `MfD3dDevice` without changing the GIF
    encoder or Phase 3.2 presentation/configuration model.

## Consequences

- The shared recording flow uses the finalized MP4 and `ITrimExporter` orchestration
  without creating another capture backend or changing GIF media logic.
- Adaptive global palette quality requires two bounded sequential decodes;
  the fixed Low size palette requires one. Neither retains the entire animation.
- Ordered dithering and median-cut quantization are deterministic and
  vendor-neutral, but GIF remains limited to 256 colors and centisecond timing.
- On the accepted deterministic desktop fixture, clearing LZW at 2,048 codes
  increased Low output from 11,172 to 11,333 bytes. The bounded 4,096-code
  full-dictionary reset therefore remains the selected policy.
- The fixed 226-color palette and the temporal deadband replaced RGB332 for
  Low size on 2026-09-23, with a target of about 42,000 bytes for the reported
  static desktop workload. On the same 364 × 490 inputs, RGB332 and the current
  policy produced 76,773 and 40,835 bytes (3.33 s decoded sequence) and 65,407
  and 39,404 bytes (3.00 s native-writer MP4); the production MP4-to-GIF
  pipeline produced 40,044 bytes. On that MP4 the first-frame mean absolute RGB error
  fell from 19.10 to 6.85 and PSNR rose from 20.35 to 29.20 dB. Savings depend
  on content.
- GIF has no audio. The configuration derives capability visibility from the
  selected recording format; its effective GIF audio source is None (ADR-014).
- The configuration maps the durable backend policies to High quality and Low size
  product presets; the UI does not expose the technical controls individually.
