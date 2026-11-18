# Easnap delivery roadmap

**Updated:** 2026-11-18

Read this with [project status](docs/project-status.md). This file records the
phase boundaries; what is implemented and validated belongs in project status,
and what each release contained in its release notes, listed in the
[documentation index](docs/README.md).

## Current: v1.3.2 published

v1.3.2 (2026-11-18) is the latest stable release: the structure refactor
([ADR-043](docs/decisions/ADR-043-structure-for-cheap-changes.md), pull
request #27) as a maintenance release with no visible change. Every release
and its notes are listed in the [documentation index](docs/README.md).

| Phase | State |
|---|---|
| Foundation | Implemented |
| Screenshots and annotations | Implemented; prior accepted UX corrections retained |
| MP4 recording | Implemented |
| GIF export and recording/trim integration | Implemented |
| Release hardening | v1.0.0 published; v1.0.1 and v1.0.2 maintenance releases; broader physical validation remains |
| Scrollshot | Implemented, including editing of the stitched image; released in v1.1.0-beta.1 |
| Beta readiness | Implemented; v1.1.0-beta.1 and v1.1.0-beta.2 published as prereleases |
| Isolated test lab | Implemented 2026-10-19: Hyper-V VM driven by winlab, direct build, capture-path reporting ([ADR-037](docs/decisions/ADR-037-isolated-test-lab.md)) |
| v1.1.0 stable | Published 2026-11-08 after stabilization on `stabilize/v1.1.0`; [notes](docs/releases/v1.1.0.md) |
| v1.2 | Published 2026-11-08: Pin to screen ([ADR-038](docs/decisions/ADR-038-pinned-images.md)) and the first slice of the coordinator split ([ADR-039](docs/decisions/ADR-039-coordinator-trim-flow.md)); [notes](docs/releases/v1.2.0.md) |
| v1.3 | Published 2026-11-09: the trim flow ([ADR-039](docs/decisions/ADR-039-coordinator-trim-flow.md)), external videos in the trim view ([ADR-040](docs/decisions/ADR-040-external-video-in-the-trim-view.md)), the Scrollshot realistic test application ([ADR-041](docs/decisions/ADR-041-scrollshot-realistic-test-application.md)), the recording's picture pool ([ADR-042](docs/decisions/ADR-042-recording-frame-buffer-cost.md)), the preview clock check ([ADR-010](docs/decisions/ADR-010-trim-view-ownership-and-orchestration.md), amended); [notes](docs/releases/v1.3.0.md) |
| v1.3.1 | Published 2026-11-17: no flash when a screenshot is fixed, the watermark field, no pointer flicker while typing; [notes](docs/releases/v1.3.1.md) |
| v1.3.2 | Published 2026-11-18: the structure refactor as a maintenance release, no visible change; [notes](docs/releases/v1.3.2.md) |
| Structure refactor | Merged (PR #27): layer order checked, one message table and one identifier header, platform and editor split into units, one table row per button, theme-owned shared values, window class helper, shorter audio and media thread functions ([ADR-043](docs/decisions/ADR-043-structure-for-cheap-changes.md), [project status](docs/project-status.md#structure-refactor-branch-refactorstructure)) |
| OCR | Planned after v1; not implemented and no date committed |

## Next bounded slices

Open; the owner will set the order (details in
[project status](docs/project-status.md#next-slice)):

1. The Scrollshot engine amendment for pictures torn between two presents
   ([ADR-027](docs/decisions/ADR-027-scrollshot-stitching-engine.md#known-weakness-2026-11-09)),
   seen in the lab; until then `scrollshot_realistic_desktop` reports torn
   input as `KNOWN_WEAKNESS`
   ([ADR-041](docs/decisions/ADR-041-scrollshot-realistic-test-application.md#validation)).
2. The remaining editor costs (first spotlight frame, blur closure).
3. The files still large after the structure refactor.
4. Notifications turned off in winlab's guest provisioning.

Then the deferred candidates under [other deferred work](#other-deferred-work),
one bounded slice each; the Scrollshot fractional scroll offsets need an
engine change of their own. Recording stays in the coordinator
([ADR-039](docs/decisions/ADR-039-coordinator-trim-flow.md#recording-decided-after-s2)).

Open on the owner's side: the host Full run for the writer fix, since the host
desktop is used only when the owner asks. Not covered yet: Windows 11, an N
edition, Scrollshot on real applications. `lab.ps1 -Variant` names further lab
images built from `testenv.json`; the first one runs at 150 % scale. A
Windows 11 image needs its ISO and virtual TPM support in winlab.

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
this release. Code signing is not in place; binaries are unsigned, and the
owner decided on 2026-11-07 to publish v1.1.0 unsigned. Left from the
2026-11-07 audit: a golden corpus of recorded frames for Scrollshot; for the
lab, an image at the development PC's update revision, one of the two
differences left that could explain why the lab missed the caption the owner
saw ([ADR-037](docs/decisions/ADR-037-isolated-test-lab.md#not-validated)). Codec
cut-point granularity, GIF palette/timing limits and source-start decoding for
late trims are documented implementation limits, not unimplemented v1 features.
