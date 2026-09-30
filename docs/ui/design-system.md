# Easnap design system

Win32 presentation baseline, accepted by the user on 2026-09-21. All text,
trim/window, playhead, export, preview-opening and recording-island corrections
from that session are resolved and closed. New or changed windows must follow
the [window implementation guide](window-implementation-guide.md) and
[ADR-024](../decisions/ADR-024-native-trim-frame.md). See
[ADR-017](../decisions/ADR-017-neumorphic-editor-and-editable-annotations.md) for
the current decisions, [ADR-016](../decisions/ADR-016-shared-appearance-and-native-controls.md)
for theme ownership, and [validation](neumorphic-validation.md) for actual evidence.
The accepted corrections are detailed in
[ADR-022](../decisions/ADR-022-text-input-and-content-sized-trim.md) and
[text/trim validation](text-trim-validation.md).

## Reference and implementation values

The behavior screenshots specify tools, selection rules and compact layout.
Their flat styling is not the material reference. The supplied neumorphic image
and source analysis (`../ui-references/theme/neumorphism-inceleme.md`, retained locally) inform light
direction, related surfaces and soft depth. The extracted 26 effect styles use
Illustrator coordinates; their blur/offset values are not imported as Easnap
DIPs or Direct2D standard deviations. Raw blend codes remain uninterpreted.
Recessed CH/VOL source geometry is separately blurred edge artwork. Easnap uses
clipped inverse-mask shading to reproduce that appearance. Its recessed slider
channel and entire Dark palette are deliberate adaptations.

## Shared tokens

The authority is `apps/easnap-desktop/src/ui/ui_theme.{h,cpp}`. Annotation colors
are content values and never come from the current application palette.

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
depth without changing the saved Light/Dark preference.

## Material and motion

`DrawMaterial` caches up to 64 geometry masks per UI thread. Rounded faces use
antialiased signed-distance coverage. A separable Gaussian filter supplies the
soft paired upper-left highlight and lower-right shadow; inverted masks clipped
to the face supply recessed edges. Base application choices are 6 DIP mask
padding, 2 DIP light offset and a blur sigma of one third of that padding. These
are calibrated implementation values, not source measurements. Geometry/DPI are
part of the cache key. The accepted optimization also keeps up to 32 colored
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

## Geometry and controls — accepted 2026-09-21 baseline

UI layout values are DIPs scaled once to physical pixels. Shared Segoe UI uses
13 DIP control text, 12 DIP supporting text and 15 DIP semibold headings/time.
Typical spacing is 4/8/16 DIP; panel/control radius tokens are 14/8 DIP. Icons use
GDI+ vector paths on one 24-unit grid, normally displayed at 18 DIP. Rounded
geometry is antialiased. Overlay vector drawing imports GDI clip regions, and
label damage includes the one-pixel antialias fringe.

| Surface | Accepted composition |
|---|---|
| Modes | Screenshot / Record native choices with a dot grip; horizontal grip/blank dragging; 500 ms retirement fade, with disabled semantics and stable visuals |
| Main editor island | Up to 780 DIP, 48 DIP high at normal widths; 34 DIP icons, grip and contrasting group separators in both palettes; no footer |
| Contextual island | Selected-object or tool-default properties only; icon text modes and themed pt presets, stroke/colors, compact effect/spotlight controls, or watermark input; brush sizes use a third mini island |
| Recording configuration | Existing responsive 390 DIP layout, prominent Start, MP4/GIF and audio/quality/cursor controls, one-click theme button and Cancel |
| Active / paused / finalizing | 310 x 60 DIP bar, grip, icon-only Pause/Resume and Stop; duration centered between the buttons, no vertical separators; existing recessed coral paused state and white unpressed icon remain; truthful finalization status; unused areas and labels drag the bar |
| Trim | Shared custom caption with native side/bottom resize frame and move/minimize/maximize/close behavior, preview, recessed timeline, raised gray oval In/Out handles without caps, slender playhead with restrained coral glow, transport and neutral Save with coral ink |
| Menus / dialogs | Native menu ownership with scoped material item drawing; application confirmations use custom rounded chrome and a native modal dialog with real default/cancel actions |

The two editor islands prefer below the frame, then above, and clamp as a group
to the monitor work area. Full-monitor selections center the group; recording
configuration follows the same full-monitor rule. Their grip/blank-space drag never starts from an
interactive child. Frame-resize press hides both, including the contextual
island, and release/capture loss restores them without acquisition. Trim opens
from the recording aspect ratio and available work area with readable DPI-scaled
controls. Manual resizing keeps a 480 x 400 client-DIP minimum, adding the
native frame when computing outer tracking dimensions and capping to the work
area, with no DPI shrink. The video allocation meets the custom caption; time and
range rows stay below the timeline. Preview fits without distortion; surplus side space
is consumed before its displayed width changes. Empty status/progress rows
collapse; notices use measured wrapped height and export uses a recessed rounded
progress track with coral fill. Child layout is batched before owner-thread
painting; DWM owns supported resizable corners and exposed backing pixels use
the theme gray. Preserve native nonclient insets and the caption-only override
from ADR-024; do not restore whole-frame removal or top-level WVR_REDRAW.
Preview-helper positioning remains asynchronous. Tutorial/footer text is removed from
all capture and trim states. Operational failures use separate transient notices.

The accepted EXPORT-01 correction on 2026-09-21 retains the recessed progress
track and rounded coral fill. The duplicate sheen over the fill ends is removed.

## Annotation content and input

New non-text annotations are unselected. Hovering shape/effect outlines exposes
the move cursor in every creation tool; click selects and drag moves immediately.
Interiors do not intercept new drawing, including mosaic/spotlight regions. Selected
shapes follow their real outline/path with a thin coral trace. Mosaic brush uses
a bounding selection frame, while its hit target follows the brush contour. Selection is
separate from creation defaults. Lines/arrows have endpoint
handles; rectangle/ellipse, effect regions and spotlight regions have four
corners. Pen strokes, text and tags move without resize handles. Watermark is an
editable document-wide effect. Completed transforms and sliders are one undo
entry; cancellation restores their prior state. Movement can go outside any
capture edge, including oversized text. Source clipping hides both content and
selection frames; undo restores the previous position. Selection chrome never exports.

Native multiline EDIT supplies the real caret, word selection, paragraph
triple-click, keyboard navigation and clipboard operations. Ctrl+Enter commits;
Escape cancels the edit. Clicking another text/tag while editing activates it
immediately. A blank click commits and deselects without creating another object;
a blank click with neither an editor nor a selection starts new text. Single
clicks select existing text for dragging, even after switching repeatedly
between texts; a blank click clears that selection. Double-click enters editing.
A source-backed EDIT paint pass preserves the real
native caret and renders a hollow rounded
coral frame during editing, selection and movement. DirectWrite measures and
renders Plain, rounded
Fill or glyph Outline. Fill/Outline use white glyphs, or black when the selected
color is white. Point presets are 12/16/24/36/48/60/72/96, with validated custom
6–288 pt input. Size always affects the whole object. Content point-to-pixel
conversion retains capture DPI rather than changing with the UI theme/monitor.

In the accepted TEXT-02 correction, EDIT remains visible for native input and
caret ownership. Its synchronous glyph/background GDI draw receives an empty
clip region. Only the separate source-backed custom painter draws annotation
pixels, so typing does not expose a second native glyph or rectangular backing.

Blur and pixelation are separate effect choices, each supporting brush and
region. Strength maps to blur radius or block size; pixel changes are rebuilt
from the immutable capture. Spotlight regions reveal their union through one
shared dim layer. Brush size opens on click or 350 ms hover and closes 250 ms
after leaving, on choice, drawing or a tool switch. Watermark accepts at most
16 UTF-16 code units (without splitting a surrogate pair); its hint disappears
after entry. A rotated lattice anchored at the image center keeps every tile
centered as text or size changes, clipped to source bounds.
The hand and chevron share one control. Brush size appears in a mini island with
a callout toward the hand and an 8 DIP island gap. All stroke presets share its
three-dot visual language. The palette includes black and gray. Watermark has a
small circular clear button inside the text field on hover; leaving hides it
even after focus. Its soft lower-right drop shadow and recessed press use the
shared material animation, and Undo restores both document and input text. Auxiliary islands,
combo lists and tooltips suspend while a native Save picker is open.
Opening brush size near the lower screen edge moves the three-island group up
only as far as needed, retaining both gaps without covering the main tools.
Tags keep a stable graphite body/white text and proportional colored dot/notch.

The magnifier is 152 × 166 DIP with a 25 × 25 sample at 4 DIP per source pixel, a minimal
reticle and three compact readout rows; it has no per-pixel border grid.

## Native semantics and validation limits

Buttons retain Invoke/Toggle/radio roles, names, shortcuts and tooltips. Native
sliders support keyboard changes; point-size combo boxes and text inputs retain
native editing. Accessible names assigned through IAccPropServices are cleared
when their HWND is destroyed. Menus keep their native command/keyboard behavior;
file pickers remain Windows-owned. Theme buttons switch immediately and persist
through the existing settings owner; tray choices remain explicit Light/Dark.

The trim timeline retains three UI Automation RangeValue children: In point,
Playback position and Out point. Tab and arrows retain their existing behavior,
including Shift for In and Ctrl for Out. Export boundaries remain governed by
media samples. Native preview pixels are not rendered by PrintWindow; a black
preview in fixture evidence is not proof of failed playback or capture exclusion.
See the validation report for the exact automated and manual coverage.

The interaction corrections are specified in [ADR-018](../decisions/ADR-018-editor-interaction-corrections.md).
Stable paint, unrestricted movement and custom chrome are specified in
[ADR-019](../decisions/ADR-019-stable-native-paint-and-custom-window-chrome.md).
All three text-mode icons share one T geometry; Fill has a close rounded backing.
Both mosaic icons share an outlined checker-cell symbol.
