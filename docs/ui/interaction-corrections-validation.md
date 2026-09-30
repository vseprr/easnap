# Editor interaction corrections: validation

Workspace availability rechecked 2026-09-29: the historical build tree is absent.
Its artifact paths are retained as archival references, not download links.
Recorded results below remain historical; see [current status](../project-status.md).

Historical implementation/test evidence. The current accepted UI baseline and
2026-09-21 session closure are in [project status](../project-status.md) and
[session validation](text-trim-validation.md). This report is not an open task list.

2026-09-17. Implements the user's Turkish correction notes and fifteen concept
screenshots while preserving the existing neumorphic Light/Dark theme.

## Delivered behavior

- New drawings stay unselected; hover/move/select work without the Pointer tool.
  Shape/path traces replace generic selection rectangles; handles appear on click.
- Native Text/Tag caret editing paints over the captured image with hollow rounded
  coral frames. First outside click commits without another object; double-click
  resumes writing. Text grows horizontally, selected point-size changes affect
  the whole object, and dragging the active text frame moves it.
- Icons-only main island with no duplicate size/footer or tutorial text; compact
  blur/text/spotlight settings, white group separators and a brush-size mini
  island. Presets use themed owner drawing and pt suffixes; custom sizes validate.
- Rounded islands, deepest pressed selected controls, coral selected ink without
  coral control borders, neutral keyboard focus underlines, buffered sliders and
  trim timeline. Hidden sliders remain hidden when their values change.
- Outside-only provisional shading, no frozen selection repaint, capture-excluded
  fixation without hide/show on supported Windows, prepainted fixed image, 500 ms
  mode fade with stable disabled appearance, and Escape after island interaction.
- Watermark cue and 16-unit limit, centered symmetric tiling, borderless recessed
  input. Capture/save/clipboard/input errors use separate temporary notices.

## Checks and artifacts

Windows 10 build 19045, MSVC 19.44, SDK 10.0.26100; warnings treated as errors.

| Check | Actual result |
|---|---|
| Full Release regression suite | 22/22 passed, 338.65 s; log (`build-ui-corrections/full-release.log`, historical artifact) |
| Final affected Release checks | 9/9; log (`build-ui-corrections/final-release-ui.log`, historical artifact) |
| Affected Debug checks | 9/9; log (`build-ui-corrections/affected-debug.log`, historical artifact) |
| Final affected Debug checks | 9/9; final log (`build-ui-corrections/final-debug-ui.log`, historical artifact) |
| Native editor evidence | Directory (`build-ui-corrections/evidence/`, historical artifact) |
| Portable installation | Easnap (`build-ui-corrections/portable/bin/Easnap.exe`, historical artifact), with matching preview helper |
| Source review patch | Patch (`build-ui-corrections/changes.patch`, historical artifact) |

The complete suite ran before the final error-notice, hidden-slider-state and
rounded-spotlight-trace refinements. The final affected runs cover those changes;
no media implementation changed after the full run. The final single-row
watermark spacing was then rebuilt in both configurations, passed native editor
checks in Release and Debug (`build-ui-corrections/final-layout-debug.log`, historical artifact),
and its refreshed render was inspected. The formerly intermittent
MFPlay natural-end case passed this run; no backend fix or guarantee is claimed.

The expanded production-HWND tests exercise all annotation types, selection and
movement, undo, native caret ownership, source pixels behind editing text, text
frame dragging, word/content persistence, point-size validation, hover/click/leave
brush popup timing, watermark limits, deepest pressed-state pixel equivalence,
Escape, rounded regions, outside-shading geometry and capture exclusion. A real
changing-color desktop HWND is captured through the production capture provider
while the overlay stays visible; captured pixels match both updates exactly.
Existing tests cover full/dirty drawing equivalence, synthetic 96/120/144/192 DPI,
frame placement/visibility, timeline UIA, real preview and repeated theme switching.

Representative captures inspected at native size:

- Main island (`build-ui-corrections/evidence/editor-light.png`, historical artifact),
  blur (`build-ui-corrections/evidence/context-blur-light.png`, historical artifact),
  spotlight (`build-ui-corrections/evidence/context-spotlight-light.png`, historical artifact),
  brush popup (`build-ui-corrections/evidence/brush-mini-island.png`, historical artifact).
- Text editing (`build-ui-corrections/evidence/text-native-edit.png`, historical artifact),
  tag editing (`build-ui-corrections/evidence/tag-native-edit.png`, historical artifact),
  Light presets (`build-ui-corrections/evidence/font-presets-light.png`, historical artifact),
  Dark presets (`build-ui-corrections/evidence/font-presets-dark.png`, historical artifact).
- Ellipse selection (`build-ui-corrections/evidence/selected-tool-4.png`, historical artifact),
  pen selection (`build-ui-corrections/evidence/selected-tool-1.png`, historical artifact),
  watermark (`build-ui-corrections/evidence/watermark-centered.png`, historical artifact).

## Remaining review limits

The Windows computer-use helper returned a native-pipe connection error. Real
HWND tests and renders were used; no pointer-driven manual acceptance or motion
video is claimed. Perceived smoothness, every IME/caret path, screen readers,
Windows 11, physical mixed-DPI/multiple-monitor setups and additional GPU/audio
devices remain manual checks. The alpha-1 click-receiving plane can differ from
the live desktop by at most one 8-bit level; it does not affect captured pixels.
Large images/long blur histories still need hardware-specific profiling.

The build preserves prior capture/media ownership, recovery and publication
contracts. The source baseline is under `build-ui-corrections/baseline/`; the
workspace has no Git metadata. Phase 3.3's earlier acceptance remains historical;
this correction slice has no claimed user acceptance and does not start Phase 4.
