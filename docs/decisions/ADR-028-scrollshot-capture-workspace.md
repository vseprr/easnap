# ADR-028 — Scrollshot in the capture workspace

**Status:** Accepted
**Date:** 2026-10-13

## Context

[ADR-027](ADR-027-scrollshot-stitching-engine.md) gave Easnap an engine that
places pictures of a scrolling rectangle on a canvas. The application needed
the flow around it: choose a region, start, scroll, and finish with the
stitched image. Three properties of that flow have no precedent in the
screenshot and recording paths.

- Capture is continuous while Easnap's own windows are on screen next to, and
  sometimes over, the rectangle being captured.
- The user has to operate another application through Easnap's full-screen
  workspace: wheel, pointer and keyboard must reach the content in the frame.
- The result is larger than the screen and may be held for a long time before
  the user decides what to do with it.

Scrolling is manual. Automatic scrolling, horizontal capture and a dedicated
shortcut are out of scope.

## Decision

### States

`session_transitions` remains the only authority. Three states were added:

| State | Meaning |
|---|---|
| `ScrollshotConfiguring` | Scrollshot mode chosen; the frame is still adjustable |
| `ScrollshotCapturing` | The frame is locked and the worker is stitching |
| `ScrollshotReview` | The worker has stopped; the image is held for an action |

`ScrollshotConfiguring` is a sibling of `ScreenshotEditing` and
`RecordingConfiguring`: the three mode buttons move between them.
`ScreenshotEditing → ScrollshotCapturing` is the tools island's scissors
button, which skips the Start island. `ScrollshotReview ↔ ScreenshotEditing`
is Edit, and the way back after a display change. Every Scrollshot state can
end in `Idle`.

### Capture exclusion is a precondition

A screenshot can hide the workspace for one frame when its windows cannot be
excluded from capture. A continuous capture cannot. Scrollshot therefore
starts only when `CanCaptureVisibleWindows()` holds: every visible window of
the process carries `WDA_EXCLUDEFROMCAPTURE`. That needs Windows 10 build
19041. On an older build the Scrollshot mode button and the scissors button
are disabled and their text says why; nothing is stitched from a desktop on
which Easnap would photograph itself.

### Nothing translucent contains the frame

Exclusion alone does not return the pixels the other application drew.
Measured on the development PC (Windows 10 build 19045, NVIDIA): while a
translucent capture-excluded window is on a monitor, Desktop Duplication and
GDI both return a few values changed by one level (47 as 46, 70 as 69)
everywhere inside that window's rectangle, even where a window region cuts
the window away. The workspace's shading was such a window: one
monitor-sized window at 48% opacity with a region around the selection.

Before the first picture `OverlayController::ExposeScrollshotFrame` replaces
it with four plain windows above, below, left and right of the frame. The
screen looks the same, and no translucent window's rectangle contains the
frame any more. The single window returns when the capture stops. The input
plane (opacity 1 of 255) and the colour-keyed chrome window were measured to
have no such effect.

### Desktop Duplication, where it is available

Windows lets a process duplicate an output once. The workspace's own provider,
which took the picture for region selection, therefore gives its duplication
up before the capture session creates one.

A duplication that has not been handed a composed frame yet returns a black
surface, and the desktop usually stands still when a capture starts. The
provider used to take such a surface as proof that the output only returns
black and read everything through GDI from then on. With
`RegionImageOptions::await_first_composed_frame` the session treats it as "no
frame yet": that one picture is read through GDI and duplication is asked
again. In practice the first picture comes through GDI and every later one
through duplication, which also lets the worker wait for the desktop to change
instead of polling. Both paths return the same pixels. Recording keeps the
earlier rule.

### The first picture comes before the lock

`StartScrollshotCapture` creates the `ScrollshotSession` and waits, up to
three seconds, for its first picture to be on the canvas. Only then does the
state change and the frame lock. A capture that cannot start leaves the
workspace as it was, with a message; only the invisible rearrangement of the
shading is put back.

### The locked frame

While the state is `ScrollshotCapturing` or `ScrollshotReview` the selection
cannot be moved or resized, the mode bar is disabled, a right-click does not
cancel and a new drag does not start a selection. `CaptureWorkspace` owns
this policy (`scrollshot_locked`, `can_adjust_selection`, `can_switch_mode`).

The border is drawn around the selection instead of on its edge, without
handles or a size label, so that it covers none of the content. A frame that
fills its monitor has no outside; its border is then drawn inside, where
capture exclusion keeps it out of the image.

### Input reaches the content through a hole, not through forwarding

The workspace's input plane is a full-screen window. While capturing it has a
window region with a rectangular hole over the frame. Windows itself then
delivers the pointer and the wheel to whatever application is under the
frame, and a click there gives that application the keyboard. Easnap injects
no input, installs no hook and forwards no message. The hole is closed again
when the capture stops, so a held image cannot be lost by a stray click into
the other application.

Easnap's shortcuts work while it has the keyboard focus. After a click in the
frame the focus belongs to the other application; the global capture shortcut
activates the island again.

Windows can be set to send the wheel to the active window instead of the
window under the pointer. Easnap does not change that setting. It reads it
(`WheelFollowsFocus`) to show the advice "click inside the frame once" from
the start, and shows the same advice when a wheel message arrives at Easnap
while the pointer is over the frame.

### Surfaces

`ScrollshotControls` owns two windows, both excluded from capture and both
built from the shared theme, controls and painter:

- the island: **Start Scrollshot** and help before the capture; the stitched
  size, one status line and exactly **Edit, Save, Discard, Copy** during and
  after it;
- the preview: the lower end of the stitched image at reduced width. It is a
  layered, click-through window and never takes input.

`PlaceScrollshotSurfaces` is a pure function of the frame, the monitors and
the surface sizes. The island goes below the frame, else above, right or left
of it, else onto the nearest other monitor, and only when none of that is
possible over the bottom of the frame. The preview goes right or left of the
frame, else onto another monitor, else over a top corner of the frame.

When the island has to lie over the frame it stays clickable. The alternative,
an island that lets clicks through, would leave a full-monitor Scrollshot on a
single display with no way to finish except the keyboard.

### Worker and notifications

`ScrollshotSession` owns its capture provider and one worker thread. The UI
thread never waits for a picture. The worker posts
`kMsgScrollshotProgress` with the session's generation, throttled and at most
one pending; a message from an earlier capture is recognised by its generation
and ignored. Stopping has a three-second barrier. If the worker is still
inside a capture call the island says so and the next action tries again.

### Finishing

Copy, Save and Edit first stop the capture. A failure never costs the image:
a clipboard that refuses a very tall image, a save that fails and a cancelled
Save dialog all return to the island with the image held and a status that
says what happened. Discard ends the session.

### Display changes

A change of the monitor layout cancels a screenshot, because its frozen image
no longer matches the desktop. A stitched image has no such dependency. The
capture stops, the image is kept, the workspace windows are hidden and the
island is placed again for the new layout. Edit rebuilds the workspace. If the
change arrives while a stitched image is being edited, the annotations are
drawn into the image and the session returns to `ScrollshotReview`.

### Text

Every string of this feature comes from the English and Turkish string tables
(`resources/strings_*.rc`, `strings::Get`). Text added later follows the same
path.

## Consequences

- Scrollshot is unavailable before Windows 10 build 19041.
- The image only grows downwards. Scrolling up is recognised and adds nothing.
- Copying and saving a very tall image run on the UI thread; the island shows
  "Copying..." or "Saving..." and is disabled meanwhile.
- On a single display with a frame that leaves no room, the island covers part
  of the content the user sees. It is not in the image.
- The shortcuts depend on focus. This is stated in the help text rather than
  solved with a keyboard hook.
- Screenshots keep the single shading window, which moves with the selection
  as one region update. A screenshot taken with the workspace visible can
  therefore differ from the screen by one level at a few values.
- A recording that starts on a still desktop is read through GDI for its whole
  length, as before. The option above would lift that; it has not been applied
  to recording.

## Validation

- `session_state_transitions`: the transition table, including every
  Scrollshot edge and the edges that must not exist.
- `scrollshot_layout_logic`: surface placement on one, two and stacked
  monitors, a frame across two monitors and a small work area.
- `scrollshot_workspace_production` (private desktop): the three-mode bar, the
  lock policy, the border outside the selection, the four shading windows
  around an exposed frame, the hole in the input plane and its closing,
  keyboard routing, both islands, the click-through preview and forty
  create/destroy cycles without a leaked window.
- `scrollshot_real_desktop` (visible desktop): a second process shows a known
  document; the real wheel turned over the frame, a real click and Page Down
  scroll it; the production session and provider return exactly the pixels
  that process drew: no row missing or repeated, no value changed, nothing
  of Easnap and no pointer in it. Where duplication works at all, no more
  than the pictures taken before the first composed frame may come through
  GDI.
- In the running application, by hand on the secondary display: mode button
  and scissors button, Start by button and by Enter, eight to ten wheel
  notches, Save through the system dialog, Edit with scrolling, zooming and a
  drawn rectangle, Copy by button and by Enter, Discard with and without the
  question. Saved and copied images had the expected size.
