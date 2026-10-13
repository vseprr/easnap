# Easnap delivery roadmap

**Updated:** 2026-10-13

Read this with [project status](docs/project-status.md). This file records the
phase boundaries; what is implemented and validated belongs in project status,
and what each release contained in its
[release notes](docs/releases/v1.1.0-beta.1.md).

## Current: v1.1.0-beta.1, published

The first beta of v1.1 was published on 2026-10-13 as a prerelease of
[`vseprr/easnap`](https://github.com/vseprr/easnap): Scrollshot in the
application and the work a public beta needs. The latest stable version is
v1.0.2, the second maintenance release on top of v1.0.0.

| Phase | State |
|---|---|
| Foundation | Implemented |
| Screenshots and annotations | Implemented; prior accepted UX corrections retained |
| MP4 recording | Implemented |
| GIF export and recording/trim integration | Implemented |
| Release hardening | v1.0.0 published; v1.0.1 and v1.0.2 maintenance releases; broader physical validation remains |
| Scrollshot | Implemented, including editing of the stitched image; released in v1.1.0-beta.1 |
| Beta readiness | Implemented; v1.1.0-beta.1 published as a prerelease |
| OCR | Planned after v1; not implemented and no date committed |

## Next bounded slices

For v1.1.0-beta.1, in this order:

1. Done: Scrollshot stitching engine, capture session, workspace integration
   and editing of the stitched image.
2. Done: diagnostics and data safety. Log file, crash reports, confirmation
   before work is lost, recovery of unsaved recordings, specific recording
   start messages, and the H.264 picture-size rule.
3. Done: all user-visible text in English and Turkish.
4. Done: Settings window, About, update check, launch behaviour.
5. Done: compatibility. Easnap starts on Windows N editions and refuses to
   start below Windows 10 build 19041. Neither was seen on such a system.
6. Done: release and distribution. Package names and checks for the
   prerelease version, package manifests, uninstall behaviour, symbol files,
   release notes. Published from the tag by the Release workflow; the package
   manager submissions are the owner's step.
7. Done: CI and community files. CodeQL, an AddressSanitizer job, issue forms
   and a pull-request template; skipped tests are reported as skipped. Both
   new jobs pass on the hosted runner.
8. Done: documentation pass and validation. Thirteen closed reports were
   removed after their conclusions moved into the ADRs they support; the
   Full tier passed 45 of 45 on 2026-10-13.

Next: the checks the beta could not make on one PC: Windows 11, an N edition, Scrollshot on real
applications, other scale factors, the installer.

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
