# ADR-024: Preserve the native trim resize frame

Date: 2026-09-21
Status: Implemented and accepted by the user on 2026-09-21; session closed.

Required implementation sequence for future windows: [window implementation guide](../ui/window-implementation-guide.md).

2026-09-29 continuation: a new Trim resize repair was authorized. Before the
30 September reboot, SAMSUNG desktop captures reproduced clipped/stale geometry
during real resizing, including with the preview released or never opened.
The all-edge gate failed; clean settled frames were insufficient. Controlled
comparisons have not justified replacing this frame decision. The historical
2026-09-21 acceptance below remains valid as historical evidence, not proof for
the current build. See [current evidence](../trim-endpoint-artifact-validation.md)
for the exact runs, rejected trials and remaining validation.

2026-09-30, before reboot: the user also confirmed visible left/top/top-right Trim defects
without any recorder or pixel comparator. A separately built standard Win32/GDI
window, without Easnap code, children, composited style or custom frame, clips
right anchors in captured fast top-left resizing. These observations constrain
causality; they neither justify an OS/driver diagnosis nor identify a verified
replacement for this decision.

After the user restarted Windows, they reported clean resizing in both the old
backup application and the unchanged current Release application. The unchanged
Release paused/top-left diagnostic then passed both themes, with 453 desktop
frames, no mixed geometry/background holes and 26 clean settled samples. The
earlier graphics-reset shortcut had not cleared that same Trim subset. A reboot
changes more desktop/session state than that shortcut; no trace identifies which
state caused the difference. Do not document a driver diagnosis or reboot as an
application repair. The native-frame policy is retained.

The subsequent final-production Release native UI, endpoint, timeline stress
and material rerun passes 4/4, but the strict fast all-edge gate still fails
22 visual cases and five minimum-size assertions across its full 60 cases:
14,726 frames, 68 mixed observations, 18 exposed-background observations and
1,193 clean settled samples. Maximum dispatch is 32 ms, with no post-gesture
queued-resize assertion failure. Reviewed PNGs confirm temporary video/control
overlap, truncated controls and duplicated caption pixels during maximize/restore.
The reboot report and clean top-left subset therefore do not establish a full
resize pass. The exact cause is unproven; the seeking repair adds no further
production frame/painting workaround.

The all-edge maximize gate sets DWM transitions disabled and samples four
maximize/restore transitions (two cycles). The endpoint test's maximize/restore occurs before
pixel observation, as preparation, so its pass does not cover that transition.
All four resize matrices are complete (220 cases) and fail the strict gate;
subsequent Debug/backend results are recorded with the same-build evidence.
Detailed results remain in the existing validation document; the authorized
repair and overall user acceptance remain open, with no Phase 4 advance.

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
raster hash. A separate opt-in `test_ui_modernization --resize-edges` diagnostic
uses real pointer drags and independent Windows Graphics Capture monitor frames
in both themes. It checks all eight edges/corners and maximize/restore, including
settled frames. Final Release CTest passed 26/26 and affected Debug checks
passed 7/7. The user accepted the final desktop behavior and closed the session.
Actual automated outcomes remain in the original logs; manual acceptance is
recorded separately. See project status for the accepted binary and evidence.
Future features must reuse this frame policy and follow the linked guide.
