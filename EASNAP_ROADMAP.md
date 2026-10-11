# Easnap delivery roadmap

**Updated:** 2026-10-10

Read this with [project status](docs/project-status.md). This file records the
phase boundaries; validation details belong in project status and the linked
technical reports.

## Current: v1.0.1 maintenance and hardening

[`vseprr/easnap`](https://github.com/vseprr/easnap) v1.0.1 is a maintenance
release on top of v1.0.0. It contains defect fixes, security hardening,
display-change robustness, a per-user installer beside the portable ZIP, and a
restructured test suite. It adds no features and starts no new phase.

| Phase | State |
|---|---|
| Foundation | Implemented |
| Screenshots and annotations | Implemented; prior accepted UX corrections retained |
| MP4 recording | Implemented |
| GIF export and recording/trim integration | Implemented; Phase 3.3 accepted |
| Release hardening | v1.0.0 published; v1.0.1 maintenance release; broader physical validation remains |
| Scrollshot | Reserved for v1.1; not implemented |
| OCR | Planned after v1; not implemented and no date committed |

## Next bounded slice

Continue hardening before any feature work:

1. Done after v1.0.1: incremental Blur/Pixelate/Spotlight pixels, and MP4 trim
   of pictures the decoder pads. Both are on `main` and not yet released.
2. Reject a recording region the H.264 encoder cannot accept with a specific
   message before recording starts.
3. Investigate the remaining Trim redraw artifacts with reproducible desktop
   evidence.
4. Extend physical validation to mixed-DPI layouts, other scale factors, HDR
   displays, other GPUs and audio devices, and keyboard/accessibility
   workflows. The physical baseline is two 1920x1080 displays at 125 %.
5. Move the remaining visible-desktop tests off the user's desktop where a
   message-driven equivalent can observe the same behavior, and make all of
   them follow the chosen display.

Do not reopen the accepted September 21 correction list from historical logs
alone. New regressions require fresh evidence. Keep the existing native frame,
shared controls and asynchronous preview boundaries described in the
[window implementation guide](docs/ui/window-implementation-guide.md) and
[ADR-024](docs/decisions/ADR-024-native-trim-frame.md).

## Scrollshot: intended v1.1 scope

Detect a scrolling region, match overlapping captures, stitch the result,
validate the output and integrate it with screenshot editing. Keep this work
separate from recording and preview pipelines. Scheduling follows release
hardening; v1 does not contain a hidden or partial Scrollshot implementation.

## OCR: post-v1 direction

Extract selectable text from screenshots. Recognition languages, engine,
interaction and distribution requirements remain to be decided. OCR is not
included in v1 and has no promised release date.

## Other deferred work

Camera, Mouse Effects, device pickers, GPU-native recording, a single muxing
thread for audio and video, and long-video trim optimization remain outside
this release. Code signing is not in place; binaries are unsigned. Codec
cut-point granularity, GIF palette/timing limits and source-start decoding for
late trims are documented implementation limits, not unimplemented v1 features.
