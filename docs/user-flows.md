# User flows

These are implemented behaviors. Validation and remaining work are recorded in
[project status](project-status.md).

## Start and select

Press **Ctrl+Shift+A** (the shortcut can be changed in
[Settings](#settings-and-the-tray-menu)), choose **Capture** from the tray
menu, click the tray icon, or start Easnap again while it runs. Each enters
Standby. Reusing the shortcut during a session brings that session forward;
while one of Easnap's dialogs is open it brings that dialog forward.

In Standby, hover a monitor for its selection or drag a custom region.
A click enters Editing with the current proposal; a completed drag enters it
with the custom region.
The magnifier reads the frozen Standby snapshot. **Shift** switches RGB/HEX;
**Ctrl+C** copies the displayed color.
The compact magnifier displays each source pixel at 4 DIP with a wider sampled area.

Double-click the selection to copy a screenshot directly, or press **Enter**
to open Editing. Entering Editing does not yet capture the final screenshot.
The frame remains adjustable over the live desktop, and the top
**Screenshot / Scrollshot / Record** options switch modes.

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

## Scrollshot

Scrollshot joins everything that passes through the frame into one tall
image. It is manual: you scroll, Easnap follows. It needs Windows 10
version 2004 (build 19041) or later; on an older build the mode button and the
tools island's button are disabled and say so.

**Start.** Choose **Scrollshot** while the workspace is provisional. An island
below the frame, or beside it when there is no room, offers **Start
Scrollshot** and a help button. The frame can still be moved and resized;
**Enter** starts. The scissors button on the screenshot tools island starts a
Scrollshot directly from an untouched Screenshot frame. Once the screenshot is
fixed the button is disabled and its tooltip says why.

**Capture.** Start locks the frame: it cannot be moved, resized or switched to
another mode, and a right-click no longer cancels. The border is drawn outside
the selection. Inside the frame the workspace is open, so the pointer, the
wheel and, after a click, the keyboard go to the application underneath.
Outside the frame Easnap keeps the input.

Scroll the content with the wheel, the keyboard or its scrollbar. The image
only grows downwards: scrolling back up adds nothing, and Easnap finds its
place again when you continue. A header or footer that stays in place is kept
once, as are fixed side rails. The mouse pointer, the border, the island and
the preview are never part of the image. The image ends at 32,000 rows or 48
million pixels, whichever comes first.

The island shows the stitched size and one status line:

| Status | Meaning |
|---|---|
| Scroll the content inside the frame. | Following |
| Easnap lost track. Scroll back up a little, then continue. | Recent pictures could not be placed; nothing is being added |
| Maximum length reached. Copy, save or edit the image. | The size limit |
| This area cannot be captured any more. What was captured is kept. | The screen rectangle cannot be read |
| The wheel goes to the active window. Click inside the frame once, then scroll. | Windows routes the wheel by focus on this computer |
| Capture stopped. Copy, save or edit the image. | After an action stopped the capture and the image is still held |

A live preview of the lower end of the image stands beside the frame. It never
takes input. The island and the preview stay outside the frame; if the frame
leaves no room on its monitor they move to another monitor, and only when
there is none do they lie over the frame.

**Finish.** Each action first stops the capture.

| Action | Result |
|---|---|
| Copy (**Enter** or **Ctrl+C**) | The image goes to the clipboard and the session ends. If the clipboard refuses a very tall image, the image stays and the status points to Save |
| Save (**Ctrl+S**) | A PNG save dialog. Cancelling it returns to the island with the image |
| Edit | Opens the stitched image in the screenshot tools |
| Discard (**Esc**) | Drops the image and ends the session. Easnap asks first once the capture has grown beyond its first picture |

The keys work while Easnap has the keyboard focus. After a click inside the
frame the focus is in the other application; **Ctrl+Shift+A** brings it back
to the island.

### Editing a stitched image

Edit opens the whole image in the same tools as a screenshot. It is fitted to
its width, never enlarged beyond 100%, and starts at the top. A label shows
its size and the zoom; a thumb on the right edge shows the position.

- The wheel scrolls; **Shift+wheel** pans sideways when the image is wider
  than the view; **Ctrl+wheel** zooms around the pointer.
- **Page Up / Page Down**, **Home / End** and, with nothing selected, the
  arrow keys scroll.
- **Ctrl+Plus / Ctrl+Minus** zoom in steps from 10% to 400%; **Ctrl+0** fits
  the width; **Ctrl+1** is 100%.

Every tool works at every scroll position and zoom. Copy and Save export the
whole image at its captured resolution with all annotations, not the part on
show. The view cannot be turned back into a Scrollshot capture. Esc, Discard
and closing ask before a stitched image is dropped.

## Record

Choose Record while the workspace is provisional. On a Windows N or KN edition
without the Media Feature Pack, Record says that recording needs it and the
mode does not change; screenshots and Scrollshot work there. Configure:

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
available in Settings and the tray (Same as Windows, Light, Dark) and as a
one-click Light/Dark toggle in the configuration panel and screenshot island;
the toggle chooses one of the two and stops following Windows. A change
applies and is saved immediately while leaving those pending recording choices
untouched. Windows high contrast temporarily uses system colors without
replacing that preference.

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
| Global | Ctrl+Shift+A, or the shortcut chosen in Settings | Open or reactivate the current capture session |
| Settings, shortcut button waiting | A combination with Ctrl, Alt or Win / Esc | Take it over / keep the current one |
| Standby | Enter | Open Editing |
| Standby | Shift | Toggle magnifier RGB/HEX |
| Standby | Ctrl+C | Copy magnifier color |
| Provisional Editing | Ctrl+A | Select receiving monitor |
| Provisional Editing | Arrows / Shift+Arrows | Move frame by 1 / 10 pixels |
| Provisional Editing | Ctrl+Arrows | Resize frame |
| Screenshot Editing | Ctrl+Z / Ctrl+Y | Undo / redo |
| Screenshot Editing | Ctrl+C / Ctrl+S | Copy / Save |
| Screenshot Editing | Esc | Cancel pending text, then clear the selection, then cancel the session; asks first when something has been drawn |
| Scrollshot, before Start | Enter | Start |
| Scrollshot capture and review | Enter / Ctrl+C | Copy |
| Scrollshot capture and review | Ctrl+S / Esc | Save / Discard |
| Stitched image | Wheel / Shift+wheel / Ctrl+wheel | Scroll / pan sideways / zoom |
| Stitched image | Page Up / Page Down / Home / End | Scroll by a page / to the start or end |
| Stitched image | Ctrl+Plus / Ctrl+Minus / Ctrl+0 / Ctrl+1 | Zoom in / out / fit width / 100% |
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
controls independently. A change to the monitor layout, resolution or scale
cancels the current selection or screenshot edit, because the frozen image no
longer matches the desktop. A work-area change alone, such as the taskbar
hiding or an app bar docking, does not. A started recording uses its fixed
capture rectangle; if that rectangle is no longer on the desktop (its monitor
was disconnected or shrank), the recording stops and opens for trimming with
what was captured up to that point.

A Scrollshot keeps its image through such a change. While capturing or
reviewing, the capture stops, the workspace is hidden and the island moves to
a valid place with a notice; Edit rebuilds the workspace. While a stitched
image is being edited, the annotations are drawn into the image and the
session returns to the four-action island.

Settings and private recordings are local to the current user.
See [SECURITY.md](../SECURITY.md) for clipboard-file lifetime and reporting.

## Settings and the tray menu

Easnap lives in the notification area. The first start on a computer says so
once, with the shortcut. The menu of the icon offers **Capture**, **Include
cursor**, **Appearance**, **Settings...**, **Open data folder**, **Check for
updates**, **About** and **Exit**.

**Settings...** opens while no capture is in progress. Cancel changes nothing.

| Setting | Effect |
|---|---|
| Language | Same as Windows, English or Türkçe. Applies at once to everything opened afterwards. |
| Appearance | Same as Windows, Light or Dark. "Same as Windows" also follows a later change of the Windows setting. |
| Capture shortcut | Click the button, then press the new combination. It needs Ctrl, Alt or the Windows key. A combination another application holds is refused by name. Esc keeps the current one. |
| Start Easnap when I sign in to Windows | Easnap then starts quietly in the notification area. |
| Include the mouse cursor | The same choice as in the tray menu and the recording panel. |
| Screenshots folder, Recordings folder | Where the Save dialogs open. Not set: the folder used last. |
| Save screenshots here without asking | With a screenshots folder: Save writes a PNG named after the time into it and says so. An existing file is never replaced. If the folder cannot be written, the Save dialog opens. |

When another application holds the shortcut at start, a notification says so;
the tooltip of the icon and the menu then name clicking the icon as the way to
capture.

**About** shows the version, the shortcut and the Windows build. **Copy
details** puts the version, Windows build, language, shortcut and display
layout on the clipboard for a bug report. **Check for updates** opens the
releases page in the browser; Easnap itself connects to nothing.

Easnap needs Windows 10 version 2004 (build 19041) or later and says so
instead of starting on an older build.

### Exit, sign-out and unexpected endings

Exit asks before work is lost: a recording that is running or not yet saved,
an export in progress, a screenshot with annotations, a Scrollshot. "Stay" is
the default answer. While one of Easnap's dialogs is open, Exit only sounds.

A sign-out or shutdown in the same situations is held back by Windows with
Easnap's reason, so you can go back and save. If the session ends anyway, a
running recording is closed first.

A finished recording that was neither saved nor discarded is kept when Easnap
ends without your decision: at sign-out, after a crash, or when the trim view
could not open. The next start offers it: **Save...** writes it as an MP4
file, **Delete** removes it, **Not now** keeps it for the start after that. A
recording that was still being written when Easnap stopped cannot be
recovered.

After a crash the next start says so once and points to the folder with the
crash report. Nothing is sent anywhere. The log is
`%LOCALAPPDATA%\Easnap\logs\easnap.log`; reports are in
`%LOCALAPPDATA%\Easnap\crashes`. **Open data folder** in the tray menu leads
to both.

Manual checks should cover real pointer speed, mixed DPI and monitor origins,
tool gestures versus double-click completion, clipboard contention, and native
audio/preview behavior on the intended devices. Passing deterministic tests is
not a substitute for those observations.
