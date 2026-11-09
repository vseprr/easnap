# Easnap delivery roadmap

**Updated:** 2026-11-09

Read this with [project status](docs/project-status.md). This file records the
phase boundaries; what is implemented and validated belongs in project status,
and what each release contained in its release notes, listed in the
[documentation index](docs/README.md).

## Current: v1.3 on `feature/v1.3`

The first beta of v1.1 was published on 2026-10-13 as a prerelease of
[`vseprr/easnap`](https://github.com/vseprr/easnap): Scrollshot in the
application and the work a public beta needs. The second beta, with the
corrections from testing the first, was published on 2026-10-30. The
stabilization toward v1.1.0 ran on 2026-11-07 on the branch
`stabilize/v1.1.0`. Its release candidate v1.1.0-rc.1 was checked by the
owner and not published; the two problems the check found were corrected, and
v1.1.0 was published on 2026-11-08. v1.2 followed on the branch
`feature/v1.2`: Pin to screen and the sturdiness slices. Its release
candidate v1.2.0-rc.1 was checked by the owner and not published, and v1.2.0
was published on 2026-11-08 as the latest stable version. v1.3 runs on the
branch `feature/v1.3`, created on 2026-11-08 from `main` at `68e34c5`; its
first slice is S2 of the coordinator split.

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
| v1.1.0 stable | Stabilization on `stabilize/v1.1.0` (2026-11-07): audit, documentation, formatting, static analysis, code fixes; rc.1 checked by the owner, not published; v1.1.0 published 2026-11-08 |
| v1.2 | On `feature/v1.2` (from 2026-11-08): Pin to screen ([ADR-038](docs/decisions/ADR-038-pinned-images.md)), audio exception boundary, undo limit, clipboard formats first, the session state machine and a required clang-tidy job; release candidate rc.1 checked by the owner, not published; v1.2.0 published 2026-11-08 |
| v1.3 | On `feature/v1.3` (from 2026-11-08): S2 of the coordinator split, the trim flow with its own test ([ADR-039](docs/decisions/ADR-039-coordinator-trim-flow.md)), implemented and checked by the owner; recording stays in the coordinator. Then, on `feature/v1.3-findings` (2026-11-09): the 2026-11-07 audit findings closed in one pass (copy and save on a worker, recording on Desktop Duplication, crash report line, settings notice and small clean-ups, editor repaint of what a move changed, trim window minimum and text fit, test rename, function splits, documentation tidy-up), the Scrollshot realistic test application ([ADR-041](docs/decisions/ADR-041-scrollshot-realistic-test-application.md)) and the frame-cost measurement ([ADR-042](docs/decisions/ADR-042-recording-frame-buffer-cost.md)); the external video feature ([ADR-040](docs/decisions/ADR-040-external-video-in-the-trim-view.md)) is implemented (media layer, flow and tray entry, command line, hand-over to the running Easnap, installer "Open with"), its validation pending |
| OCR | Planned after v1; not implemented and no date committed |

## Next bounded slices

In this order (details in [project status](docs/project-status.md#next-slice)):

1. The external video feature on `feature/v1.3-findings`: its validation
   (lab run, installer "Open with", owner checklist of
   [ADR-040](docs/decisions/ADR-040-external-video-in-the-trim-view.md)).
2. The per-frame buffer decision
   ([ADR-042](docs/decisions/ADR-042-recording-frame-buffer-cost.md)), after the
   Soak figures.
3. The remaining editor costs (first spotlight frame, blur closure) and the
   Scrollshot fractional scroll offsets, which need an engine change and an
   ADR-027 amendment.
4. The owner's checks of the branch, then the other v1.3 candidates under
   [other deferred work](#other-deferred-work), one bounded slice each.
   Recording stays in the coordinator
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
owner decided on 2026-11-07 to publish v1.1.0 unsigned. Candidates from the
2026-11-07 audit left for v1.3 (v1.2 took the exception boundary for the audio
threads, the required clang-tidy job and the first slice of the coordinator
split; v1.3 began with the trim flow out of `CaptureCoordinator`): shorter
audio and editor functions, a golden corpus of recorded frames for Scrollshot, and copying and
saving off the interface thread; for the lab, an image at the development PC's
update revision, one of the two differences left that could explain why the lab
missed the caption the owner saw ([ADR-037](docs/decisions/ADR-037-isolated-test-lab.md#not-validated)). Codec
cut-point granularity, GIF palette/timing limits and source-start decoding for
late trims are documented implementation limits, not unimplemented v1 features.
