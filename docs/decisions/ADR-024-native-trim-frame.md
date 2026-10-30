# ADR-024: Preserve the native trim resize frame

Date: 2026-09-21
Status: Implemented and accepted by the user on 2026-09-21; session closed.

Required implementation sequence for future windows: [window implementation guide](../ui/window-implementation-guide.md).

Transient artifacts while the trim window is resized or maximized are a known,
unresolved limitation; see [Known limitation](#known-limitation-transient-artifacts-while-resizing).

## Context

At 125% scaling, right/bottom resize was usable but changing the left/top
origin displaced the previous client surface before the new layout appeared.
Maximize/restore also exposed mixed old/new geometry. The previous tests
observed bottom-right motion and did not cover that origin change.

## Decision

- Let DefWindowProc calculate the native left, right and bottom frame. Replace
  only the caption allocation. On maximize, retain the DPI-scaled top resize
  inset instead of replacing an intermediate proposed rectangle with the whole
  monitor work area. Windows continues to own maximize, restore and resizing.
- Return zero from WM_NCCALCSIZE. Remove the top-level SWP_NOCOPYBITS override.
  Keep the existing child layout batching and complete owner-thread painting;
  no synchronous paint/position call crosses into the preview helper.
- Include the native side/bottom frame in the 480 x 400 DIP client minimum.
  Measure status text with a memory DC, avoiding a window-DC presentation during
  layout. The custom caption and button materials remain unchanged.
- Reuse the horizontally constant part of the Gaussian material raster with
  three slices. Preserve complete corner/blur support and bound the colored
  cache to 32 entries. Palette colors, size and animation depth are cache keys.
- When owner polling resumes after a long pause, returning credit for an old
  preview snapshot starts one bounded refresh interval. Keep it across timer
  ticks until a snapshot sent after that credit arrives, or three seconds
  elapse. Another old snapshot cannot extend the interval. Pipe/process failure
  and explicit native recovery remain immediate; this change retained protocol
  version 3 at the time. The later retained-endpoint work uses version 4;
  see current [ADR-010](ADR-010-trim-view-ownership-and-orchestration.md).
  This prevents premature helper replacement while its fresh acknowledgement
  is still in flight. The Debug starvation test reproduced that race.

## Basis and verification boundary

Microsoft documents [custom client/frame calculation](https://learn.microsoft.com/en-us/windows/win32/dwm/customframe),
[WM_NCCALCSIZE preservation](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-nccalcsize),
[window positioning flags](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)
and [composited DC lifetime](https://devblogs.microsoft.com/oldnewthing/20171018-00/?p=97245).
Chromium's [native frame implementation](https://chromium.googlesource.com/chromium/src/+/master/ui/views/win/hwnd_message_handler.cc)
also documents WVR_REDRAW mispositioning child HWNDs when nonclient insets exist.
The connection to Easnap's directional regression is based on local before/after
desktop captures, not a guarantee made by those sources.

The material test compares 768 cases against the pre-optimization production
raster hash; on 2026-09-21 the three-slice raster ran that comparison in 141 ms,
against 891 ms before. A separate opt-in `test_ui_modernization --resize-edges` diagnostic
uses real pointer drags and independent Windows Graphics Capture monitor frames
in both themes. It checks all eight edges/corners and maximize/restore, including
settled frames. On 2026-09-21 that observation recorded 4,065 drag frames and
363 clean settled samples (Windows 10 build 19045, 120 DPI, two monitors).
Final Release CTest passed 26/26 and affected Debug checks
passed 7/7. The user accepted the final desktop behavior and closed the session.
Future features must reuse this frame policy and follow the linked guide.

### Known limitation: transient artifacts while resizing

The strict resize gates (`--resize-edges` and `--resize-controls`) fail in
their latest recorded runs, and the cause is not established. Measured on
2026-09-29/30 on the secondary display (1920 × 1080, 125%):

- Four complete runs, made after the restart described below, covered all four
  edges, all four corners, caption movement and maximize/restore in both
  themes, with paused, playing and retained-endpoint preview and with visible
  status, progress and Cancel: 220 cases and 61,095 desktop frames. 59 cases
  failed visually during motion and 12 minimum-width assertions failed; all
  4,185 settled samples were clean. The longest measured dispatch was 47 ms.
  Per run (2026-09-30, both themes):

  | Run | Cases | Desktop frames | Visual-failing cases | Minimum-width assertions | Settled samples, all clean |
  |---|---:|---:|---:|---:|---:|
  | Release, fast | 60 | 14,726 | 22 | 5 | 1,193 |
  | Debug, fast | 60 | 12,854 | 16 | 1 | 1,060 |
  | Release, slow | 60 | 23,870 | 10 | 4 | 1,149 |
  | Status, progress and Cancel visible | 40 | 9,645 | 11 | 2 | 783 |

- During the slow run, private memory rose from about 65.1 to 86.6 MiB in the
  process that owns the trim window (the test executable, which also runs the
  capture observer) and from 68.4 to 90.6 MiB in the preview helper. The run
  does not show whether the growth is in the product or in the observer.
- Failing frames showed native video overlapping the timeline, clipped
  right-side controls and readout, and displaced video with duplicated caption
  pixels during maximize.
- The gate disables DWM transitions and observes four maximize/restore
  transitions. `trim_endpoint_presentation` maximizes and restores before its
  pixel observation, so its pass does not cover that transition.
- The artifact reproduced with the preview released and with the preview
  helper never created. The user saw it at the left edge, the top edge and the
  top-right corner with neither a screen recorder nor a pixel comparator
  running. A separately built window that uses only public Win32/GDI (standard
  native frame, no child windows, no custom caption, no `WS_EX_COMPOSITED`, no
  Easnap code) clipped its right anchors in captured fast top-left resizing.
- A backup checkout whose status records the 2026-09-21 acceptance, run
  unmodified with its own observer, failed 15 of 18 visual cases (4,060 frames,
  679 mixed-geometry and 310 exposed-background observations), and its
  unmodified `Easnap.exe` showed the same clipping in an external recording.
  The 2026-09-21 manual acceptance was made on the primary display; there the
  old and the current sources both failed the paused top-left subset. No run
  showed an old build passing where the current build failed.
- After the user restarted Windows on 2026-09-30 they reported clean resizing
  in the old and the current application, and the unchanged Release paused
  top-left diagnostic passed both themes (453 frames, 26 clean settled
  samples). An injected graphics-reset shortcut had not cleared that subset
  before the restart. No trace identifies what the restart changed, and a
  restart is not an application repair.

These observations do not justify replacing the frame decision above, and they
identify neither a driver nor an operating-system defect. The current state is
tracked in [project status](../project-status.md).

### Alternatives tried and rejected (2026-09-29/30)

Each of the following was applied as a controlled comparison to the top-left
resize, did not remove the artifact and was reverted:

- removing the trim window's `WS_EX_COMPOSITED` style;
- holding a DC only across layout and owned painting;
- releasing the preview entirely;
- using the complete default native caption and frame;
- a 32-bit DIB paint buffer;
- flushing GDI after the complete resize paint traversal;
- flushing the final `BitBlt` before `EndPaint`;
- reversing the owned-control paint order;
- letting Windows paint the whole subtree, with no foreign children present;
- automatic redraw in child placement;
- empty valid rectangles, and preserving the screen intersection;
- waiting for DWM presentation;
- normal DWM transition settings;
- letting normal `WM_PAINT` dispatch follow positioning;
- invalidating the old geometry before positioning;
- disabling DWM nonclient rendering (diagnostic only);
- holding a DC from `WM_WINDOWPOSCHANGING` through completed painting;
- suppressing automatic top-level redraw and repainting explicitly after layout;
- `CS_HREDRAW | CS_VREDRAW` class styles.

The paint-order and flush runs recorded foreground interference and do not
give complete directional coverage. The desktop used an NVIDIA GeForce
RTX 4060 Ti with driver 32.0.16.1088; switching the primary display from
144 Hz to 60 Hz and back did not change the result.
