# Easnap delivery roadmap

**Updated:** 2026-10-13

Read this with [project status](docs/project-status.md). This file records the
phase boundaries; validation details belong in project status and the linked
technical reports.

## Current: v1.1.0-beta.1 preparation

The branch `feature/v1.1-beta` prepares the first beta of v1.1: Scrollshot in
the application, then the work a public beta needs. Nothing from it has been
released; the published version is v1.0.2.

[`vseprr/easnap`](https://github.com/vseprr/easnap) v1.0.2 is the second
maintenance release on top of v1.0.0. v1.0.1 brought defect fixes, security
hardening, display-change robustness, a per-user installer beside the portable
ZIP, and a restructured test suite. v1.0.2 adds incremental redaction rendering
in the editor and a fix for MP4 trims of pictures the decoder pads. Neither
adds features or starts a new phase.

| Phase | State |
|---|---|
| Foundation | Implemented |
| Screenshots and annotations | Implemented; prior accepted UX corrections retained |
| MP4 recording | Implemented |
| GIF export and recording/trim integration | Implemented; Phase 3.3 accepted |
| Release hardening | v1.0.0 published; v1.0.1 and v1.0.2 maintenance releases; broader physical validation remains |
| Scrollshot | Implemented on `feature/v1.1-beta`, including editing of the stitched image; not released |
| Beta readiness | In progress on `feature/v1.1-beta` |
| OCR | Planned after v1; not implemented and no date committed |

## Next bounded slices

For v1.1.0-beta.1, in this order:

1. Done: Scrollshot stitching engine, capture session, workspace integration
   and editing of the stitched image.
2. Diagnostics and data safety: log file, crash dumps, confirmation before
   work is lost, recovery of unfinished recordings, specific error messages,
   including a recording region the H.264 encoder cannot accept.
3. The remaining user-visible text in English and Turkish.
4. Settings window, About, update check, launch behaviour.
5. Compatibility: Windows N editions, the real minimum Windows version.
6. Release and distribution: prerelease versioning, package manifests,
   uninstall behaviour.
7. CI and community files.
8. Documentation pass and validation.

Carried over from v1.0.2 and not part of the beta:

- Investigate the remaining Trim redraw artifacts with reproducible desktop
  evidence.
- Extend physical validation to mixed-DPI layouts, other scale factors, HDR
  displays, other GPUs and audio devices, and keyboard/accessibility
  workflows. The physical baseline is two 1920x1080 displays at 125 %.
- Move the remaining visible-desktop tests off the user's desktop where a
  message-driven equivalent can observe the same behavior, and make all of
  them follow the chosen display.

Do not reopen the accepted September 21 correction list from historical logs
alone. New regressions require fresh evidence. Keep the existing native frame,
shared controls and asynchronous preview boundaries described in the
[window implementation guide](docs/ui/window-implementation-guide.md) and
[ADR-024](docs/decisions/ADR-024-native-trim-frame.md).

## Scrollshot: v1.1 scope

Implemented: a manual, vertical scrolling capture of a fixed frame, stitched
without guessing and editable with the screenshot tools. Not in v1.1:
automatic scrolling, horizontal capture and a dedicated shortcut.

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
