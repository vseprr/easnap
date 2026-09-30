# User flows

These are implemented behaviors. Validation and remaining work are recorded in
[project status](project-status.md).

## Start and select

Press **Ctrl+Shift+A** or choose **Capture** from the tray to enter Standby.
Reusing the shortcut during a session brings that session forward.

In Standby, hover a monitor for its selection or drag a custom region.
A click enters Editing with the current proposal; a completed drag enters it
with the custom region.
The magnifier reads the frozen Standby snapshot. **Shift** switches RGB/HEX;
**Ctrl+C** copies the displayed color.
The compact magnifier displays each source pixel at 4 DIP with a wider sampled area.

Double-click the selection to copy a screenshot directly, or press **Enter**
to open Editing. Entering Editing does not yet capture the final screenshot.
The frame remains adjustable over the live desktop, and the top
**Screenshot / Record** options switch modes.

## Screenshot Editing

Move the provisional frame by dragging its body; use its handles to resize it.
Arrow keys move it, **Shift+Arrow** uses a larger step, and **Ctrl+Arrow** resizes.
**Ctrl+A** selects the receiving monitor. The frame's size label stays current; the tools island has no duplicate readout.

The first click in the floating toolbar fixes the image. This includes tool and
style controls, blank padding and the move grip.
A screenshot command such as Copy or Save also fixes it on demand.
The close/cancel command exits without taking a screenshot.

Fixation captures the current desktop at the chosen rectangle, then shows the
acquired image. Capture-excluded surfaces stay visible on supported Windows;
older builds or unprotected popups use a hide/show fallback. The handles disappear,
the frame stops moving/resizing, and mode switching stops immediately.
The top mode options fade away over **500 ms**. Moving the toolbar after this
changes only the toolbar's position. If acquisition fails, the frame stays
provisional and the action can be retried.

Use Pointer, Rectangle, Ellipse, Line, Arrow, Pen, Text, Tag, Blur/Pixelate,
Spotlight or Watermark. The main island contains icons; its separate contextual
island shows properties for the selected annotation, or defaults for the next
object. Pointer without a selection has no contextual island. The group prefers
below the frame, then above it, and stays within the monitor work area.
Full-screen screenshot editing and full-screen recording configuration default
to the center instead. The existing placement rule remains for dragged regions. Both
islands hide immediately during frame-handle resizing and restore on release or
capture loss. The top mode strip can be dragged horizontally from its grip or
blank space; the main island can be dragged from its own grip or blank space.

New drawings stay unselected. Hover the outline of an existing shape/effect with
any tool to see the move cursor; a single click selects it and a drag moves it.
Empty interiors allow another drawing, including inside mosaic/highlight regions. Line/arrow
endpoints have two handles; shapes, effect regions and spotlight regions have
four corner handles. Pen, text and tags move without stretching. Selection traces
the shape or stroke; mosaic brush uses a bounding frame while pointer interaction
still follows its brush contour. Text and tags use a hollow
rounded coral frame.
Objects can move beyond every capture edge, including oversized text. Their
content and selection frames are hidden outside the capture; Undo restores a
fully hidden object's previous position.
Property changes affect the selected object; creation defaults stay separate.
Each completed move, resize or slider gesture is one undo step. Escape cancels
an active gesture before the normal session cancellation behavior.

Single-click existing text or a tag to select it for dragging. A blank click
clears the selection without creating text, even after repeatedly switching
between boxes. With neither selection nor active editor, a blank click creates
a new object. Double-click text or a tag to edit with a native caret;
double-click selects a word and triple-click selects the current paragraph. Enter inserts a line,
Ctrl+Enter commits, and Escape cancels that editing session. The first click
on blank space commits and deselects without creating more text; a blank click
with neither active editor nor selection starts a new object. The double-click continuation of a
blank press never creates another object. Clicking another text/tag while
editing activates that box immediately. The native caret remains active until editing ends,
and dragging its hollow frame moves the text. Plain, Fill and
Outline apply to the whole annotation, as do color and point-size changes even
when only part of its text is selected. Presets and custom 6–288 pt input are
available in a rounded themed list with pt suffixes. Fill/Outline use white glyphs, or black when the chosen color is white.
Tags have a stable dark label body and a colored dot/notch.
Clicking active text again keeps its editing session; it does not consume the
first outside click. Black and gray are included in the annotation palette.

Blur and Pixelate each support Brush and Region, brush sizes and a strength
slider. The hand/chevron opens a brush-size mini island on click or delayed hover;
it closes after leaving, choosing a size or drawing. The hand and chevron share
one button; the three size dots appear in
a pointed mini island with the same spacing as the main/context islands. Other
stroke-width presets use the same dot presentation. Regions stay editable after
creation. Spotlight shape icons share one outside
dim-opacity setting and reveal the union of their regions. Watermark settings
edit up to 16 characters of repeated angled text, including color, opacity and
size. Tiles grow symmetrically around the image center.
A small round X appears at the right of the watermark text field on hover and
clears its text; moving away hides it, and Undo restores both the watermark
and input value. The circular button has a soft lower-right shadow and a
recessed press animation in either theme.
These effects are reversible and preview/export use the same document.

To finish:

- **Copy:** check-mark icon or **Ctrl+C** outside caret editing. A double-click on
  blank image content with Pointer and no active object interaction also copies.
  Object selection, drawing and text editing take priority over quick-copy.
- **Save:** download icon or **Ctrl+S** opens the native PNG/JPEG destination
  picker. Success closes the session; cancelling or failure keeps it available.
  Auxiliary configuration/brush islands stay hidden while the picker is open.
- **Discard:** X, **Esc**, or the workspace's right-click cancellation path.
  Escape first cancels an active text edit or transform when applicable.

Copy always uses the full-resolution composited source and annotations.
Success briefly shows a green notice and closes the session. Clipboard,
acquisition or compositing failure keeps the image/tools available for retry.
Neither dragging a handle nor double-clicking a toolbar control copies the image.

### Mode round trips and quick return

Before screenshot fixation, switching Screenshot → Record hides and retains
the screenshot tools; returning restores their placement and settings.
Recording configuration likewise survives eligible mode switches.

Leaving an untouched Screenshot workspace arms the existing one-shot
quick-return behavior. On return, the next successful editing mutation or
eligible blank Pointer content click can complete through the normal Copy path.
Editor pointer sequences do not independently consume the latch; a successful
document commit retains the existing one-shot completion behavior.
Selecting a tool or fixing the frame alone does not consume that latch.
Completion failure leaves a normal retryable session, without a repeating
automatic-copy loop. See [ADR-002](decisions/ADR-002-explicit-region-confirmation.md).

## Record

Choose Record while the workspace is provisional. Configure:

| Choice | Behavior |
|---|---|
| MP4 | H.264 video; optional AAC audio |
| GIF | Silent animation with High quality/Low size presets |
| Audio | Independent System audio / Microphone toggles: neither, either or Both for MP4 |
| Cursor | Include or omit the pointer |

Switching to GIF hides audio controls and records without audio, while retaining
the MP4 audio choice. Format, preset, audio and cursor choices survive mode round
trips and are saved for later sessions after Start succeeds. Cancelling
configuration does not save newly selected recording preferences. Appearance is
available in the tray and as a one-click Light/Dark toggle in the configuration
panel and screenshot island. Light/Dark changes apply and
save immediately while leaving those pending recording choices untouched. Windows
high contrast temporarily uses system colors without replacing that preference.

Drag the configuration or active recording panel from its grip, informational
labels or any other noninteractive space. Buttons and settings retain their own
actions. Moving the provisional frame or mode strip keeps configuration above
the dimming layer and available for Start.

Only **Start** begins recording. **Esc** before Start cancels without creating a
recording. Start retires the capture workspace and displays the actual recording
bounds and movable controls. Encoding may reduce an odd width/height by one
pixel at the right/bottom edge. If Start fails, the configuration and frame stay
available with a retry message; partial-media recovery remains coordinator-owned.

The recording island uses a Pause icon while recording and a Play icon to
resume while paused. It preserves the existing raised/recessed material and
state colors. The elapsed duration sits midway between Pause/Resume and Stop,
with no vertical separators. Pause freezes the active-time timer and excludes
the paused interval from output. Resume continues the same recording. Stop—or Esc while recording/paused—drains
and finalizes the media asynchronously. Finalization cannot be discarded by Esc.
An error is shown as an error, with recoverable media retained when available.

## Preview, trim and finish a recording

After finalization, the shared completion window opens with the first decoded
frame already visible, paused at zero, and the full source interval selected.
This also applies to one-frame recordings; opening does not require Play. Preview uses the finalized MP4 working recording for both output
formats; GIF conversion happens during Save/Copy, so preview does not show the
GIF preset's final palette or cadence. Play/Pause, seek and In/Out controls
operate on that source.
The trim window and discard confirmation have custom Light/Dark outer chrome.
Trim retains dragging, resizing, minimize/maximize and system close. It opens
to suit the recording aspect ratio while keeping the controls readable. Manual
resizing retains a 480 x 400 DIP usable client minimum, capped to the work area. The
video starts below the custom caption; time is below the timeline. Extreme
source ratios fit inside the readable UI. The preview keeps its aspect ratio and
consumes surplus side space before shrinking. The redundant recording heading
is absent. Empty message/progress space collapses between the two button rows;
notices expand to their wrapped text height. Export shows a themed progress bar
driven by actual export progress, with Cancel remaining available. In/Out
handles are raised gray ovals without horizontal caps; the slender playhead
uses a restrained coral glow. Save uses coral ink on neutral material. Closing the discard dialog keeps the recording.
Playback stops at the selected Out point and keeps the cursor there. The last
source frame before the exclusive Out remains visible until another action;
full-length playback similarly retains its final frame. Play again seeks to In
(zero for the full recording), presents the frame covering that time, and resumes.
Native preview runs in a supervised
helper so a stalled transport can recover or fail visibly without trapping the
parent window.

The trim interval is half-open: In is included and Out is excluded by the
exporter's sample policy. Video sample overlap and audio encoder granularity
mean MP4 endpoints are not arbitrary sub-frame cuts. Late MP4 excerpts may take
longer because decoding currently starts at the beginning.

- **Save** selects a durable destination and publishes only a completed export.
- **Copy** exports and places a recording file on the clipboard for file-aware
  applications. It is not a bitmap or text clipboard item.
- **Discard** asks for confirmation before removing the session.

Full-range MP4 Save/Copy reuses the finalized recording without re-encoding.
A new GIF result requires conversion for both full-range and trimmed selections.
A completed result can be reused for an identical range while its file remains
available. Successful Save/Copy closes the recording session.

A cancelled or failed export keeps the source available. Controls reflect the
busy state while work is pending. Clipboard-referenced recording files survive
normal cleanup so pasting remains possible after closing Easnap.

## Keyboard reference

| Context | Input | Action |
|---|---|---|
| Global | Ctrl+Shift+A | Open or reactivate the current capture session |
| Standby | Enter | Open Editing |
| Standby | Shift | Toggle magnifier RGB/HEX |
| Standby | Ctrl+C | Copy magnifier color |
| Provisional Editing | Ctrl+A | Select receiving monitor |
| Provisional Editing | Arrows / Shift+Arrows | Move frame by 1 / 10 pixels |
| Provisional Editing | Ctrl+Arrows | Resize frame |
| Screenshot Editing | Ctrl+Z / Ctrl+Y | Undo / redo |
| Screenshot Editing | Ctrl+C / Ctrl+S | Copy / Save |
| Screenshot Editing | Esc | Cancel pending text, otherwise cancel session |
| Recording configuration | Enter / Esc | Activate focused control / cancel |
| Active or paused recording | Esc | Stop and finalize |
| Trim timeline | Tab / Shift+Tab | Focus In, playhead, Out, then leave timeline |
| Trim timeline | Arrows | Adjust focused handle by 1 second |
| Trim timeline | Shift+Arrows / Ctrl+Arrows | Adjust In / Out |
| Trim timeline | Home / End | Move focused handle toward source start / end within valid bounds |
| Completion window | Enter / Esc | Activate focused control / confirm Discard |

Native controls retain their normal Tab and keyboard behavior. Input is routed
by context and focus; for example Standby's color copy differs from Editing's
image copy.

## Desktop and recovery behavior

Selection/source coordinates use signed physical desktop pixels; DPI scales UI
controls independently. Workspace display-layout changes cancel the current
selection to avoid silently using stale coordinates. A started recording uses
its fixed capture rectangle.

Tray actions include capture, cursor preference, About, opening the local data
folder and Exit. Settings and private recordings are local to the current user.
See [SECURITY.md](../SECURITY.md) for clipboard-file lifetime and reporting.

Manual checks should cover real pointer speed, mixed DPI and monitor origins,
tool gestures versus double-click completion, clipboard contention, and native
audio/preview behavior on the intended devices. Passing deterministic tests is
not a substitute for those observations.
