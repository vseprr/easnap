# Paint stability and custom chrome validation

Workspace availability rechecked 2026-09-29: the historical build tree is absent.
Its artifact paths are retained as archival references, not download links.
Recorded results below remain historical; see [current status](../project-status.md).

Historical implementation/test evidence. The current accepted UI baseline and
2026-09-21 session closure are in [project status](../project-status.md) and
[session validation](text-trim-validation.md). For new windows, follow the
[window implementation guide](window-implementation-guide.md); its native frame
policy supersedes the earlier chrome experiments recorded here.

2026-09-17, Windows 10 19045, MSVC 19.44, SDK 10.0.26100. The existing Light/Dark
neumorphic material remains the visual authority.

## Implemented scope

| Report | Correction |
|---|---|
| Native ghost text and press rectangles | Native state/typing paint guards and complete buffered paints; native caret preserved |
| Grip/separator jitter | Canvas invalidation without repainting unchanged island chrome; buffered panels |
| First fullscreen black frame | Transparent allocation and explicit window-surface painting before exposure; capture exclusion retained |
| Colors and mode underline | Contrasting Light separators, khaki screenshot frame, coral recording frame, no mode focus underline |
| Primary action backgrounds | Coral ink on neutral material for Start, Save and other primary actions |
| Recording markers and Pause | Dot/check without extra boxes; latched recessed Pause/Resume and reliable paused-time color |
| Trim handles | Raised gray ovals without horizontal caps; keyboard focus changes depth |
| Fullscreen placement | Centered screenshot group and recording configuration; dragged-region rules preserved |
| Clamped annotations | Unrestricted movement, source clipping of content/selection/edit host, off-canvas effect exclusion and Undo |
| Text/mosaic icons | One T geometry with rounded inset Fill backing; shared checker-cell mosaic |
| Legacy window frames | Custom rounded trim/modal chrome with native window management and default/cancel semantics |

## Automated results

Final affected Release **9/9 passed** (20.40 s), Debug **9/9 passed** (27.29 s).
Release log (`build-ui-stability/final-release-affected.log`, historical artifact),
Debug log (`build-ui-stability/final-debug-affected.log`, historical artifact).
All targets build with warnings as errors. Portable app/helper SHA-256 values
match the built executables.

- Already presented BUTTON pixels are read after mouse down/up and disabled
  transitions before WM_PAINT; foreign native rectangles do not appear.
- Repeated WM_CHAR typing in actual Text/Tag EDIT controls matches the production
  renderer at the presented-pixel level; native caret checks remain.
- Rectangle, ellipse, line, arrow, pen, blur region, spotlight, Text and Tag move
  outside all four edges, disappear including selection decoration, and return
  through Undo. Text/Tag movement includes 288-point size.
- First composed fullscreen fixture pixel is RGB(49,66,87) before queued paint.
  Only that deterministic fixture surface becomes capturable for observation,
  then its exclusion is restored. Production capture also reads changing fixture
  pixels through visible excluded UI.
- Repeat Pause/Resume checks verify native state and actual elapsed-text colors
  in both themes. Dialog checks verify modality, default Keep recording, safe
  close and full-client chrome. Trim checks include real H.264 preview readiness,
  three UI Automation RangeValue children, resize hit tests and compact layout.
- 100 theme switches retain GDI counts (21 to 21) and identical compositor pixels.

## Visual evidence and limits

Actual HWND renders were recorded in evidence (`build-ui-stability/evidence/`, historical artifact):
discard Dark (`build-ui-stability/evidence/discard-dark.png`, historical artifact),
discard Light (`build-ui-stability/evidence/discard-light.png`, historical artifact),
trim Dark (`build-ui-stability/evidence/trim-dark.png`, historical artifact),
trim compact (`build-ui-stability/evidence/trim-compact-dark.png`, historical artifact),
paused recording (`build-ui-stability/evidence/recording-paused-dark.png`, historical artifact).
Editor/context, text editing, icons, presets and held-button images are included.
PrintWindow cannot render the separate media preview helper's pixels; the black
preview area in static renders is not a playback assertion.

Computer Use could not connect to its native pipe (OS error 2). No manual motion
video, physical mixed-DPI, full IME or screen-reader acceptance is claimed.
Automated intermediate-pixel and first-frame checks cover narrower regressions;
other GPUs and Windows 11 remain unverified.

The prior slice's 22/22 full suite is historical. This slice runs the nine affected
UI/editor/state/preview tests in both configurations; no media engine changes.
See [ADR-019](../decisions/ADR-019-stable-native-paint-and-custom-window-chrome.md).

Portable: Easnap.exe (`build-ui-stability/portable/bin/Easnap.exe`, historical artifact), with
EasnapPreviewHost.exe alongside. Review patch (`build-ui-stability/changes.patch`, historical artifact).
No external publication.
