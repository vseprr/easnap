# ADR-033 — Settings, the capture shortcut and how Easnap starts

**Status:** Accepted
**Date:** 2026-10-13

## Context

v1.0.2 had no settings window. The shortcut could be changed only by editing
`settings.ini`, a taken shortcut produced a modal error at every start, a
second start showed "already running", the theme was Light or Dark and never
followed Windows, and nothing could start Easnap at sign-in.

## Decision

### The Settings dialog

Tray menu, "Settings...", while no capture is in progress. It is a fixed modal
dialog on the path of the message dialog (shared chrome, shared styled
controls, real default and cancel buttons; see the
[window implementation guide](../ui/window-implementation-guide.md)). It edits
a copy of the values (`ui::SettingsDialogModel`); the shell applies them when
the dialog is accepted and Cancel changes nothing. It opens on the display
under the pointer.

| Setting | Stored as | Behaviour |
|---|---|---|
| Language | `Language=System\|English\|Turkish` | [ADR-032](ADR-032-interface-text-in-string-tables.md) |
| Appearance | `AppearanceTheme=System\|Light\|Dark` | `System` follows the application mode of Windows, also while Easnap runs. A missing key means `System`. |
| Capture shortcut | `HotkeyModifiers`, `HotkeyVirtualKey` | see below |
| Start with Windows | a value in the user's `Run` key | see below |
| Include the cursor | `CaptureCursor` | as before |
| Screenshots folder, Recordings folder | `ImageFolder`, `VideoFolder` | Where Save dialogs open. Unset: the folder used last (`LastImageDirectory`, `LastVideoDirectory`). |
| Save screenshots without asking | `QuickSaveImages` | Only with a screenshots folder. Save writes `Easnap_<time>.png` there, never over an existing file, and says where. |

`settings.ini` is written as UTF-16 with a byte-order mark so that folder names
keep their characters. `LastSaveDirectory` of older versions is read once and
replaced by the two new keys.

### The capture shortcut

- The shortcut button waits for the next combination. Ctrl, Alt or the Windows
  key must be part of it; Shift alone would take a capital letter away from
  every application. Esc keeps the current shortcut.
- Whether a combination is free is found out by registering it. While the
  dialog is open Easnap gives its own shortcut up: Windows hands a registered
  combination to its owner and never to the window that wants to read it.
- A combination another application holds is refused in the dialog, by name.
- When the stored shortcut cannot be registered at start, Easnap says so in a
  notification, not in a dialog, and the icon's tooltip and menu say that a
  click on the icon captures. Settings is where it is changed.
- While any of the application's dialogs is open, the shortcut brings that
  dialog forward and starts nothing.

### Starting

- **Start with Windows** writes `"<path>\Easnap.exe" --startup` to
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `Easnap`. The
  check box shows whether that value starts this executable; an entry left by
  a copy in another folder is not this copy's. A refused write is reported.
- A start with `--startup` is quiet: no notification.
- **Starting Easnap again asks the running Easnap to capture.** The second
  process posts a registered message to the host window, passes on its right
  to take the foreground and ends. With `--startup` it does nothing.
- The first start on a computer says once where Easnap lives and what the
  shortcut is (`FirstRunNoticeShown`).
- Easnap refuses to start below Windows 10 version 2004 (build 19041), with
  the build number it found. Its windows cannot be kept out of a capture
  there, and nothing is tested there.

### About and updates

About shows the version text, the shortcut and the Windows build. "Copy
details" puts version, Windows build, language, shortcut and display layout on
the clipboard for a bug report. "Check for updates" opens the releases page in
the browser. Easnap itself still opens no network connection.

## Consequences

- `platform/startup_entry` takes the registry key as a parameter; its test
  writes to a key it creates and removes, and only reads the real `Run` key.
- The Settings dialog is driven by messages in `data_safety_questions` and
  measured in both languages by `text_fits_in_every_language`.
- A portable copy that is moved after "Start with Windows" was set leaves an
  entry that points to the old place; Settings in the moved copy shows the box
  cleared and setting it again repairs the entry.
- Not implemented: a separate shortcut per mode, a choice of default mode, and
  file-name patterns.
