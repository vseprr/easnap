# Easnap delivery roadmap

**Updated:** 2026-11-07

Read this with [project status](docs/project-status.md). This file records the
phase boundaries; what is implemented and validated belongs in project status,
and what each release contained in its release notes, listed in the
[documentation index](docs/README.md).

## Current: v1.1.0-beta.2 published, stabilization toward v1.1.0 under way

The first beta of v1.1 was published on 2026-10-13 as a prerelease of
[`vseprr/easnap`](https://github.com/vseprr/easnap): Scrollshot in the
application and the work a public beta needs. The second beta, with the
corrections from testing the first, was published on 2026-10-30. The
stabilization toward v1.1.0 started on 2026-11-07 on the branch
`stabilize/v1.1.0`. The latest stable version is v1.0.2, the second
maintenance release on top of v1.0.0.

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
| v1.1.0 stable | Stabilization on `stabilize/v1.1.0` (2026-11-07): audit, documentation, formatting, code fixes; then rc.1 |
| OCR | Planned after v1; not implemented and no date committed |

## Next bounded slices

On `stabilize/v1.1.0`, in this order (details in
[project status](docs/project-status.md#next-slice)):

1. Documentation pass.
2. Formatting and static analysis.
3. Code fixes from the 2026-11-07 audit, and the hosted-runner Build failures.
4. v1.1.0-rc.1: version label, release notes, Full and Soak runs in the lab,
   the owner's manual checklist.
5. The owner's results with rc.1, then v1.1.0.

Open on the owner's side: the host Full run for the writer fix, since the host
desktop is used only when the owner asks. The checks the betas could not make
on one PC remain: Windows 11, an N edition, Scrollshot on real applications,
other scale factors, the installer. The lab can take Windows 11 and N-edition
images once a second `testenv.json` names them.

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
