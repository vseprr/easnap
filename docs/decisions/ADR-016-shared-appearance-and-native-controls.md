# ADR-016 — Shared appearance and accessible native controls

**Status:** Accepted implementation decision  
**Amended by:** [ADR-017](ADR-017-neumorphic-editor-and-editable-annotations.md) (neumorphic material, owner-drawn menus); [ADR-018](ADR-018-editor-interaction-corrections.md) (500 ms mode fade); [ADR-019](ADR-019-stable-native-paint-and-custom-window-chrome.md) (custom message dialog and caption); [ADR-024](ADR-024-native-trim-frame.md) (native frame for resizable windows); [ADR-033](ADR-033-settings-shortcut-and-launch-behaviour.md) (appearance can follow Windows)  
**Date:** 2026-09-14

## Context

The user authorized modernization across the existing capture, recording and
completion surfaces, using the supplied Lark workflow proportions and restrained
neumorphic depth. Phase 3/3.3 acceptance remains closed. This presentation effort
precedes, and does not constitute, Phase 4 release hardening.

## Decision

`AppSettings::appearance_theme` stores `Light` or `Dark` in the existing Easnap
INI. Light is the default for missing/invalid values. The coordinator applies and
saves appearance immediately. It saves its accepted settings object: pending
recording configuration remains separate and is accepted only after Start succeeds.
Persistence failure uses the existing logging path and is never reported as saved.

`ui_theme` supplies semantic colors, dimensions, Segoe UI fonts and small GDI
panel/control drawing operations. The preference is distinct from the effective
palette: Windows high contrast temporarily replaces colors with system colors
and removes decorative shadows without rewriting the preference. Annotation,
captured-image and sampled-pixel colors are content and never use theme tokens.

Appearance is a synchronous presentation-only message on the owning UI thread,
including retained hidden windows. It does not invoke layout, capture, state
transitions, document mutation, media transport or export cancellation. Child
windows in the separate preview process are excluded from the broadcast.
`WM_SETTINGCHANGE` with `SPI_SETWORKAREA` is handled like a display change:
it cancels a workspace only when the monitor bounds or DPI differ from those
recorded at session start, so a work-area change alone does not cancel
(v1.0.1). Color/theme
notifications refresh presentation. Display/DPI changes retain their existing
geometry/invalidation behavior.

The mode selector, screenshot tools and recording/trim actions are actual Win32
BUTTON controls, subclassed for shared rendering. Their native push, radio and
toggle roles, enabled/check states, focus and accessibility providers remain.
Names and geometry icons replace font-dependent pictographs. Visible bounds,
hit areas, layout, tooltips and input exclusion use the same child HWND geometry.
Enter activates the focused control; it must not silently start/save while a
different control has focus. Native Appearance menus retain Windows menu behavior.
Menu scope excludes popup windows from capture, suppresses input leakage into
the workspace and restores a surviving invoker's focus.

The screenshot **toolbar only** changes from a D2D/DXGI swap chain to GDI with
native children. Small static controls do not need synchronized presentation;
this provides native keyboard/automation semantics without a second editor.
The existing Direct2D image renderer, one immutable source, annotation document,
history, pending input, mosaic and full-resolution compositor remain. ADR-001's
GDI overlay rationale and ADR-015's damage-limited frame path remain in force.
This amends ADR-003's toolbar rendering detail, not its capture/export contract.

Recording uses a compact configuration panel and a separate layout of the same
HWND for active, paused and finalizing states. Independent System audio and
Microphone toggles map to the existing four-value audio source. GIF hides audio
controls while preserving the pending MP4 choice. Failed Start retains the
configuration and workspace for retry; the coordinator keeps ownership of partial
session cleanup and recovery. No timer limit or unsupported source is added.

Trim keeps the production MFPlay helper and controller. It reflows at the actual
DPI instead of reducing font and hit-target scale to fit a row. Its timeline
exposes In, playhead and Out as named UI Automation RangeValue children, in
seconds, with production bounds and mutation rules. Keyboard focus moves among
the three handles; arrow keys use the same range/transport rules. Providers use
COM apartment marshaling and detach callbacks before HWND destruction.

## Consequences and boundaries

- UI assets are vector geometry in code; no web framework, bitmap theme atlas,
  capture engine, encoder or second editing model is introduced.
- Windows file pickers and the preview renderer retain
  Windows ownership. A dark app preference does not promise custom dark styling
  of every operating-system dialog.
- The theme has no full-screen blur, additional dim layer or per-frame theme
  animation. The 500 ms mode retirement remains the only mode fade.
- Unsupported Camera, Mouse Effects, OCR and device pickers remain
  absent.
- See the [design system](../ui/design-system.md) for the implemented values
  and validation limits; implementation is not manual release acceptance.

## Platform references

- [Windows high contrast guidance](https://learn.microsoft.com/en-us/windows/win32/controls/supporting-high-contrast-themes)
- [UI Automation provider overview](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-providersoverview)
- [ProviderOptions and COM threading](https://learn.microsoft.com/en-us/windows/desktop/api/uiautomationcore/ne-uiautomationcore-provideroptions)
- [Fragment runtime identifiers](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcore/nf-uiautomationcore-irawelementproviderfragment-getruntimeid)
