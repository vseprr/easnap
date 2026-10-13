# Troubleshooting

Answers to what comes up most often. What Easnap does in detail is in
[user flows](user-flows.md). Entries marked *beta* apply to v1.1.0-beta.1 and
later, not to v1.0.2.

## Starting and installing

**Windows warns about an unknown publisher.** The binaries and the installer
are unsigned. Compare the SHA-256 value of your download with the `.sha256`
file of the same release, or run
`gh attestation verify <file> --repo vseprr/easnap`.

**Nothing appears after starting Easnap.** It has no main window. Its icon is
in the notification area, possibly among the hidden icons behind the arrow.
The first start on a computer says so in a notification (*beta*).

**"Easnap needs Windows 10 version 2004 (build 19041) or later."** (*beta*)
Earlier builds cannot keep Easnap's own windows out of a recording or a
scrolling screenshot. There is no workaround.

**The portable copy does not work after extracting.** Extract the whole ZIP.
`bin\EasnapPreviewHost.exe` must stay beside `bin\Easnap.exe`. If a security
product removed it, the preview of a recording cannot open; see below.

## The shortcut

**Ctrl+Shift+A does nothing, or a notification says another application uses
it.** Click the Easnap icon to capture, and choose another combination under
**Settings...** in the menu of the icon (*beta*). In v1.0.2, exit the other
application or change `HotkeyModifiers` and `HotkeyVirtualKey` in
`%LOCALAPPDATA%\Easnap\settings.ini` while Easnap is not running.

**Settings does not accept my combination.** (*beta*) It needs Ctrl, Alt or
the Windows key; Shift alone is refused. A combination that another
application or Windows itself holds is refused with its name.

**The shortcut does nothing while a dialog of Easnap is open.** (*beta*) That
is intended: the dialog comes forward and has to be answered first.

## Scrollshot (*beta*)

**The image stops growing, or the island says "Easnap lost track".** Scroll
back up a little until the picture in the frame matches the end of the image,
then continue more slowly. Easnap only joins pictures it can place without
guessing.

**Parts repeat or are missing.** Content that moves by itself inside the frame
(video, animations, a blinking caret, a list that loads more while it
scrolls) gives the stitcher nothing stable to compare. Narrow the frame to the
part that only moves when you scroll.

**Keyboard shortcuts of Scrollshot do nothing.** They work while Easnap has
the keyboard. Press the capture shortcut once: it brings the island forward.

**The wheel does not scroll the page.** The pointer has to be inside the
frame. If Windows is set to scroll only the window that has the focus, the
island says so: click inside the frame once.

## Recording

**Record says it needs the Media Feature Pack.** (*beta*) You are on an N or KN
edition of Windows, which comes without Media Foundation. Install the Media
Feature Pack from the optional features in Windows Settings and start Easnap
again. Screenshots and Scrollshot work without it. v1.0.2 does not start on
these editions without the pack.

**"This region is larger than H.264 video allows."** (*beta*) H.264 limits the
picture size; the message shows the size of your region. Choose a smaller one.

**The preview does not open after Stop.** The recording is finished and kept.
In the beta, close the window: Easnap offers to save the recording as it is,
as an MP4 file. If the message names `EasnapPreviewHost.exe`, that file is
missing from the Easnap folder: extract the ZIP again or reinstall.

**Easnap closed, crashed or the PC shut down before I saved a recording.**
(*beta*) A recording that was already finished is offered at the next start:
Save, Delete or Not now. A recording that was still running when Easnap
stopped cannot be recovered.

**No sound in the recording.** Choose System audio, Microphone or both in the
recording panel before Start. GIF has no sound.

## Display

**Colours look washed out or too bright on an HDR display.** HDR output has
not been tested. Please report it with the details from About.

**Text or controls are cut off.** Report it with your scale factor and
resolution. Layout is tested on 1920x1080 at 125% and by calculation for other
sizes.

## Reporting a problem

1. **About, then Copy details** (*beta*) puts the version, Windows build,
   language, shortcut and display layout on the clipboard. Paste that into the
   issue.
2. **Open data folder** in the menu of the icon leads to the log
   (`logs\easnap.log`) and, after a crash, to `crashes\*.dmp` (*beta*). The
   log holds no file name, window title or typed text. A crash report holds
   the call stacks of the program and can contain fragments of what was being
   handled: look at it before you attach it.
3. Reproduce the problem on a test window, not on private content.

Security problems go through
[private vulnerability reporting](https://github.com/vseprr/easnap/security/advisories/new),
not a public issue.

## Settings and data

Everything is under `%LOCALAPPDATA%\Easnap`: `settings.ini`, `logs`,
`crashes` and `temp` (recordings that were not saved yet).

- To start over, exit Easnap and delete `settings.ini`.
- Edit `settings.ini` only while Easnap is not running; Easnap writes the file
  when a setting changes and would overwrite your edit.
- Uninstalling the beta asks whether settings, log and crash reports should be
  removed. Recordings that were not saved are kept.
