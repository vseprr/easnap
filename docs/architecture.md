# Architecture

This document describes the implemented system. Read
[project status](project-status.md) for the current validation boundary and
[the ADRs](README.md#decisions) for durable implementation contracts.

## Processes and ownership

Easnap has a native Win32 parent process and a supervised preview helper.
The parent owns the tray, global shortcut, capture workspace, screenshot editor,
recording workflows and export. `EasnapPreviewHost.exe` hosts native MFPlay:
a stalled media callback or shutdown can be contained without blocking the
application UI. Both executables must come from the same build. Both restrict
library loading to the Windows system directory before anything else runs, and
the parent range-checks the state the helper reports over their private pipe.

The hidden host window in `src/main.cpp` receives application messages.
`CaptureCoordinator` is the sole owner of application session state. UI surfaces
forward commands and reflect that state; they do not independently start a
second capture session.

The normal flows, in the states of `SessionState`
(`app/session_transitions.h`):

```text
Idle → RegionSelecting → ScreenshotEditing → ScreenshotExporting → Idle
                       → RecordingConfiguring → RecordingActive ↔ RecordingPaused
                             → RecordingFinalizing → RecordingTrimming ↔ Exporting → Idle
                       → ScrollshotConfiguring → ScrollshotCapturing
                             → ScrollshotReview ↔ ScreenshotEditing → Idle
```

The user-facing documents call `RegionSelecting` Standby and the adjustable
frame of the three configuring states Editing; those are interaction names of
the workspace (`ui/capture_workspace.h`), not session states.

Screenshot publication runs while `ScreenshotEditing` remains active. Successful
completion transitions through `ScreenshotExporting` to Idle; publication
failure leaves the existing editing session available. Recording finalization failures have
explicit recovery handling; a failed or partial file is never presented as
successful export.
The production transition table remains the authority for permitted edges.

## Module map

All paths below are under `apps/easnap-desktop/src`.

| Area | Responsibility |
|---|---|
| `app/` | Coordinator, transition policy, settings, recording/finalization, trim and completion orchestration |
| `ui/` | Workspace geometry and pointer policy, overlays, magnifier, native recording controls, Scrollshot island and preview, trim view, and the message and Settings dialogs |
| `editor/` | Screenshot document, commands/history, tools, reversible effects, the scrolled and zoomed view, inline rendering and full-resolution composite |
| `capture/` | Desktop acquisition, per-output fallback and capture worker |
| `scrollshot/` | Stitching engine and the capture session that feeds it |
| `strings/` | Localized text from the English and Turkish resource tables ([ADR-032](decisions/ADR-032-interface-text-in-string-tables.md)) |
| `recording/` | Active-time recording session, cadence and audio timeline |
| `media/` | Bounded queues, WASAPI, dual-source mixing/resampling, MP4/GIF writers/exporters, preview and the private session store |
| `platform/` | Shared Win32/COM/GDI utilities, capture/image helpers, the data-folder path and contained deletion, log file and crash reports, the sign-in start entry |
| `main.cpp` | The shell: single instance, notification icon and menu, the capture shortcut, Settings and About, session end |

Native Windows APIs also appear in UI, capture and media code. `ui_theme.h`
centralizes shared workspace styling; individual surfaces retain their own
layout metrics. This is a Windows implementation, not a portable UI abstraction.

## Screenshot workspace

### Provisional selection and acquisition

Entering Standby captures a monitor snapshot for selection feedback and the
magnifier. It is not the final editor image. Entering Editing exposes a
provisional frame over the live desktop; geometry, Screenshot/Scrollshot/Record
switching and mode-specific controls remain available.

The first screenshot toolbar interaction, including padding or a grip press,
or an explicit screenshot action requests acquisition. The coordinator keeps
capture-excluded windows visible on Windows 10 build 19041+ when every visible
process window has the exclusion flag; otherwise it uses the hide/show fallback.
It calls `DwmFlush`, captures the desktop and crops the physical-pixel rectangle.
Only a successful `ScreenshotEditor::SetImage` fixes the frame.
Failed acquisition leaves a retryable provisional session. Cancellation does
not acquire an image.

After fixation, the source rectangle is immutable, handles disappear, and mode
switching is disabled immediately. The top mode options fade out over 500 ms.
Moving the tools island then changes only its placement.

### Rendering and input

The overlay uses layered GDI windows rather than a dim/chrome swap chain:

| Surface | Purpose |
|---|---|
| `dim_` | Full-monitor alpha-1 input plane; selected content remains live before fixation |
| `shade_` | Constant-alpha shading clipped to the area outside the selection |
| `chrome_` | Full-monitor color-keyed, click-through frame, handles, size label and notices |
| `mode_toolbar_` | Small color-keyed/alpha mode-options window on the palette-owning monitor |
| `magnifier_` | Small opaque companion on the pointer's monitor in Standby |
| `image_` | Opaque fixed-image intersection on each affected monitor |

Each monitor's serial GDI paints share one reusable bitmap/DC. Frame movement
invalidates disjoint old/new border and label regions; provisional Editing
updates the outside-shading region without painting a frozen desktop into the
selection. There is only one size label, attached to the capture frame.
The mode fade changes only the small layered window's opacity; its timer stops
when hidden or destroyed. See ADR-001 and ADR-015.

The shade is filled while transparent before interaction; later selection masks
change without requesting a full-monitor repaint. Editor/configuration islands
are owned by the top chrome surface so raising an input/mode window cannot cover
them. Recording detaches that owner before workspace retirement. Shape/effect
interiors are excluded from annotation hit tests; Save suspends auxiliary islands
during its native modal loop. See [ADR-020](decisions/ADR-020-workspace-layering-and-outline-interaction.md).

The inline image uses the existing editor's Direct2D DC renderer. The tools
island uses small GDI surfaces and styled native controls (ADR-016). Image coordinates are physical
pixels, independent of UI DPI. Dirty annotation bounds restrict image repaint.

`WorkspacePointerSequence` retains the capture-frame and blank-content sequence
policy. The production editor first claims native text editing, transform
handles, selected objects, object hits and drawing tools. These sequences cannot
trigger overlay double-click quick-copy or its pointer-release quick-return
path. Document commits retain the existing one-shot quick-return contract. Blank
Pointer content continues through the workspace policy. Double-click directly on text starts
native caret editing even when that text was not selected.

### Document and output

One `ScreenshotEditor` owns the immutable source, persistent annotation document,
selection, undo/redo snapshots, active transaction, tool defaults and derived
effect cache. Selected properties do not overwrite defaults. Native multiline
EDIT owns caret/selection/clipboard behavior; DirectWrite measures and renders
editing and committed text, including the source-backed EDIT paint. The EDIT
HWND remains visible: its synchronous native glyph/background DC is clipped to
an empty region, while the independent source-backed paint pass owns annotation
pixels. Single-click selection/dragging and blank-click deselection do not
create a new box until neither selection nor active editor exists. Point sizes
retain the acquired image's scale when UI DPI changes.

The main, contextual and brush-size islands are separate owned HWNDs. Frame-aware placement,
blank/grip dragging, resize hiding and capture-loss restoration are presentation
operations. They do not reacquire the image. Shared controls use cached soft
material masks and antialiased vector geometry; local animation timers do not
invalidate the full capture overlay. See ADR-017 and ADR-018 for input and transform rules.

Native controls suppress their synchronous paint, controls and panels paint
complete buffered frames, and the trim window and dialogs share one custom
chrome (ADR-019). The redirected capture image
is shown transparently and populated before exposure, preserving display affinity.
Document movement is unrestricted; source clips apply to content, selection and
the native editing host. Fully off-canvas effect regions do not alter source pixels.

Blur/pixelation operate on pixels derived from the original capture, owned by
`editor/redaction_renderer`. Effect changes, moves, deletion and undo
recompute only the rectangle they can influence, closed over every effect that
reads it, so the result always equals a rebuild from the capture; unrelated
vector edits reuse it. Spotlight masks combine with union semantics before one outside
dim layer is applied. Tiled watermark and vector annotations share the preview
and export drawing code. Selection frames and handles have a separate editor-only
pass. There is no second document or export-specific approximation of the tools.

`Composite()` renders at the original selected resolution into an independent
WIC target. It never captures the island or samples displayed image pixels.
Copy publishes CF_DIB and PNG; Save uses WIC and the native PNG/JPEG picker.
Acquisition, composite or clipboard failure keeps the session available and
does not substitute an unannotated success result. See ADR-003.

## Native windows and shared presentation

New windows and window-related features must follow the
[window implementation guide](ui/window-implementation-guide.md).
`StyleWindowChrome` / `WindowChrome` own the common chrome; styled native controls
and `PaintBuffer` own complete buffered paints. Resizable content windows retain
native side/bottom nonclient geometry and customize only the caption through
the accepted [ADR-024](decisions/ADR-024-native-trim-frame.md) path. Fixed dialogs,
floating islands and layered capture surfaces keep their dedicated paths.

Both executables declare per-monitor v2 DPI awareness in their manifests
(`resources/easnap.manifest`, `resources/preview_host.manifest`). The awareness
of a process cannot be raised after start, so it is not left to a run-time call
or to a build-system property. Windows take their scale from the monitor they
are on and adopt the rectangle Windows suggests when it changes.

The host window forwards display, DPI and work-area notifications to the
coordinator. A screenshot-editing session is cancelled only when the monitor
layout or scale actually changed since its snapshot was taken; a work-area
change alone (taskbar auto-hide, a docked app bar) leaves it intact. A
recording whose region is no longer inside the desktop is ended through the
normal Stop path rather than left repeating its last frame. Nothing in the
application assumes a refresh rate: recording is paced by the selected frame
rate on a steady clock and UI animation is time-based.

Layout measures with a memory DC, calculates DPI-scaled client rectangles,
batches child positioning, then invalidates and completes owner-thread paints.
Client minima include the native frame when converted to outer tracking sizes.
The native sizing path does not recreate window regions, hold window DCs across
events or synchronously paint/position the foreign video helper. Theme materials
reuse bounded geometry/colored-raster caches with complete rounded ends and a
constant center strip; palette and animation state remain part of cache identity.

Preview resize acknowledgements use the bounded asynchronous process channel.
Size changes coalesce one posted owner-thread poll so progress does not depend
solely on a low-priority timer during sustained input. The existing timer and
recovery deadlines remain; no synchronous helper operation enters layout.
During shutdown/deferred cleanup, dependent editor/recording islands retire
before their overlay owner. Recording WM_NCDESTROY clears retired native handles
even when Windows destroys the owned HWND before its C++ view is released.

The regression path covers all eight resize directions, rapid shrink/grow,
maximize/restore, both themes and 125% scaling using real HWNDs and desktop
frames. The guide identifies the production symbols and existing tests to
reuse for a new surface; how the tests are run is in [testing](testing.md),
and the open resize limitation is in [project status](project-status.md).

## Scrollshot

Scrollshot stitches the pictures of one fixed screen rectangle into a tall
image while the user scrolls its content.

`scrollshot/stitch_engine.*` has no window, thread, clock or capture
dependency. It places each submitted frame on a canvas or returns the reason
it could not: no guess is ever written. It recognises fixed headers, footers
and side rails, tolerates local changes such as a caret or a hover highlight,
and refuses ambiguous or low-information fits. See
[ADR-027](decisions/ADR-027-scrollshot-stitching-engine.md).

`scrollshot/scrollshot_session.*` owns a capture provider and one worker. The
worker reads the rectangle through `IRegionCaptureProvider::CaptureRegionImage`
when the desktop changes, submits it, and keeps a reduced preview. The UI
thread learns about progress through throttled, generation-tagged messages
and never waits for a picture; stopping has a deadline.

The coordinator starts a capture only when every visible Easnap window is
excluded from capture, rearranges the outside shading so that no translucent
window contains the frame, and takes the first picture before it locks the
frame.
While capturing, the workspace's input plane has a hole over the frame, so
Windows delivers pointer and wheel to the application underneath; Easnap
neither injects nor forwards input. `ScrollshotControls` owns the island and
the click-through preview; `PlaceScrollshotSurfaces` keeps both outside the
frame wherever the monitors allow. A stitched image survives a display change.
See [ADR-028](decisions/ADR-028-scrollshot-capture-workspace.md).

Edit opens the image in the screenshot editor's document mode. `ImageView`
is the single mapping between desktop and image pixels for every tool; the
base bitmap is uploaded in bands and the export composites the whole image in
bands at its captured resolution. See
[ADR-029](decisions/ADR-029-scrolled-and-zoomed-image-editing.md).

## Capture and recording

Capture tracks adapters and outputs independently. Desktop duplication is
preferred, with fallback scoped to unavailable or unusable outputs.
Recovery is deferred safely across callbacks. Blank-output detection is a
heuristic, not a guarantee that protected or unavailable content can be captured.
Cursor inclusion is an explicit setting.

Recording configuration stays inside the provisional workspace until Start.
Start hides the workspace, releases its snapshot/provider and flushes hidden
chrome before starting live recording. Success retires the workspace; failure
restores the same configuration and provisional frame for retry. Encoder-compatible bounds are reflected by
the visible recording boundary.

Capture, encoding, audio processing and finalization execute on workers; startup
and pause can synchronously await their safe initialization/drain barriers. Video and
audio queues are bounded by time/capacity rather than recording duration.
Pause gates delivery and removes paused time from the shared active timeline;
resume cannot inject stale queued media. Stop drains and finalizes on an owned
worker, then posts a generation-tagged notification. `RecordingFinalizer` retains
the result so the existing status timer can recover from failed delivery. Only
the matching completed generation can be consumed; stale notifications cannot
join new work. Workers are joined before their session resources are released.

The encoder thread starts only after its complete object state exists. Capture
startup waits for the first acquisition/queue result; Resume retains the remaining
interval on the active-time frame grid. This prevents empty immediate-stop output
and cumulative timestamp rounding across short active segments. See
[ADR-021](decisions/ADR-021-recording-startup-timeline-and-session-leases.md).

WASAPI preserves each device's native format and produces QPC-correlated
session-relative timestamps. MP4 can use no audio, system audio, microphone,
or the dual-source provider. Both uses clock alignment, resampling and mixing
before the single mux input. GIF is silent and retains the user's MP4 audio
preference for a later format switch. Both output formats record to a finalized
H.264 MP4 working file; the GIF path converts the selected interval at completion.
Detailed policies are in ADR-004–008, ADR-012 and ADR-014.

## Trim, preview and completion

Finalized media opens the shared trim workflow. The native view owns controls;
controllers own preview, export, generation tracking and completion decisions.
UI state cannot claim completion merely because an export worker was started.

The parent preview proxy polls the helper and sends coalesced intents. At most
one native command is outstanding. An acknowledgement is distinct from a native
completion callback. Bounded deadlines, confirmed helper retirement, one recovery
attempt and stale-generation rejection keep transport, resize and close bounded.
Resize requests are separately coalesced with one unacknowledged request.
After delayed owner polling returns credit for an old snapshot, a bounded
refresh grace persists across timer ticks until a fresh reply or three seconds;
another old snapshot cannot extend it. Explicit failure/recovery remains prompt.
Timeline pointer input remembers the last requested position within each
gesture, so stationary down/move/up messages do not replay the same seek after
playback advances. A new press or a different target remains a new request.
The native health monitor also distinguishes user intent from actual command
dispatch. A completed seek can reset the clock between periodic observations;
a changed native command sequence starts a new progress baseline. Input alone
cannot extend a missing callback's deadline or excuse a genuinely stalled clock.
Opening presents the first decoded frame paused at zero, including one-frame
media, without requiring a Play request.
Natural completion retains the logical end and the final decoded frame; selected
Out retains the greatest video PTS strictly before that exclusive boundary.
The helper keeps at most two bounded BGRA stills (source endpoint and the current
selected/restart frame), not a duration-dependent frame cache. A helper timer
uses the measured final video timestamp to cover EVR during its final frame,
before EOS clears the native surface; actual transport/audio still complete
through the native callback. Replay seeks to In and preserves its first picture
until playback resumes. The helper's still HWND is an opaque layered child:
shared PaintBuffer submits its complete GDI bitmap before the native-to-still
handoff and on resize. Its independent surface prevents stale parent/timeline
backing pixels from appearing in the video. The helper's Windows 10 manifest
enables layered child support. Both siblings keep clipping; all still drawing
and positioning remain inside the helper. Ordinary playback/paused images
remain native, and the still hides after replay's actual first-sample interval.
The private protocol distinguishes ordinary paused
seeks from exclusive-Out PauseAt intent and requires a matching version-4 pair.
Logical media duration and the final playable video position remain distinct,
including AAC tails. See
[ADR-010](decisions/ADR-010-trim-view-ownership-and-orchestration.md).

Exact full-range MP4 Save/Copy reuses the finalized source without re-encoding.
A new GIF result requires conversion even for the full interval; a previously
completed artifact may be reused only for matching bounds and an existing,
non-empty file. MP4 trim uses a half-open selection interval, overlapping video
sample spans, whole PCM-frame audio trimming and staged publication. A later In
point still decodes from the source start, so late excerpts can take longer. AAC packet
granularity limits exact container endpoints. GIF export uses bounded streaming,
centisecond timing and High quality/Low size preset policies. Neither path
buffers the entire decoded recording. See ADR-009 and ADR-013.

Native trim forwards the original decoded sample after rebasing its timing;
holding a new sample with only the same buffers would not retain the decoder's
tracked surface lease. The decoded format is read again when the decoder
announces it with the first sample: a system-memory NV12 buffer padded beyond
the picture is repacked to the picture before encoding, and the BGRA fallback
copies only the display aperture. Cancellation drains the synchronous sink on the encoder
worker before releasing it and shutting down Media Foundation. The staging
owner still discards that output; draining does not publish a cancelled export.

Save publishes a completed destination file. Copy publishes a private file
through CF_HDROP. Discard asks for confirmation. Cancellation/failure preserves
the recoverable source. Format-specific exporters share the completion and
publication layer rather than duplicating clipboard ownership.

## Lifetime, settings and privacy

Owned UI objects hide and retire first, then post cleanup to the host so the
current callback can unwind before their C++ owners are destroyed. Editor and
companion windows are released before their native owners. This avoids
re-entrant destruction of callback state.

The temporary store restricts session directories to the current user and
SYSTEM. Cleanup retains files still referenced by the clipboard and defers
deletion if clipboard inspection fails. Shutdown cancels and joins active work
before store cleanup. A copied recording can therefore outlive the parent
process until it is no longer needed by the clipboard. Deletion goes through
checked handles, refuses reparse points and verifies that each opened entry
still resolves to a direct child of the directory it was listed in.

A screenshot is saved by encoding to a temporary file beside the destination
and replacing the destination only on success, the same staged publication
recordings use.

Each new session holds an exclusive `.session-lock` handle for its live lifetime.
A different store/process cannot collect it. Released or crashed sessions are
collected at the next start, with one exception: a session marked recoverable
holds a finished recording the user has neither saved nor discarded. The next
start adopts it and offers the recording. A directory without a lease file is
left alone for an hour and removed after that. Protected directory permissions
remain unchanged.

Exit, close and the end of the Windows session ask `PendingWorkFor` what would
be lost. Exit is refused while one of the application's dialogs runs its own
message loop, and retired windows are not destroyed during such a loop. See
[ADR-031](decisions/ADR-031-unsaved-work-survives.md).

`platform/diagnostics` appends every log line to
`%LOCALAPPDATA%\Easnap\logs\easnap.log` (three files of one megabyte) and
writes a minidump without the heap to `%LOCALAPPDATA%\Easnap\crashes` when
either executable fails. Log lines carry no user content. See
[ADR-030](decisions/ADR-030-log-file-and-crash-reports.md).

Every word the user reads comes from the string tables through `strings::Get`;
the language follows Windows or the choice in Settings. Low-level files that
many tests compile take their captions from the caller instead
([ADR-032](decisions/ADR-032-interface-text-in-string-tables.md)).

The shell owns what reaches outside the process. One instance holds the
notification icon and the capture shortcut; a second start posts a registered
message to it and ends. The Settings dialog edits a copy of the values and the
shell applies them: language, appearance, shortcut (given up while the dialog
is open, so that the dialog can read any combination), the sign-in entry in
the user's `Run` key, folders and quick save. While one of the application's
dialogs runs, the shortcut only brings that dialog forward. See
[ADR-033](decisions/ADR-033-settings-shortcut-and-launch-behaviour.md).

`Easnap.exe` binds Media Foundation on first use, so that it starts on Windows
editions that lack it; the coordinator tells the workspace whether recording
is available ([ADR-034](decisions/ADR-034-media-foundation-on-first-use.md)).

Recording format, GIF preset, cursor and audio preferences are saved to the
per-user settings file after recording starts successfully. The tray cursor
toggle also persists immediately. Initial cursor/audio defaults are off/None;
saved user choices apply to later sessions. Appearance is separately persisted
as System/Light/Dark through that same INI, immediately on user selection,
without accepting pending recording choices; System follows the application
mode of Windows, also while Easnap runs. Semantic tokens and a presentation-only
UI-thread message update retained surfaces. High contrast overrides effective
colors without overwriting the preference. The timeline exposes three native
UI Automation RangeValue children backed by the existing trim model; see
[ADR-016](decisions/ADR-016-shared-appearance-and-native-controls.md).
Easnap uses no telemetry and opens no network connection; "Check for updates"
hands the releases page to the browser. See [SECURITY.md](../SECURITY.md) and
ADR-011.

## Validation boundaries

Tests exercise production policy, native windows and rendering, real clipboard
publication, capture fallback, codecs, audio and helper recovery. Native tests
depend on available Windows capabilities; a capability skip is not hardware
acceptance. The current results and remaining visual/device checks belong in
[project status](project-status.md), not in a second architectural progress log.
Tests are organised in tiers; message-driven window tests run on a private
desktop and the few that need the real desktop run on a chosen display. See
[testing](testing.md), [ADR-025](decisions/ADR-025-incremental-test-execution.md)
and [ADR-026](decisions/ADR-026-test-tiers-and-desktop-isolation.md). Full
validation bypasses the local successful-result cache.
