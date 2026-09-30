# Preview and Low Size GIF repair - 2026-09-23

Historical validation record. The 2026-09-29 report of duplicated trim controls
revealed a remaining full-window handoff regression that these standalone pixel
checks did not cover. The current backing/visibility contract and validation are
in [trim endpoint artifact validation](trim-endpoint-artifact-validation.md) and
ADR-010. Results and pending checks below describe the 2026-09-23 snapshot;
use project status for the current outstanding work.

## Scope and evidence

The user approved fixing the reported endpoint green band, black transition and
held-frame movement regression, and requested Low Size output around 42,000 bytes
or less for the supplied static desktop workload. The original 6.473 s source
MP4 was deleted. The supplied GIFs and screen recording remain comparison
material; no claim is made to have reproduced that deleted source exactly.

## Preview changes

- The software decoder returned a 1920x1088 RGB buffer for a 1920x1080 image.
  The extra eight rows were green padding, not visible image data. Preview
  stills now use the same validated Media Foundation display aperture cropping
  as GIF export. The shared helper rejects inconsistent dimensions/offsets.
- In this snapshot, native video and held stills had separate helper-owned child windows. The
  still uses the existing shared PaintBuffer. Visibility switches suppress
  intermediate redraw: hide native video, show the still, then explicitly paint
  the complete replacement once. This sequence was superseded on 2026-09-29 by
  an independently submitted opaque layered still surface. Hiding native video after painting the still
  was insufficient: a later repeated check still observed an erase/black frame.
  Play reveals native video behind the prepared In frame before transport starts.
- The first selected frame remains visible for its actual decoded sample
  interval. The last frame is scheduled from its actual PTS with a waitable
  timer; callbacks post to the owner thread. Native audio/transport completion
  remains authoritative, and logical cursor/half-open Out behavior is preserved.
- Parent painting no longer unconditionally resizes/invalidates its child.
  MFPlay UpdateVideo is called only inside the native video child's own paint
  transaction. Parent/helper resizing remains asynchronous.
- The real-desktop startup regression now also checks 1080p and 364x490 edges,
  a one-frame source, a 60 fps final sample, natural EOS, replay, aligned and
  unaligned In/Out, and retained images after aspect-ratio resize.
- The opt-in native drag test now includes caption movement and both native
  paused video and retained endpoint images in both themes. It requires an idle
  desktop and has not yet been run for this repair.

## Low Size changes and measurements

Low Size remains full resolution, 12 fps and one decoding pass. It uses a fixed
226-color cube/neutral-gray palette and restrained ordered dithering. A bounded
per-pixel anchor suppresses RGB changes of at most 8/255 per channel, reducing
H.264 noise that otherwise creates redundant GIF delta rectangles. Comparison
is against the last accepted pixel, so slow changes accumulate rather than being
lost indefinitely. Tiny changes within the deadband can intentionally be omitted;
High Quality has no such temporal filtering and retains its original output.

The old two-pass and previous one-pass RGB332 implementations produced identical
55,913-byte files on an identical diagnostic MP4. Removing an unused histogram
was not the source of the size increase. The original supplied Easnap GIF was
80,916 bytes; Lark was 45,761 bytes, with slightly different dimensions.

| Workload | Old RGB332 | New Low Size |
|---|---:|---:|
| Same decoded desktop sequence, 364x490, 3.33 s | 76,773 B | 40,835 B |
| Same native-writer MP4, 364x490, 3.00 s | 65,407 B | 39,404 B |
| Native writer -> production MP4/GIF exporter, static desktop, 364x490, 3.33 s | n/a | **40,044 B** |

The production-pipeline sample meets the 42,000-byte target. These are comparable
local workloads, not an exact rerun of the deleted recording. File-size savings
vary with content; this does not establish a universal twofold reduction.
On the same native-writer source, first-frame mean absolute RGB error decreased
from 19.10 to 6.85 and PSNR increased from 20.35 to 29.20 dB.

The deterministic synthetic desktop test decreased from 11,172 to 9,735 bytes;
High Quality remains exactly 120,066 bytes. Added tests independently decode the
GIF through WIC and check small moving pixels, clearing old pixel locations,
slow fades, neutral colors, timing, transparency, one-pass behavior and bounded
memory. The production MP4 adapter test includes nonaligned 64x50 geometry and
compares decoded edges/timing against the unmodified High Quality path.

## Validation and remaining work at the 2026-09-23 snapshot

- Release and Debug builds: passed with the existing compiler warning policy.
- Final Release background suite: **24/24 passed**, 155.67 s, four jobs.
- Final Debug background suite: **24/24 passed**, 157.90 s, four jobs.
- Final visibility sequence: **10 consecutive startup passes**, 229.36 s,
  following four passes before the additional presentation trace was added.
- After these runs, the test observer was strengthened to inspect the exact
  first/last still pixels, validate captured dimensions/ownership, and read a
  fresh production screenshot on paused `NoNewFrame`. The final observer passed
  Release (22.19 s) and Debug (22.93 s). This handles an unchanged desktop without
  reusing cached pixels; an acquired black image still fails.
- Matching Release binaries are installed in `dist/bin`; both hashes match
  `build/bin/Release`. The source and binary manifest is
  `.local/preview-repair/final-build-inputs.json`.
- Earlier candidates had passed background suites before an extra repetition
  exposed another black frame. Those failed logs are retained; the final sequence
  suppresses the intermediate redraw and explicitly paints the replacement.
- Seven registered physical desktop tests are explicitly **pending**, not passed:
  `real_desktop_recording_session`, `recording_pause_stop_stress`,
  `capture_overlay_interaction`, `capture_workspace_production`,
  `capture_frame_interaction`, `ui_modernization_native`,
  `annotation_editing_native`. Full Release and physical drag acceptance remain
  incomplete until these run on an idle desktop. Affected Debug native UI/editor
  checks and all-edge/caption checks in both configurations also remain pending.
- Actual display-mode switching during pointer/focus checks is not claimed safe.

The standalone preview window stays nonactivating/topmost and continues to
measure real desktop pixels. A trial composited test parent also failed with the
old helper; that test-only experiment and the ineffective Direct2D renderer
experiment were reverted. No failed pixel assertion was weakened or removed.
The previous failed logs and final evidence are retained under
`.local/preview-repair/`; the earlier snapshot is `source-before.zip`.

Commands retained for reproducing this snapshot's physical checks after building
the intended source version on an idle desktop:

```powershell
ctest --test-dir build -C Release -L interactive-desktop --output-on-failure
ctest --test-dir build -C Debug -R "^(ui_modernization_native|annotation_editing_native)$" --output-on-failure
.\build\bin\Release\test_ui_modernization.exe --resize-edges .local\preview-repair\resize-release
.\build\bin\Debug\test_ui_modernization.exe --resize-edges .local\preview-repair\resize-debug
```

The relevant decisions are [ADR-010](decisions/ADR-010-trim-view-ownership-and-orchestration.md)
and [ADR-013](decisions/ADR-013-gif-stream-timing-palette-and-publication.md).
