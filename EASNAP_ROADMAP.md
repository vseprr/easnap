# Easnap delivery roadmap

**Updated:** 2026-09-30

Read this with [project status](docs/project-status.md). This file records the
phase boundaries; validation details belong in project status and the linked
technical reports.

## Current: v1 publication and release hardening

The initial public release baseline is **v1.0.0** for
[`vseprr/easnap`](https://github.com/vseprr/easnap). Distribution is a portable
Windows x64 ZIP containing the application and its matching preview helper.
Release work includes version metadata, checksums, English documentation,
reviewed product screenshots and public-repository privacy checks.

This publication work was requested on 30 September. It does not turn earlier
failed tests into passes or close the remaining physical compatibility matrix.
The Trim resize issue remains documented and unresolved.

| Phase | State |
|---|---|
| Foundation | Implemented |
| Screenshots and annotations | Implemented; prior accepted UX corrections retained |
| MP4 recording | Implemented |
| GIF export and recording/trim integration | Implemented; Phase 3.3 accepted |
| Release hardening | v1 package and publication materials prepared; broader validation remains |
| Scrollshot | Reserved for v1.1; not implemented |
| OCR | Planned after v1; not implemented and no date committed |

## Next bounded slice

Investigate the remaining Trim redraw artifacts with reproducible desktop
evidence. Resume the complete automated suite when requested; the publication
run was stopped after 29 of 37 tests had completed. Extend the release
matrix to additional Windows versions, GPUs, audio devices, mixed-DPI displays,
negative-origin/multiple-monitor layouts and keyboard/accessibility workflows.
The recorded local physical display baseline is 125% scaling.

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

Camera, Mouse Effects, device pickers, GPU-native recording and long-video trim
optimization remain outside this release. Codec cut-point granularity, GIF
palette/timing limits and source-start decoding for late trims are documented
implementation limits, not unimplemented v1 features.
