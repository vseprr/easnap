# Neumorphic UI and editor implementation report

Workspace availability rechecked 2026-09-29: the historical build tree is absent.
Its artifact paths are retained as archival references, not download links.
Recorded results below remain historical; see [current status](../project-status.md).

Historical implementation/test evidence. The current accepted UI baseline and
2026-09-21 session closure are in [project status](../project-status.md) and
[session validation](text-trim-validation.md). Current materials and window rules
are in the [design system](design-system.md) and [window implementation guide](window-implementation-guide.md).

2026-09-16. Implements the supplied `easnap-ui-overhaul-prompt.en.md` against the
existing Windows application. This is the separately authorized UI/editor slice;
Phase 3.3 remains accepted and Phase 4 has not started.

The subsequent interaction correction slice and its current validation are in
[the 2026-09-17 report](interaction-corrections-validation.md). Results below
are the earlier overhaul baseline.

## Implemented

- Shared solid Light/graphite panels, soft raised/recessed material, antialiased
  vector icons, independent focus/selection/disabled states and retargetable
  local motion respecting reduced animation/high contrast. Native input roles
  remain. Recording, trim, timeline, progress, menus and application dialogs use
  the shared presentation layer; theme buttons toggle immediately and persist.
- Slim icon island plus a separate contextual island, frame-aware placement,
  blank/grip dragging, horizontal mode-strip dragging and resize/capture-loss
  hiding/restoration. Existing acquisition and media ownership remain.
- Persistent annotations with hit testing, selection, movement, type-specific
  handles, separate creation defaults, transaction-based undo/redo and rollback.
  Text has native caret/clipboard/multiline editing, word/paragraph selection,
  whole-object point sizes and Plain/Fill/glyph Outline. Tags retain stable
  content colors and proportional label/dot/notch geometry.
- Brush/region blur and pixelation rebuilt from the immutable source, adjustable
  strength, union spotlight regions with shared outside opacity, and repeated
  angled watermark text with color/opacity/size. Preview/export share rendering;
  editor selection frames never export.

Design provenance and chosen numeric values are in the [design system](design-system.md)
and [ADR-017](../decisions/ADR-017-neumorphic-editor-and-editable-annotations.md).
No exact source-to-DIP mapping or source-derived Dark palette is claimed.

## Validation performed

Windows 10 build 19045, MSVC 19.44, Windows SDK 10.0.26100, x64. Release and the
affected Debug application/test targets build with warnings treated as errors.

| Check | Actual result |
|---|---|
| Final full Release CTest run, parallelism 2 | **21/22 passed**, 181.32 s; unchanged `mfplay_preview_transport` failed its natural-end wait |
| Isolated retry of that MFPlay test | **Passed**, 181.35 s; no preview/backend code changed |
| Affected Debug session/trim/selection/magnifier/overlay/workspace/frame/UI/editor tests | **9/9 passed**, 16.68 s |
| Final editor/capture repaint checks after cursor review | Release **3/3 passed**, 6.60 s; Debug **3/3 passed**, 7.89 s |
| Final real native-window evidence runs | General UI fixture and editor fixture each **0 failures** |
| Contrast | Light: text 10.97, secondary 5.50, primary text 5.39, boundary 3.18; Dark: 12.07 / 6.93 / 5.48 / 3.43 |
| 100 alternating theme switches | Final evidence run GDI objects **20 → 20**, identical composited pixels |

The full run is not reported as an uninterrupted 22/22 pass. MFPlay passed both
an earlier full run and the isolated retry, but its intermittent natural-end
failure remains a validation risk, without a claimed root cause. Initial media
tests denied normal LocalAppData temporary storage passed when run with normal
access. No storage protections or media assertions were weakened.

The new native editor gate exercises production document transactions, rollback,
native text entry, selected text with whole-object font changes, text bounds,
endpoint moves, effect-region moves without baked remnants, pixelation/blur
changes, one-step slider undo, multiple spotlights, watermark output, theme
invariance, synthetic DPI messages and frame-resize island hiding. Existing
workspace/clipboard tests use the production compositor. The existing damage
test caught an antialiased size-label fringe outside its dirty rectangle; the
production damage bounds now include that pixel and partial/full renders agree.

Reproduction commands, from the repository root:

```powershell
cmake --build build-ui-validation --config Release -j 4
ctest --test-dir build-ui-validation -C Release --output-on-failure -j 2
ctest --test-dir build-ui-validation -C Release -R '^mfplay_preview_transport$' --output-on-failure
build-ui-validation/bin/Release/test_ui_modernization.exe --editor-checks build-neumorphic/editor-evidence
build-ui-validation/bin/Release/test_ui_modernization.exe --evidence build-neumorphic/final-surfaces
cmake --install build-ui-validation --config Release --prefix build-neumorphic/portable
```

Logs: full Release (`build-neumorphic/release-verified.log`, historical artifact),
isolated preview (`build-neumorphic/preview-isolated-retry.log`, historical artifact),
affected Debug (`build-neumorphic/debug-affected.log`, historical artifact),
final editor Release (`build-neumorphic/final-editor-release.log`, historical artifact),
final editor Debug (`build-neumorphic/final-editor-debug.log`, historical artifact).
At delivery, the portable directory contained Easnap and EasnapPreviewHost;
each installed binary had the same SHA-256 as its Release build output.

## Actual rendered evidence

Light capture sheet (`build-neumorphic/easnap-light.png`, historical artifact) ·
Dark capture sheet (`build-neumorphic/easnap-dark.png`, historical artifact)

These sheets arrange unscaled captures of actual production HWNDs; captions are
added outside the captures. They are not design mockups. Native windows are
captured through PrintWindow; child controls through WM_PRINTCLIENT. Evidence
runs create real editor, recording, trim and MessageBox windows. The screenshot
content fixture is a deterministic checkerboard; trim uses an encoded H.264
fixture. Black preview areas reflect PrintWindow's inability to capture the
separate video renderer, not evidence of playback failure or capture exclusion.

- Light text at 200% (`build-neumorphic/inspection-200-light.png`, historical artifact) and
  Dark text at 200% (`build-neumorphic/inspection-200-dark.png`, historical artifact) are enlarged
  native captures for pixel inspection, not physical 200% display-scaling tests.
- Annotation export (`build-neumorphic/editor-evidence/annotations-export.png`, historical artifact),
  white-outline text (`build-neumorphic/editor-evidence/text-outline-white-export.png`, historical artifact),
  spotlight and watermark export (`build-neumorphic/editor-evidence/spotlight-watermark-export.png`, historical artifact).
- Button rest/hover/pressed/focus/disabled, Light/Dark dialogs and contextual
  controls are under `build-neumorphic/editor-evidence/`. MP4/GIF, active/paused,
  finalizing, mode strip, compact trim and error/progress captures are under
  `build-neumorphic/final-surfaces/`. These were inspected at native size, with
  representative text/control edges additionally inspected at 200%.

## Concrete limitations and next manual checks

The native computer-control connection failed twice with “native pipe is
unavailable.” Therefore there is **no live pointer-driven review or interaction
recording** from this session. Native fixture messages and rendered captures do
not establish animation smoothness, every keyboard path, live caret hit accuracy,
menu popup interaction or screen-reader usability.

1. In both themes, review rapid hover/press/release, retargeting, focus and reduced
   animation; drag sliders and trim handles, and exercise the tray submenu.
2. Double/triple-click text at multiple offsets; edit with clipboard, selections,
   IME and multiple paragraphs; change whole-object size while a range is selected.
   Move each object type, resize every handle direction, and cancel with Escape
   and lost capture. Verify deliberate blank-content quick-copy and the retained
   one-shot quick-return-on-commit behavior after a mode round trip.
3. On physical 100/125/150/200% displays, move islands between monitors and near
   every work-area edge, including negative origins; resize the provisional frame
   and test compact trim. Current DPI coverage uses 96/120/144/192 messages and
   geometry assertions, not four physical desktop configurations.
4. Verify Windows 11, live high contrast, screen readers, additional audio/GPU
   devices, external clipboard destinations and capture exclusion. Profile large
   screenshots with many CPU blur regions. Investigate MFPlay natural-end
   intermittency if reproduced; this task made no media-backend change.

The workspace has no Git metadata. Local baseline snapshots and a source review
patch are retained under `build-neumorphic/`; no commit, push or release occurred.
