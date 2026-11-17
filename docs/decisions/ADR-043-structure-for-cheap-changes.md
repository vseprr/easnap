# ADR-043 — A structure in which a change touches one place

**Status:** Accepted (implemented on the branch `refactor/structure`, draft
pull request, not merged)  
**Date:** 2026-11-18

## Context

On `main` at `38538fa` a small change often had to be made in several files
that did not name each other:

- `editor/screenshot_editor.cpp` had 3,633 lines: the annotation document,
  its drawing, both islands, the brush popup, the text field, the keyboard
  shortcuts and the export. Its control identifiers were bare literals
  (`2000 + tool`, `2100 + i`, `2500` …), and a button's icon was chosen by
  chains of identifier tests.
- `platform/platform_utils.cpp` (1,396 lines) held geometry and monitors,
  graphics devices, images, WIC, the clipboard, the save dialog, folders and
  logging. `ui/native_controls.cpp` (1,598 lines) held every icon in one
  switch, the message dialogs, the custom caption and the styled controls.
  `main.cpp` (874 lines) held the tray icon and menu, the About dialog and
  the application of Settings beside start-up. `app/session_state.cpp` had
  1,909 lines.
- `WM_APP + n` values were defined in 11 files (22 places), with
  hand-written cross-reference comments in `app/session_state.h`; timer and
  subclass identifiers were bare numbers.
- Includes ran upward: `ui/` and `editor/` headers included
  `app/session_state.h`, `media/` used `PaintBuffer` from `ui/`, and
  `capture::SaturatingCounter` tied `media/` and `capture/` both ways.
- Shared colours and metrics were literals in the painters; the annotation
  colours sat beside interface code.

The goal was a structure in which colours, metrics, icons, buttons, tools,
shortcuts, messages and identifiers each have one place, without changing
pixels, strings, the settings format, the preview protocol or any user flow.

## Decision

### Layer order, checked

The source folders form tiers: `platform` 0; `media`, `capture` 1;
`recording`, `scrollshot` 2; `editor`, `ui` 3; `app` 4; `main.cpp` 5. A file
includes its own folder or a lower tier. Between folders of one tier only
`capture` → `media` and `editor` → `ui` are allowed. `ui` and `editor` may
include the pure value headers of `app` listed in `APP_VALUE_HEADERS`, each of
which may include only lower layers or another listed header; the string
module is read only by `ui`, `editor`, `app` and `main.cpp`.
`include_direction_violations` in `scripts/check_repository_hygiene.py`
enforces this, with tests in `scripts/test_repository_hygiene.py`. To get
there, `CaptureMode` moved to `ui/capture_mode.h`, `PaintBuffer` to
`platform/paint_buffer.h` and `SaturatingCounter` to
`platform/saturating_counter.h`.

### One message table and one identifier header

Every `WM_APP + n` of the application is defined in `platform/app_messages.h`,
in the namespace of the window that receives it, with the values unchanged;
`static_assert`s check that the messages one window receives are distinct.
Messages registered by name stay with their users.

Every control, menu, timer and subclass identifier is defined in
`ui/control_ids.h`, grouped by surface, values unchanged; consecutive buttons
are a `ControlRange`. The tray menu commands and the capture hotkey stay in
`resources/resource.h`, which the resource script needs; the result values a
caller gives `ShowChoice` stay with that caller.

### Platform split

`platform_utils` keeps COM and handle lifetime, process setup, logging and
modal loops. The rest moved, one concern per file: `screen_geometry`,
`system_info`, `bgra_image`, `graphics_device`, `image_output` and `files`.
`ApplyMosaicMasked` moved to `editor/mosaic`; `ApplyMosaic`, which had no
caller, was deleted.

### Editor units owned by `ScreenshotEditor`

`ScreenshotEditor` stays the owner and the only class tests and the
coordinator talk to. It holds the units by value and implements the narrow
owner interfaces they call back through (`ToolbarButtonOwner`,
`ContextPanelOwner`, `TextEditorOwner`, `EditorExportOwner`):

| Unit | Holds |
|---|---|
| `editor/annotation_document` | `Annotation`, `AnnotationDocument`: the document, undo and redo, hit testing |
| `editor/annotation_renderer` | `DrawAnnotation`, `DrawSelectionChrome` |
| `editor/annotation_tools.h` | one `ToolDefinition` per tool in `kToolDefinitions` |
| `editor/editor_toolbar`, `editor/toolbar_item` | `EditorToolbar` (the tools island), `ToolbarItem`, `kEditorButtonLooks` |
| `editor/editor_context_panel`, `editor/context_panel_layout` | `EditorContextPanel` (the property island and brush popup), `LayOutContextPanel` |
| `editor/text_annotation_editor` | `TextAnnotationEditor`: the native text field |
| `editor/key_commands.h` | `kKeyBindings`, the plain shortcuts in order of precedence |
| `editor/editor_export` | `CompositeAnnotations`, `EditorExport`: the running copy or save |
| `editor/annotation_palette.h` | the annotation content colours |

`ScreenshotEditor::HandleMessage` only dispatches to named handlers. The
window class name, window titles, control identifiers, the order of `ToolId`
and the public type names are unchanged, because tests rely on them.
`screenshot_editor.cpp` now has 1,688 lines.

### One table row per button, one function per icon

The buttons of the mode island (`kModeButtons`), recording panel, Scrollshot
island, trim window, caption and Settings dialog are rows of one table each;
outside the mode island a row is a `StyledButtonRow` created by
`CreateStyledButton`. Editor tools come from `kToolDefinitions`, editor
options and commands from `kEditorButtonLooks`. Creation order, tab order and
identifiers are unchanged; buttons without a tooltip still have none.

Each icon is a `DrawIcon<Name>` function in `ui/icons.cpp`, chosen from
`kIconPainters`. The message dialogs moved to `ui/message_dialog.cpp` and the
custom caption to `ui/window_chrome.cpp`; `ui/native_controls.cpp` keeps the
styled controls (971 lines).

### The theme owns shared values; pixels do not change

`ui/ui_theme.h` names the derived tokens (`Palette::separator`,
`Palette::scroll_thumb`, filled by `WithDerivedTokens`, keeping the existing
high-contrast difference between them), the theme-independent chrome colours
(`kSwatchCheck*`, `kReticleLight`, `kSelectionHandleFill`) and the shared
metrics: icon stroke, button metrics used by both `PaintButtonFace` and
`ButtonContentWidth`, animation timings, grips, island gap and margin,
caption, menus and font roles. Annotation colours moved to
`editor/annotation_palette.h`, which does not include the theme (ADR-016).

The rule is pixel identity: no field of `Palette` is reordered, values that
differ stay separate (the 24 and 28 DIP grips), and no DIP-to-pixel formula
changed. `ui_control_raster_parity` (Fast tier, 602 cases: every icon at four
sizes in three inks, 30 button styles and states in both palettes at two
scales, separator, grips, panel and the palette fields) was added before the
first move, with hashes recorded from the unchanged painters;
`ui_material_raster_parity` already guarded the material. Annotation pixels
are guarded by `AnnotationRenderingIsStable`: shapes and effects in
`capture_workspace_production` on every machine, text in
`annotation_text_rendering` where the machine's text rendering fingerprint
matches a recorded one (skipped with 77 elsewhere).

[Change map](../ui/change-map.md) lists the file and symbol for each kind of
change.

### Coordinator: pure decisions, no `ScrollshotFlow`

The Scrollshot status text (`ChooseScrollshotStatus`), the reaction to a
display change (`DecideDisplayChange`) and the outcome of a finished image
copy or save (`ChooseImageExportOutcome`, shared with the editor) became pure
functions, checked by the unit test `coordinator_decisions`. A
`ScrollshotFlow` would have needed more crossings than `TrimFlow` and was not
created; the measurement is in
[ADR-039](ADR-039-coordinator-trim-flow.md#amendment-2026-11-18-scrollshot-measured-again).
`session_state.cpp` has 1,876 lines.

### Shell split between `ui/` and `app/`

The notification icon (`ui/tray_icon`), its menu (`ui/tray_menu`,
`ShowTrayMenu` from a `TrayMenuState`) and the About dialog
(`ui/about_dialog`) take plain data and do not read the coordinator.
`app/tray_shell` holds the capture shortcut, the tray commands and the
application of Settings. `main.cpp` keeps start-up, the host window and its
message loop (469 lines). The window procedures of the overlay, the trim
timeline and the host window were split into named handlers.

### Window class helper

`platform/window_class.h` (header-only, so the preview host and the tests use
it without linking more) provides `RegisterWindowClass`, `BindWindowOwner`,
`UnbindWindowOwner` and `BoundWindowProc`. Every custom window class
registers and binds its owner through it; nothing else of a window changed.
The `ChromeParentProc` subclass and its `WM_NCCALCSIZE` path
([ADR-024](ADR-024-native-trim-frame.md)) were left as they were.

### Per-folder static libraries: not adopted

Linking each source folder as a static library would replace about 500
`../src/` lines of `apps/easnap-desktop/tests/CMakeLists.txt` and compile
each production file once. The selection of tests to run (ADR-025) already
follows static-library links, but each test's fingerprint rests on the hash
of its executable. A rebuilt `.lib` relinks every test that links it, and without
`/Brepro` each relink changes the executable's hash, so one edit in a folder
would rerun every test linking that folder where today only the tests that
list the edited file rerun: selection would degrade from per test to per
library. Revisit after `/Brepro` is added to the project options and two
clean builds are shown to give identical executable hashes.

## Consequences

- A colour, metric, icon, button, tool, shortcut, message or identifier is
  changed in one named place ([change map](../ui/change-map.md)); a wrong
  include direction fails the hygiene check in CI.
- A visible change to a shared painter now fails `ui_control_raster_parity`
  until its recorded hash is updated, deliberately, in the same change.
- Declared differences, all reviewed: the tools island's hit test looks at
  its own items only, which removes a latent case in which, at a fractional
  scale, a click on the edge of a disabled command button could pick a
  property item; `OnDisplayChanged` reads the monitor layout on every call
  instead of in three branches; a key binding without a command no longer
  compiles. Dead code was removed: `ApplyMosaic`,
  `AnnotationDocument::HasRedactions` and English button names in the editor
  that the string tables always replaced.
- Left as they are: `screenshot_editor.cpp` (1,688 lines) with
  `OnToolbarActivated` at about 99 lines; `ContextPanelOwner` with 22 methods
  and `TextEditorOwner` with 17; `TrimWindow::HandleMessage` (87 lines, near
  the ADR-024 frame path); per-tool decisions in `DrawAnnotation` and
  `Annotation::HitTest`/`bounds`/`Handles`; `annotation_document.cpp`
  includes `annotation_tools.h`, which carries interface identifiers and
  icons; some surface-local layout literals are still unnamed;
  `ui/pinned_images` keeps its own copy of the export outcome choice; the
  capture workspace notice keeps a colour flag in `GWLP_USERDATA`.

## Validation

- Every commit: Release build, the Fast tier and the format check on the
  host. Phase 2 also ran Standard on the host (56/56); after the owner asked
  on 2026-11-17 that Standard run only in the lab, the host ran Fast only.
  One host run at `bf59714` had `trim_flow_production` time out under
  parallel load; the lab Standard run at `7a39ece` passed it in 14 s.
- Lab Standard, green: after Phase 4 at `7a39ece` (56/56), Phase 5 at
  `51eeedf` (57/57), Phase 7 at `c616335` (59/59) and Phase 8 at `2d16e71`
  (59/59).
- Lab Full, green: after Phase 6B at `9fb06c2` (67/67;
  `scrollshot_realistic_desktop` failed once on torn pictures, the known
  [ADR-027](ADR-027-scrollshot-stitching-engine.md#known-weakness-2026-11-09)
  weakness, and passed on retry), Phase 6C at `eccffd7` (68/68, no retries)
  and Phase 9 at `0eb5040` (69/69, no retries; capture paths Desktop
  Duplication and WGC as expected). `annotation_text_rendering` compared text
  in the VM: its fingerprint matches the recorded machine.
- `coordinator_decisions` makes 169 checks, including all 40 Scrollshot status
  combinations.
- An independent review of each phase (5, 6A, 6B, 6C, 7, 8, 9) compared moved
  bodies with the originals and accepted it.
- With the text guard as its own test, `capture_workspace_production` and
  `annotation_text_rendering` running in parallel failed 2 of 6 host runs: both
  test processes moved their editor thread to the same desktop, so one
  found the other's text field. `ff0267a` names each private desktop after
  the process and thread and gives the three tests that copy there a shared
  `RESOURCE_LOCK`; 30 parallel repeats passed on the host, and both passed in
  the lab Full run at `1d188c9`.
- Phase 11 split three timing-sensitive functions by extraction only:
  `WasapiAudioProvider::CaptureThread` (428 to 64 lines; its COM objects and
  event handle are members of `WasapiCaptureSession`, declared in the order
  they are acquired, so they are released in the old order and before
  `CoUninitialize`), `MixerMain` (232 to 60) and `MfPlayPreview::OnPlayerEvent`
  (227 to 61). The review compared every call's order, the lock scopes, the
  exception boundary and the lifetimes, and accepted it; two temporaries of
  the mixer are now freed slightly earlier, outside any lock.
- Lab Full at `1d188c9` (after Phase 11), green: 69 passed, every audio,
  recording and preview test among them. `island_capture_exclusion` failed
  once and passed on retry: the foreign pixels covered a 455 x 239 px
  rectangle above the taskbar at the right edge from the first frame, before
  the recording island was created, where Windows 10 shows its notifications;
  the island sits in the centre of the screen. The order of creation, capture
  exclusion and showing of the recording island is the same as on `main`. Its
  four earlier Full runs on this branch and the retry had no foreign pixels.
- clang-tidy with the CI's checks: 0 warnings in 83 files. Both hygiene
  scripts pass.

## Not validated

- No test compiles `main.cpp` or the tray shell. To check by eye in the
  running application: a tray click starts a capture and a right click opens
  the menu; the menu's text, order, check marks and greying in both
  languages; Settings: the shortcut after Cancel and after OK, language and
  theme, a taken shortcut refused, folders and quick save; About: version,
  shortcut, Copy details in English, links; Exit asks and is refused while a
  dialog is open; the icon returns after Explorer restarts; a second instance
  starts a capture.
- `ui_control_raster_parity` does not hash the high-contrast palette, button
  captions (every case has an empty caption, so the caption insets and
  `ButtonContentWidth` were checked by reading), the scroll thumb or the faces
  drawn only in real windows (menus, combo boxes); these were compared by
  reading the code.
- Phase 11 rests on the lab: the host ran no audio or preview test. The
  lab VM has a virtual audio cable and no GPU, so real audio devices and
  hardware decoding were not exercised.
- Notifications in the lab VM are not turned off; that the one retry of
  `island_capture_exclusion` came from one is the most likely explanation, not
  a proven one.
