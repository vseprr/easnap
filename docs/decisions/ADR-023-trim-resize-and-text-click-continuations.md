# ADR-023: Trim resize surfaces and text click continuations

Date: 2026-09-19
Status: Implemented and accepted in the closed 2026-09-21 UI correction session.  
**Amended by:** [ADR-024](ADR-024-native-trim-frame.md) (top-level redraw and copy-bits policy)  
New windows follow the [implementation guide](../ui/window-implementation-guide.md).

## Context

Desktop review rejected the previous trim resize behavior: stale/white pixels,
flicker, excessive top spacing and unusable extreme sizes. The user explicitly
permits sensible minimum dimensions. Text editing also created an unwanted box
when a blank click was followed by the native double-click continuation.

## Decisions

- Supersede ADR-022's unrestricted manual size decision. Keep a 480 x 400 client-DIP
  minimum with ADR-024's native outer-frame allowances, capped by the monitor work area. Preserve real DPI, every action and
  source-driven initial sizing. Extreme source ratios fit inside a usable UI.
- The video allocation starts at the 40 DIP custom caption boundary. Time and
  selected range sit below the timeline on two stable rows. Their height does
  not change at a horizontal breakpoint. Extra preview space uses the theme's
  gray panel color; the fitted video preserves source aspect.
- Resizable chrome opts into DWM rounding; it no longer replaces an application
  window region on every resize. Unsupported systems keep a rectangular frame.
  The original WVR_REDRAW/SWP_NOCOPYBITS policy is superseded by
  [ADR-024](ADR-024-native-trim-frame.md), following the user's left/top resize
  regression report. Batch caption button positions and repaint owner-thread
  surfaces after positioning.
  Background-erasure requests actually fill the exposed gray backing surface.
  No system white class brush is used for trim, timeline or video backing.
- Never synchronously paint or position the foreign preview helper from the
  owner. Preserve its existing asynchronous resize transport and media lifetime
  boundaries. Parent and video child retain clipping separation.
- Create the video backing HWND with its TrimWindow pointer. The generic
  control factory passed null, so VideoProc never reached the existing
  paint/erase/size handlers. The regression now checks the production video
  background handler as well as the outer window and caption.
- A blank WM_LBUTTONDBLCLK is a continuation, never a second text creation
  request. It may close a still-active editor. A later ordinary blank press
  with neither active editor nor selection can create text. Single-click text
  selection and blank-click deselection retain that rule across box switches.
  Double-clicking an existing text/tag
  still activates native editing and word selection. Caret logic is unchanged.

## Basis and validation boundary

Microsoft documents [client-pixel preservation and WVR_REDRAW](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-nccalcsize),
[minimum tracking sizes](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-getminmaxinfo),
[DWM rounding and the restrictions on window regions](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-rounded-corners),
and [separating GDI and DirectX drawing into different windows](https://learn.microsoft.com/en-us/windows/win32/dwm/bestpractices-ovw).
These support the implementation choices; they are not a media-editor-specific
prescribed layout.

Native tests exercise Text and Tag click continuations, real H.264 preview
windows, theme-colored erase surfaces, repeated resize/maximize/minimize/restore,
caption gaps, control retention, message/progress states and aspect geometry.
They do not establish perceived flicker on every GPU. GDI/PrintWindow snapshots
cannot be used to judge the out-of-process Direct3D video surface.
