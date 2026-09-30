# Text, trim and recording UI correction validation

Updated: 2026-09-21. **Session resolved and accepted by the user.**
The final review closes all text, trim/window, preview, recording-island and
test-timeout issues raised in this session. It supersedes earlier pending-review
notes. Phase 3.3 remains closed; Phase 4 has not started.

Workspace availability checked on 2026-09-23: the historical build trees,
accepted executable pair and manifests referenced below were already absent
from the supplied source workspace. Their recorded outcomes remain historical;
the optimization task did not delete them. See [current status](../project-status.md).

## Accepted behavior

- Native text input keeps its visible EDIT, caret, selection, clipboard and undo.
  Synchronous native glyph/background output is clipped; the source-backed
  custom painter renders new/reopened Plain, Fill and Outline text.
- Existing text selects on a single click for dragging. A blank click clears
  a selection without creating text, regardless of earlier box switches.
  A blank click with neither editor nor selection creates text; double-click
  enters editing. Blank double-click continuation does not create another box.
- Trim uses the native side/bottom resize frame and shared custom caption,
  a DPI-aware 480 x 400 client-DIP minimum, batched child layout, themed backing
  surfaces and owner-thread painting. The playhead has restrained coral glow.
- Preview opens on its first decoded frame paused at zero. Helper resize stays
  asynchronous and coalesced; delayed polling has bounded fresh-snapshot grace.
- Pause/Resume is icon-only with the existing material and state colors. The
  compact recording island has no vertical separators and centers elapsed time.
- Export retains its rounded coral actual-progress fill without duplicate sheen.
- Transport no longer repeats the dedicated startup suite. Startup waits for
  actual desktop samples while checking paused state; the workspace DPI test
  checks containment in the suggested rectangle's destination monitor.

## Executed checks

| Check | Actual result | Evidence under `build-ui-origin-resize-20260921/` |
|---|---|---|
| Final full Release CTest | 26/26 passed, 438.50 s | `release-verified-ctest.log` |
| Affected Debug CTest | 7/7 across the listed runs | `debug-unit-ctest.log`, `debug-corrected-ctest.log`, `debug-ui-verified-ctest.log` |
| Final Debug UI/workspace/annotation group | 3/3 passed, 65.93 s | `debug-ui-verified-ctest.log` |
| Presented startup repetition | 5 consecutive Release passes, 38.29 s | `release-startup-ctest.log` |
| Delayed owner polling | 5 consecutive Debug passes, 41.09 s | `debug-feedback-stress.log` |
| Shared material parity | 768 cases, hash `527290a4517b5712`; 891 ms before / 141 ms after | Material logs and `ui_material_raster_parity` |
| All-edge desktop observation | Both themes, eight edges/corners, maximize/restore; 4,065 drag frames and 363 clean settled samples | `verified-edges.log`, `verified-evidence/` |
| Final product review | User accepted and closed all session issues | 2026-09-21 session closure |

The automated runs used Windows 10 build 19045, native 120 DPI (125%) and a
two-monitor desktop. Release and affected Debug targets built with warnings
treated as errors. The opt-in all-edge observer uses actual Windows Graphics
Capture monitor frames and real pointer drags. The outcomes above are the
historical execution record; product acceptance is a separate recorded result.
The documentation-only closure did not rerun these tests.

## Package and durable references

The accepted pair was `build-ui-origin-resize-20260921/portable/bin/Easnap.exe`
with adjacent `EasnapPreviewHost.exe`. The session recorded matching hashes in
`review-manifest.json` and `verification-results.json`. That archived directory
is absent from the workspace as checked on 2026-09-29; these are historical
paths, not currently available comparison binaries. This does not invalidate
the recorded tests or acceptance. At that session's closure, the running
`build/bin/Release` installation had not been overwritten.

New window work must follow the [window implementation guide](window-implementation-guide.md)
and [ADR-024](../decisions/ADR-024-native-trim-frame.md). Text/input and layout
decisions are in [ADR-022](../decisions/ADR-022-text-input-and-content-sized-trim.md)
and [ADR-023](../decisions/ADR-023-trim-resize-and-text-click-continuations.md).

Earlier evidence was recorded under `build-ui-bugfix-20260921/`,
`build-ui-resize-followup-20260921/`, `build-trim-resize-repair/` and
`build-text-trim-review/`. Those snapshots describe their own revisions and
must not replace current source or reopen this accepted session.
Additional device/display/accessibility release coverage belongs to Phase 4.
