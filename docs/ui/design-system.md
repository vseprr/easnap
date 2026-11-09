# Easnap design system

The implemented Win32 presentation: colours, materials, geometry and control
behaviour. New or changed windows follow the
[window implementation guide](window-implementation-guide.md) and
[ADR-024](../decisions/ADR-024-native-trim-frame.md). The decisions behind
these values are in
[ADR-016](../decisions/ADR-016-shared-appearance-and-native-controls.md)
(theme ownership),
[ADR-017](../decisions/ADR-017-neumorphic-editor-and-editable-annotations.md)
(material and editor) and
[ADR-022](../decisions/ADR-022-text-input-and-content-sized-trim.md) (text
input and the trim window).

## Reference and implementation values

The material follows a neumorphic reference image that is not part of the
repository: light from the upper left, related surfaces, soft depth. Its
effect styles were drawn in Illustrator coordinates; their blur and offset
values are not Easnap DIPs or Direct2D standard deviations and were not
imported. The recessed controls of the reference are separately blurred edge
artwork; Easnap reproduces that appearance with clipped inverse-mask shading.
The recessed slider channel and the entire Dark palette are adaptations made
for Easnap.

## Shared tokens

The authority is `apps/easnap-desktop/src/ui/ui_theme.{h,cpp}`. Annotation colors
are content values and never come from the current application palette.

Text colours (main, supporting, disabled and success) have a contrast of at
least 4.5:1, and the control boundary and keyboard focus colours at least 3:1,
against the panel, raised, recessed, hover, pressed and selected surfaces of
both palettes; `ui_modernization_native` asserts this (`PaletteContrast`).

| Role | Light | Dark |
|---|---|---|
| Uniform panel | `#E8E8E7` | `#2B2E32` |
| Raised face | `#E9E9E8` | `#2D3034` |
| Recessed face | `#E4E4E3` | `#282B2F` |
| Main text | `#2C2F33` | `#F0F1F3` |
| Supporting text | `#585C60` | `#B5B9C0` |
| Coral accent | `#EE5E5A` | `#EE5E5A` |
| Text on coral | `#261312` | `#1E1414` |
| Highlight / shadow | `#FFFFFF` / `#9EA2A4` | `#4F535A` / `#0C0E11` |
| Hover / pressed face | `#ECECEB` / `#E2E3E2` | `#313439` / `#272A2E` |
| Control boundary | `#7F8284` | `#7D828A` |
| Keyboard focus | `#A72F2C` | `#FF9A96` |
| Success | `#1C683F` | `#72DC9B` |
| Error / Stop | `#B22334` | `#FF8289` |
| Screenshot boundary | `#B1B780` | `#B1B780` |
| Recording boundary | `#EE5E5A` | `#EE5E5A` |

All application-owned backgrounds are uniform solid colors. Raised/recessed
shading stays local to components; there are no panel gradients or raster grids.
Primary actions use coral text/icons on the neutral material. Selected controls retain the deepest
pressed material state (depth -1), with coral icons/text and no coral border.
Keyboard focus uses a neutral underline except on the mode island, where the
selected recess identifies the choice. Radio/check markers have no surrounding
material box. Radio/check semantics stay native. Disabled controls expose native disabled state and
retain readable labels. High contrast uses system colors and removes decorative
depth without changing the saved appearance preference.

## Material and motion

`DrawMaterial` caches up to 64 geometry masks per UI thread. Rounded faces use
antialiased signed-distance coverage. A separable Gaussian filter supplies the
soft paired upper-left highlight and lower-right shadow; inverted masks clipped
to the face supply recessed edges. Base application choices are 6 DIP mask
padding, 2 DIP light offset and a blur sigma of one third of that padding. These
are calibrated implementation values, not source measurements. Geometry/DPI are
part of the cache key. The renderer also keeps up to 32 colored
rasters per UI thread, keyed by the effective palette/fill, dimensions, offsets,
radius, animated depth and drop-shadow setting. Complete rounded ends retain
their blur support; a horizontally constant center strip stretches without
changing the pixels. Theme switches cannot reuse stale colors. The 768-case
material-parity check preserves the pre-optimization production raster hash.

Buttons retarget local 15 ms timers: hover 145 ms, press 85 ms, release 145 ms,
checked transitions 160 ms. Contextual-island appearance uses a 160 ms fade.
Commands run immediately. Timers stop at completion; new input replaces the
target. `SPI_GETCLIENTAREAANIMATION` and high contrast disable animation. Slider
thumbs and timeline handles track input directly, with no easing delay. Actual
perceived smoothness still requires interactive desktop review.

## Geometry and controls

UI layout values are DIPs scaled once to physical pixels. Shared Segoe UI uses
13 DIP control text and 15 DIP semibold headings and time; the trim window
uses 12.5 DIP for its text and dialogs 14 DIP.
Typical spacing is 4/8/16 DIP; panel/control radius tokens are 14/8 DIP. Icons use
GDI+ vector paths on one 24-unit grid, normally displayed at 18 DIP. Rounded
geometry is antialiased. Overlay vector drawing imports GDI clip regions, and
label damage includes the one-pixel antialias fringe.

| Surface | Composition |
|---|---|
| Modes | Screenshot / Scrollshot / Record native choices with a dot grip, one width for all three from the widest label in the active language; horizontal grip/blank dragging; 500 ms retirement fade, with disabled semantics and stable visuals |
| Main editor island | Up to 832 DIP, 48 DIP high at normal widths; 34 DIP icons, grip and contrasting group separators in both palettes; no footer |
| Contextual island | Selected-object or tool-default properties only; icon text modes and a rounded themed pt preset list with pt suffixes, stroke/colors, compact effect/spotlight controls, or watermark input; brush sizes use a third mini island |
| Recording configuration | Existing responsive 390 DIP layout, prominent Start, MP4/GIF and audio/quality/cursor controls, one-click theme button and Cancel |
| Active / paused / finalizing | 310 x 60 DIP bar, grip, icon-only Pause/Resume and Stop; duration centered between the buttons, no vertical separators; existing recessed coral paused state and white unpressed icon remain; truthful finalization status; unused areas and labels drag the bar |
| Trim | Shared custom caption with native side/bottom resize frame and move/minimize/maximize/close behavior, preview, recessed timeline, raised gray oval In/Out handles without caps, slender playhead with restrained coral glow, transport and neutral Save with coral ink |
| Menus / dialogs | Native menu ownership with scoped material item drawing; application confirmations use custom rounded chrome and a native modal dialog with real default/cancel actions |

Where the islands go around the frame, when they hide and how they are dragged
is in [user flows](../user-flows.md#screenshot-editing) and
[ADR-036](../decisions/ADR-036-island-placement-around-the-frame.md). How the
trim window opens, lays out and resizes is in
[user flows](../user-flows.md#preview-trim-and-finish-a-recording) and the
[window guide](window-implementation-guide.md#3-compute-layout-before-painting).
Their visual values:

- Newly exposed backing pixels of a resizing window use the theme gray; DWM
  draws the corners of resizable windows.
- Export progress is a recessed track with a rounded coral fill and no sheen
  over the ends of the fill.
- No capture or trim state shows tutorial or footer text; operational failures
  use separate transient notices.

## Annotation content and input

How the tools behave (selection, handles, text editing, effects, watermark
input) is in [user flows](../user-flows.md#screenshot-editing). The decisions
are [ADR-017](../decisions/ADR-017-neumorphic-editor-and-editable-annotations.md),
[ADR-018](../decisions/ADR-018-editor-interaction-corrections.md),
[ADR-020](../decisions/ADR-020-workspace-layering-and-outline-interaction.md)
(Save suspension, brush-size group placement) and
[ADR-022](../decisions/ADR-022-text-input-and-content-sized-trim.md), which owns
how the native EDIT and the source-backed painter share a text box. The visual
values:

- Selection traces and the hollow coral frame around text and tags follow the
  behaviour described in [user flows](../user-flows.md#screenshot-editing);
  selection chrome never exports.
- DirectWrite renders text as Plain, rounded Fill or glyph Outline. Fill and
  Outline use white glyphs, or black when the selected color is white. The
  point-size presets and the custom size range are listed in
  [user flows](../user-flows.md#screenshot-editing). Content point-to-pixel
  conversion keeps the capture DPI rather than changing with the UI theme or
  monitor.
- Tags keep a stable graphite body with white text and a proportional colored
  dot and notch. The palette includes black and gray.
- The brush size mini island opens from the shared hand and chevron control
  with the hover and close timings given in
  [user flows](../user-flows.md#screenshot-editing). Visually it has a callout
  toward the hand and an 8 DIP island gap, and all stroke presets share its
  three-dot visual language.
- Watermark tiles form a rotated lattice anchored at the image center; every
  tile stays centered as text or size changes, clipped to source bounds.
- The watermark field's clear button is a small circle inside the field, with a
  soft lower-right drop shadow and a recessed press that use the shared
  material animation; when it clears is described in
  [user flows](../user-flows.md#screenshot-editing).

The magnifier is 152 × 166 DIP with a 25 × 25 sample at 4 DIP per source pixel, a minimal
reticle and three compact readout rows; it has no per-pixel border grid.

## Native semantics and validation limits

Buttons retain Invoke/Toggle/radio roles, names, shortcuts and tooltips. Native
sliders support keyboard changes; point-size combo boxes and text inputs retain
native editing. Accessible names assigned through IAccPropServices are cleared
when their HWND is destroyed. Menus keep their native command/keyboard behavior;
file pickers remain Windows-owned. Theme buttons switch immediately and persist
through the existing settings owner; the tray's Appearance submenu, like
Settings, offers Same as Windows, Light and Dark.

The trim timeline retains three UI Automation RangeValue children: In point,
Playback position and Out point. Tab and arrows retain their existing behavior,
including Shift for In and Ctrl for Out. Export boundaries remain governed by
media samples. Native preview pixels are not rendered by PrintWindow; a black
preview in fixture evidence is not proof of failed playback or capture exclusion.
What the tests cover is in [testing](../testing.md); the checks made for each
release are in the [validation records](../releases/validation-history.md).

The interaction corrections are specified in [ADR-018](../decisions/ADR-018-editor-interaction-corrections.md).
Stable paint, unrestricted movement and custom chrome are specified in
[ADR-019](../decisions/ADR-019-stable-native-paint-and-custom-window-chrome.md).
All three text-mode icons share one T geometry; Fill has a close rounded backing.
Both mosaic icons share an outlined checker-cell symbol.
