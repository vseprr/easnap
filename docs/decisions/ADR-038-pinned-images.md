# ADR-038 — Pinned images stay on top as screen content

**Status:** Accepted
**Date:** 2026-11-08

## Context

Pin to screen is the first new feature of v1.2: a finished screenshot is kept
on the desktop as a small window above other applications, for reference while
the user works elsewhere. Every other Easnap surface belongs to a capture
session and ends with it; a pinned image outlives the session that made it.
Several existing contracts are affected:

- The screenshot tools island has one row of commands at its preferred width
  of 832 DIPs; the last command ended 28 DIPs short of the right edge, so one
  more button would wrap the row.
- Acquisition keeps the workspace on screen only when every visible window of
  the process is excluded from capture (`ui::CanCaptureVisibleWindows`);
  otherwise it hides the workspace while it reads the desktop. Scrollshot has no
  such fallback and refuses to start
  ([ADR-028](ADR-028-scrollshot-capture-workspace.md)). A visible window that
  is not excluded would therefore make every screenshot flicker and disable
  Scrollshot.
- The overlay windows are topmost. They are created anew for each session, so
  they sit above every topmost window that existed before.
- Windows that run their own message loop declare `platform::ModalLoopScope`
  or `ui::PopupScope`; Exit and deferred cleanup wait for them.

## Decision

### Making a pin

The tools island gets a Pin command between the appearance button and Save,
with the shortcut **Ctrl+T**. The preferred island width grows from 832 to 870
DIPs, which keeps the same 28 DIPs of room after the last command. Like Copy,
Pin fixes the image when it is still provisional, composites the annotations
at the captured resolution, and ends the session as an export
(`ScreenshotEditing → ScreenshotExporting → Idle`). The editor hands the image
and its source rectangle to the coordinator through `Callbacks::on_pin`. When
no pin window can be created, the session stays open with a status notice, as
after a failed copy.

The command is also offered while a stitched image is edited, because it is
the same island. A screenshot is placed exactly over the region it was taken
from, at 100 %. An image of another size, such as a stitched one, is scaled
down to fit the work area of that monitor and centred on it.

### The pin window

`ui/pinned_images` owns the windows; `ui/pin_layout` holds their geometry as
pure functions over desktop pixels, tested on their own. A pin is a top-level
`WS_POPUP` with `WS_EX_TOPMOST | WS_EX_TOOLWINDOW` (no taskbar button, not in
Alt+Tab). It is not layered and has no caption styles: it needs neither, and
the classic caption Windows draws for a layered window with a caption
(ADR-008) cannot appear. Its client area is the image plus a one-DIP border
outside it, in the focus colour while the pin is active and the border colour
otherwise, so the image covers its source region pixel for pixel and the pin
can still be told apart from the desktop under it. The image is drawn with
`StretchDIBits` into a `PaintBuffer`, halftone when reduced and nearest pixel
when enlarged, so that enlarged text stays sharp; `WM_PRINTCLIENT` draws the
same picture for readers such as `PrintWindow`.

A pin has no drop shadow. The class shadow (`CS_DROPSHADOW`) was tried first:
Windows draws it as a separate visible window of the process (class
`SysShadow`), which is neither marked nor excluded, so
`CanCaptureVisibleWindows` turned false while a pin was open. The native test
caught it.

Interaction:

| Input | Action |
|---|---|
| Drag anywhere | Move (native move loop through `HTCAPTION`, as the islands do) |
| Wheel | Zoom in steps from 10 % to 400 % around the pointer |
| Ctrl+1 | Original size |
| Ctrl+C / Ctrl+S | Copy / Save, as from the editor, with the same notices |
| Double-click, Esc | Close |
| Right-click, Shift+F10 | Menu: Copy, Save..., Original size, Close, and Close all pinned images when there is more than one |

A step that would make the shorter side of the image smaller than 24 DIPs, or
its longer side larger than 16 384 pixels, is not taken. A pin closes without a
question: like a copy on the clipboard, it is a finished result the user chose,
not an editing session. Save follows the quick-save setting and the last image
folder exactly as the editor does; the code is shared, not repeated.

### Pins are screen content

A pin is part of what the user sees, so it is part of what Easnap captures:
later screenshots, Scrollshots and recordings contain it like any other window.
What is on the screen is what ends up in the picture, and a pin can be combined
with other content on purpose. To keep a pin out of a capture, the user moves
or closes it first.

For this to work, a pin is marked as screen content (a window property), and
`CanCaptureVisibleWindows` skips marked windows. The workspace therefore stays
visible during acquisition and Scrollshot stays available while pins are open.
The overlay of a new session is created above existing pins, so a pin never
covers the capture workspace. A pin inside a Scrollshot frame does not scroll
with the page; like any other window fixed over a scrolling page, it can keep
the stitching from matching.

### Lifetime and ownership

`ui::PinnedImages` is held by `CaptureCoordinator`, which has the settings and
receives the editor's request; the coordinator only forwards to it and closes
the pins on Shutdown. Pins are independent of the session state: they stay
when a session starts or ends, any number can be open, and the newest is on
top. Exit does not ask about pins, and they are not a reason to hold back a
sign-out.

The pin's menu runs inside `ui::PopupScope` and its Save dialog inside the
existing `ModalLoopScope`, so Exit and cleanup wait for them as for every other
dialog. A pin that is asked to close while its own menu or dialog is open (Close
all from another pin) closes when that menu or dialog returns, and one asked
while its copy or save runs on a worker
([ADR-031](ADR-031-unsaved-work-survives.md#copying-and-saving-a-finished-image-amended-2026-11-09))
closes when that has finished. A closed pin
releases its image at once; the small object left behind is removed the next
time the collection is used.

### DPI, displays and appearance

A pin takes the rectangle Windows suggests on `WM_DPICHANGED`, so its size in
DIPs is kept when it moves to a monitor with another scale; the zoom follows
from the new size. When the monitor layout or a work area changes and less than
48 DIPs of a pin remain on any work area, the pin is moved into the work area of
the nearest monitor. The border follows the theme through
`theme::kChangedMessage`.

## Consequences

- Each pin holds its image at full resolution: 8 MB for a 1920 x 1080
  screenshot, up to about 190 MB for a stitched image at the 48-million-pixel
  limit. There is no limit on the number of pins.
- A pin cannot be edited again; annotations are part of its image.
- Being topmost, a pin also stays above Easnap's ordinary windows, such as
  Settings, the message dialogs and the trim view. The capture workspace and
  its islands are created later and stay above it.
- Pins are not restored after Easnap ends.
- The tools island is 38 DIPs wider. On a work area narrower than 870 DIPs the
  row wraps as before.

## Not in scope

Opacity, rotation, click-through pins, pinning from the Scrollshot four-action
island, a history of closed pins, and an option to keep pins out of captures.

## Validation

`pin_layout_logic` (unit) covers placement, zoom steps and limits, zoom around
the pointer, the zoom of a suggested DPI rectangle and keeping a pin on
screen. `pinned_images_native` creates real pin windows from the production
collection, driven by messages:

- on the isolated desktop, that a pin does not turn `CanCaptureVisibleWindows`
  false while a plain visible window still does (this is where the class
  shadow was caught);
- on a private window station and desktop, the styles, the missing capture
  exclusion and the screen-content mark, the placement over the source, the
  hit test, the picture read through `WM_PRINTCLIENT` at 100 % and enlarged
  with both border colours, wheel zoom around the pointer with half notches,
  Ctrl+1 and Original size, Copy onto that station's own clipboard, a DPI
  change and a display change, Esc, double-click, Close and `WM_CLOSE`, Close
  all from a pin and from the owner, a close request while the pin's own menu
  is open, and the placement of a stitched image.

A private window station counts no window as visible, so the
`CanCaptureVisibleWindows` check needs the isolated desktop; and with no
window left on a desktop `EnumWindows` reports failure, so that check keeps a
hidden window, as Easnap always has its host window. The editor's Pin button,
Ctrl+T and a refused pin are covered in `capture_workspace_production`. Save
from a pin is covered with quick save into a folder of the test (2026-11-09);
its Save dialog, the menu's look and pins on a real screen are not covered by
an automated test.
