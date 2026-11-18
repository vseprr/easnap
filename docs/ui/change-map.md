# Change map

Where a common change is made. Each row names the file and the symbol to edit;
paths are relative to `apps/easnap-desktop/src` unless they start with
`apps/`, `scripts/` or `resources/` (`apps/easnap-desktop/resources`). The
structure behind it is recorded in
[ADR-043](../decisions/ADR-043-structure-for-cheap-changes.md).

Three rules hold for every row:

- Text the user reads goes into both string tables
  (`resources/strings_en.rc`, `resources/strings_tr.rc`); a table row holds
  its string id, never a literal
  ([ADR-032](../decisions/ADR-032-interface-text-in-string-tables.md)).
- A move or rename of a theme value keeps every pixel. `ui_control_raster_parity`
  and `ui_material_raster_parity` compare hashes recorded from the painters; a
  deliberate visual change updates the recorded hash in
  `apps/easnap-desktop/tests/test_native_ui.cpp` in the same change, and says so.
- Annotation pixels are guarded by `capture_workspace_production` and
  `annotation_text_rendering` (`AnnotationRenderingIsStable` in
  `apps/easnap-desktop/tests/test_capture_workspace.cpp`).

## Theme colours

| To change | File | Symbol |
|---|---|---|
| A light or dark interface colour | `ui/ui_theme.cpp` | `NormalPalette` (positional: the values follow the field order of `Palette`) |
| A new palette role | `ui/ui_theme.h`, `ui/ui_theme.cpp` | `Palette`, then `NormalPalette` and the high-contrast palette in `RefreshSystemColors` |
| High-contrast colours | `ui/ui_theme.cpp` | `RefreshSystemColors` |
| Group separator or document scroll thumb | `ui/ui_theme.cpp` | `WithDerivedTokens` (`Palette::separator`, `Palette::scroll_thumb`; only the separator takes the text colour in high contrast) |
| Check mark on a colour swatch | `ui/ui_theme.h` | `kSwatchCheckDark`, `kSwatchCheckLight`, `kSwatchCheckThreshold` |
| Magnifier reticle | `ui/ui_theme.h` | `kReticleLight` (inner ring `kDim`) |
| Fill of the editor's selection handles | `ui/ui_theme.h` | `kSelectionHandleFill` |
| Overlay dimming and transparency key | `ui/ui_theme.h` | `kDim`, `kDimAlpha`, `kChromeColorKey` |

## Theme metrics

All in `ui/ui_theme.h`, in device-independent pixels unless noted.

| To change | Symbol |
|---|---|
| Icon grid and stroke | `kIconGrid`, `kIconStroke`, `kIconOutlineStroke` |
| Styled button insets, icon box, chevron, check marker, focus underline | `kButtonFaceInset` … `kButtonFocusInset` (used by `PaintButtonFace` and `ButtonContentWidth` alike), `kSwatchRadius` |
| Button and island animation timings (ms) | `kButtonSettleMs`, `kButtonPressMs`, `kButtonCheckMs`, `kButtonHoverMs`, `kAnimationTickMs`, `kIslandFadeMs` |
| Grips | `kIslandGripWidth` (mode, editor and Scrollshot islands), `kPanelGripWidth` (recording panel); two values on purpose |
| Gap and margin of islands | `kIslandGap`, `kIslandMargin` |
| Custom caption | `kCaptionHeight` (read through `kWindowChromeHeight` in `ui/window_chrome.h`), `kCaptionButtonSize`, `kCaptionButtonGap`, `kResizeEdge`, `kCaptionTitleInset`, `kCaptionTitleReserveResizable`, `kCaptionTitleReserveFixed` |
| Owner-drawn menus | `kMenuItemHeight` … `kMenuArrowSize` |
| Tooltip width | `kTooltipMaxWidth` |
| Font sizes | `kBodyFontSize`, `kHeadingFontSize`, `kDialogFontSize`, `kTrimFontSize`; fonts are made by `CreateUiFont` |
| Panel and control radii, mode buttons, handles, magnifier | `kPanelRadius`, `kControlRadius`, `kPaletteButton*`, `kHandleSize`, `kHandleHitRadius`, `kMagnifier*` |

Layout values that only one surface uses stay in that surface's file.

## Icons

| To change | File | Symbol |
|---|---|---|
| How one icon is drawn | `ui/icons.cpp` | its `DrawIcon<Name>` function (for example `DrawIconPin`); the three text icons share `DrawTextGlyph` |
| Add an icon | `ui/icons.h`, `ui/icons.cpp` | a value of `Icon` before `Pin` (or move the anchor of `kIconCount`), a `DrawIcon<Name>` function and its row in `kIconPainters` |

## Buttons of each surface

Styled buttons outside the editor are described by one `StyledButtonRow` each
(`ui/native_controls.h`: identifier, style, caption, look, tooltip and the
second state of a two-state button), created by `CreateStyledButton` and given
a tooltip by `AddButtonTooltip`.

| Surface | File | Symbol |
|---|---|---|
| Editor tools | `editor/annotation_tools.h` | `kToolDefinitions` (one `ToolDefinition` per `ToolId`: identifier, place on the island, separator, name, icon) |
| Editor options and commands (icon, tooltip, role) | `editor/toolbar_item.cpp` | `kEditorButtonLooks`; the commands are `CommandId` in `editor/toolbar_item.h` |
| Which options the property island shows for a tool | `editor/annotation_tools.h` | `ToolDefinition::properties` (`ToolProperties`); laid out by `LayOutContextPanel` in `editor/context_panel_layout.cpp` |
| What an editor button does | `editor/screenshot_editor.cpp` | `ScreenshotEditor::OnToolbarActivated` |
| Annotation colour swatches | `editor/annotation_palette.h` | `kAnnotationSwatchColors`; their names are the `colors` list in `CreateToolbarButton` (`editor/toolbar_item.cpp`) |
| Stroke thickness and brush size choices | `editor/toolbar_item.h` | `kThicknessOptions`, `kBrushSizeOptions` |
| Mode island | `ui/overlay.cpp` | `kModeButtons` (`ModeButtonRow`) |
| Recording panel | `ui/recording_controls.cpp` | `kRecordingButtons` |
| Scrollshot island | `ui/scrollshot_controls.cpp` | `kScrollshotButtons` |
| Trim window | `ui/trim_window.cpp` | `kTrimButtons` |
| Caption buttons | `ui/window_chrome.cpp` | `kCaptionButtons` |
| Settings dialog | `ui/settings_dialog.cpp` | `kSettingsButtons` |
| Message dialogs | `ui/message_dialog.cpp` | `MessageDialogProc` (OK/Cancel from the flags); named answers come from the caller through `ShowChoice` and `ConfirmDestructive` |
| Tray menu items, order, check and greying | `ui/tray_menu.cpp` | `ShowTrayMenu`, from `TrayMenuState` |
| Tray menu commands | `resources/resource.h`, `app/tray_shell.cpp` | `IDM_TRAY_*`; handled in `OnTrayMenuCommand` |

## Editor tools

| To change | File | Symbol |
|---|---|---|
| Per-tool facts: text box, base effect, selectable, pointer, property island | `editor/annotation_tools.h` | `ToolDefinition`; read through `IsTextTool`, `IsBaseEffect`, `IsSelectable`, `PropertiesOf`, `CursorFor` |
| How an annotation is drawn | `editor/annotation_renderer.cpp` | `DrawAnnotation` (switch on the tool) |
| Selection frame and handles as drawn | `editor/annotation_renderer.cpp` | `DrawSelectionChrome` |
| Hit test, bounds and handle points | `editor/annotation_document.cpp` | `Annotation::HitTest`, `Annotation::bounds`, `Annotation::Handles` |
| Add a tool | `editor/annotation_document.h`, `editor/annotation_tools.h`, `ui/control_ids.h` | a `ToolId` value, its `ToolDefinition`, the count of `kToolButtons` and, after `Watermark`, the anchor of `kToolCount`; then the three places above |

## Keyboard shortcuts

| To change | File | Symbol |
|---|---|---|
| A plain editor shortcut (key, Ctrl, Shift, document only) | `editor/key_commands.h` | a `KeyBinding` row in `kKeyBindings`; the first matching row wins |
| A new shortcut command | `editor/key_commands.h` | a `KeyCommand` value (every row must name one) |
| What a shortcut command does | `editor/screenshot_editor.cpp` | `ScreenshotEditor::RunKeyCommand` |
| Keys that depend on the editing state (Esc, Delete, nudges, Tab) | `editor/screenshot_editor.cpp` | `ScreenshotEditor::OnKeyDown`, before the table |

## Messages, identifiers and windows

| To change | File | Symbol |
|---|---|---|
| A private window message (`WM_APP + n`) | `platform/app_messages.h` | the constant in the namespace of the window that receives it; the `static_assert` lists keep each window's messages distinct |
| Where the host window handles a message | `main.cpp` | `HostWindowProc` |
| A control, menu, timer or subclass identifier | `ui/control_ids.h` | the constant of its surface; consecutive buttons are a `ControlRange` |
| Register a window class or bind a window to its C++ owner | `platform/window_class.h` | `RegisterWindowClass`, `BindWindowOwner`, `UnbindWindowOwner`, `BoundWindowProc` ([window guide](window-implementation-guide.md)) |

## Layers and new files

| To change | File | Symbol |
|---|---|---|
| Which folder may include which | `scripts/check_repository_hygiene.py` | `LAYER_TIERS`, `SAME_TIER_INCLUDES`, `STRINGS_USERS`, checked by `include_direction_violations`; tests in `scripts/test_repository_hygiene.py` |
| An `app/` header that `ui/` and `editor/` may include | `scripts/check_repository_hygiene.py` | `APP_VALUE_HEADERS` (a listed header may include only lower layers or another listed header) |
| A new `.cpp` in the application | `apps/easnap-desktop/CMakeLists.txt` | `EASNAP_SOURCES` |
| The same `.cpp` in the tests | `apps/easnap-desktop/tests/CMakeLists.txt` | every `add_executable` that compiled the file the code came from, or that now calls it |
