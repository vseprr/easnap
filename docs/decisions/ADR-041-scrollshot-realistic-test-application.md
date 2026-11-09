# ADR-041 — A generated application for the Scrollshot tests

**Status:** Accepted (offscreen part validated on the host; desktop part ran green in the lab on 2026-11-09)
**Date:** 2026-11-09
**Relates to:** [ADR-027](ADR-027-scrollshot-stitching-engine.md) (the rules exercised),
[ADR-028](ADR-028-scrollshot-capture-workspace.md) (the workspace and session),
[ADR-037](ADR-037-isolated-test-lab.md) (where the desktop part runs)

## Context

Every Scrollshot test so far stitches synthetic documents. Their "text" is
grey vertical strokes, their pictures are gradients with noise, and the
viewports are cut out at exactly the scripted offsets
(`tests/scrollshot_synthetic.h`, `scrollshot_stitch_logic`).
`scrollshot_real_desktop` shows such a document through GDI
`SetDIBitsToDevice` and jumps 120 rows per wheel notch with no animation.
None of this has ever reached the engine:

- real glyphs: ClearType, hinting, several fonts, bold and italic runs;
- photo-like pictures;
- a background fixed to the viewport while the content scrolls over it;
- a sticky heading that starts to stick in the middle of a scroll;
- something that changes on every frame;
- content that loads while the page moves, and a list that grows at its end;
- scroll offsets that are not whole pixels;
- an overlay scrollbar that fades;
- pictures taken in the middle of an eased scroll animation;
- an application that presents through a flip-model swap chain.

There are two ways to get closer to a real application: record frames of real
applications into a corpus, or generate an application whose every pixel is
known.

## Decision

A generated application comes first; there is no recorded corpus.

- It needs no third-party content and no licence, and nothing binary enters
  the repository.
- It is reproducible from a seed, and its truth is exact: every picture's
  offset and every expected pixel are known, so the tests need no ruler and no
  blessing.
- The offscreen test runs everywhere the Standard tier runs, hosted CI
  included; the desktop test catches regressions in capture and timing that
  no replay of stored frames can.

The cost is that the application is our own model of "real", which risks
tuning the engine to it. ADR-027's thresholds are therefore not touched by
this decision; scenarios the engine fails are recorded as findings, not tuned
away.

The corpus format of the 2026-11 audit is kept. `--dump` writes the pictures
of any scenario as `NNN.png` with a `sequence.txt` manifest in that format, so
a later slice that replays recordings of real applications
(`scrollshot_corpus_replay`, skipped when the recordings are absent) can share
the reader. Whether such recordings are made and where they are kept is the
owner's decision.

## Components

- `apps/easnap-desktop/tests/scrollshot_realistic_app.h/.cpp`: the document
  model, the renderer, the scroll model and the behaviours, the masks and the
  side channel for the desktop test. Test code only.
- `tests/test_scrollshot_realistic.cpp`, test `scrollshot_realistic_stitch`:
  pictures from the model straight into the production `StitchEngine`.
- `tests/test_scrollshot_realistic_desktop.cpp`, test
  `scrollshot_realistic_desktop`: the application in a second process on the
  visible desktop, captured by the production workspace and session.

No production code changed. The desktop test observes the production provider
through a pass-through decorator that it creates at its own call site.

### Document model

A page is generated from a seed: headings, paragraphs, a code block, quotes,
bulleted and numbered lists, picture cards with captions, a comment box being
typed into, a status card with an activity indicator, picture cards that load
late, and a list of feed rows at the end. Words are made of syllables that
include ı, İ, ş, ğ, ü, ö and ç; nothing is real text.

### Scroll model

- A wheel notch scrolls 100 DIP (125 rows at 125 %), Page Down 87.5 % of the
  area under the application bar.
- Each input starts an ease-out cubic animation of 150 to 250 ms (seeded)
  from the current position towards the accumulated target, as browsers do;
  a notch that arrives during an animation retargets it.
- One frame is presented per refresh: 60 Hz offscreen, the swap chain's
  vertical blank on the desktop.
- Offsets are rounded to device pixels unless a scenario says otherwise.

### Behaviours

- **Late pictures:** a placeholder until 250 ms after the slot first came
  into view.
- **Growing list:** when the end of the loaded rows comes into view, a loading
  row with a spinner appears below it; 400 ms later five more rows arrive.
  Three batches, then an end marker. The scrollbar thumb shrinks as the page
  grows.
- **Caret:** blinks every 530 ms offscreen (the Windows default, for
  reproducibility) and at `GetCaretBlinkTime` on the desktop.
- **Spinner:** changes on every presented frame.
- **Application bar:** fixed from the start; in one variant its activity
  indicator changes every frame.
- **Sticky section heading:** scrolls with the page until it reaches the top,
  then stays there.
- **Overlay scrollbar:** shown while scrolling, fades out between 700 and
  1,000 ms after the last motion.
- **Fixed background:** solid, vertical gradient or a detailed texture, fixed
  to the viewport.

## Rendering

- Direct2D device context on a WARP Direct3D 11 device, rendering into an
  opaque target (`B8G8R8A8`, alpha ignored) so that ClearType applies.
- Custom DirectWrite rendering parameters: gamma 1.8, enhanced contrast 0.5,
  ClearType level 1, RGB, `NATURAL_SYMMETRIC`. The pixels depend neither on
  the GPU nor on the machine's ClearType tuning.
- The core Windows fonts Segoe UI, Consolas, Times New Roman, Georgia and
  Arial, from 9 to 24 pt, regular, semibold, bold, italic and underlined
  links, with the locale fixed to `tr-TR`. Each family is looked up with
  `FindFamilyName`; a missing one makes the tests exit 77 and name it.
- Pictures are computed: sky, sun, two ridges and grain. No file is read.
- Every block is rendered on its own at its own origin and copied into a
  content layer at a whole row. A viewport is composed by copying rows of that
  layer at an integer offset; placeholders, the loading row, caret, spinners,
  scrollbar and the bar are drawn over it on the CPU. What any picture shows is
  therefore known exactly.
- A fixed background: each block is rendered twice, over black and over
  white. Per channel, their difference is how much of the background shows
  through, and each frame is composed over the viewport's background with
  integer arithmetic.
- The immediate mode renders a viewport again with Direct2D, translated to the
  offset, instead of copying the layer. It checks that text rendering is
  translation-invariant at whole-pixel offsets, on which the engine's exact
  row hashes depend.
- Fractional offsets: the layer at the whole part of the offset is resampled
  linearly between rows, as a compositor does; the bar and the scrollbar are
  not resampled.

## Scenarios

Each scenario runs the scroll model with jittered capture: a picture is taken
every 16 to 50 ms (seeded), and it shows the frame presented last, so most
pictures fall in the middle of an animation. Identical pictures are dropped
before the engine, as the session drops them; that filter is inline in the
session's worker loop, so the tests restate it with the same whole-picture
comparison. "Assert exact" means every
placement at the true offset and the image equal to the page outside the
masks; "assert placement" asserts the placements only; "measured" and
"expected failure" print figures and assert nothing about the image.

| Scenario | What the page does | ADR-027 rule exercised | Expectation |
|---|---|---|---|
| `wheel-eased` | Single notches and bursts of two or three | Finding the offset, steps 3 to 6; exact row hashes; `Unchanged` | Assert exact |
| `page-down` | Mostly Page Down | The coarse search within one frame height | Assert exact |
| `sticky-header` | Application bar fixed from the start | Fixed bands; the header from the first frame | Assert exact |
| `caret-spinner` | Caret blinks, spinner changes every frame (masked) | The 55 % share; visible rows overwritten | Assert exact |
| `overlay-scrollbar` | Fading overlay scrollbar (columns not compared) | A scrollbar is content | Assert exact; columns measured |
| `lazy-at-rest` | Four late pictures; the next input waits until they load | "Unmoved" judged by rows; a late picture ends loaded | Assert exact |
| `lazy-in-motion` | Page Down every 100-140 ms; pictures leave before loading | A frame that cannot be placed changes nothing | Assert exact; each slot row placeholder or picture |
| `growing-tail` | Loading row, three batches, end marker | Rows appended; `Refreshed` when a batch arrives | Assert exact |
| `solid-background` | Fixed solid background | A flat area carries no detail | Assert exact |
| `cleartype-immediate` | Every picture rendered again at its offset | Exact row hashes need translation-invariant text | Assert exact, 0 pixels unlike the layer |
| `realistic-app` | Bar, caret, spinner, three late pictures, growing list, scrollbar, notches, bursts and Page Down | Everything above at once | Assert exact |
| `gradient-background` | Fixed vertical gradient | Informative segments; the exact path lost on the background | Assert placement; image measured |
| `sticky-section` | Heading that sticks mid-scroll | Bands from the median of five | Measured |
| `fractional-offsets` | Offsets not rounded | No exact path; tolerant descriptors only | Measured |
| `textured-background` | Fixed detailed texture behind sparse text | "Unmoved" judged by rows can win over motion | Expected failure |
| `animating-header` | Bar with an indicator that changes every frame | A header that animates every frame is not fixed (ADR-027 limit) | Expected failure |

The desktop test shows the `realistic-app` configuration.

## Comparison rules

- **Placement.** Every frame the engine writes (placed, and not `Unchanged`)
  must sit at its true offset minus the first frame's: zero rows of tolerance.
  `Unchanged` frames and refusals are counted, not failed: ADR-027 allows a
  refusal. For fractional offsets the figure is the distance from the true
  position, and one row is the expectation.
- **Image.** Tolerance 0 per channel, alpha ignored. Left out: the
  scrollbar's columns and the caret and spinner rectangles. The bar rows must
  equal the bar as the first frame showed it. In `lazy-in-motion` a late
  picture's rows may show its placeholder or the picture, nothing else. The
  height must be the deepest offset minus the first, plus the viewport.
- **Scrollbar.** Its columns are counted separately: ADR-027 treats a
  scrollbar as content, so they end up showing the thumb where viewports
  showed it.
- **Fixed gradient or texture.** There is no single right image. Each row is
  compared with the row as the last frame that wrote it presented it; the
  count is reported.

## Tests, labels, tiers and skips

| Test | Labels | Tier | Timeout | Skips (77) |
|---|---|---|---|---|
| `scrollshot_realistic_stitch` | `integration;scrollshot` | Standard, hosted CI included | 240 s | WARP, Direct2D, DirectWrite or one of the five fonts missing |
| `scrollshot_realistic_desktop` | `integration;scrollshot;real-desktop-capture`, in `interactive_desktop_tests` (`interactive-desktop`, `RUN_SERIAL`) | Full, in the lab only | 180 s | No capture exclusion; work area too small; the application cannot be rendered; the application process cannot create a flip-model swap chain |

No script parameter changed. In `testenv.json` the desktop test joins
`scrollshot_real_desktop` in `tests.captureTests`, `probe.tests` and the
`display` and `gpu` parity lists.

**The desktop test.** It follows `scrollshot_real_desktop`: the display from
`EASNAP_TEST_MONITOR`, the same 77 rules, one click into the frame where the
wheel follows the focus, and real `SendInput` only after `WindowFromPoint`
has returned the application's window.

- *The application.* A second process (`--app`) shows the application in a
  window that has static chrome around the content pane; the pane is the
  frame. Every frame is composed on the CPU, copied into a flip-model swap
  chain with frame latency 1 and presented with vsync. The process publishes
  the content layer, the bar, the masks, the late-picture slots and its
  scroll state in the named mapping `Local\EasnapScrollshotRealisticApp-<pid>`.
  The test waits on that state with deadlines, never with fixed sleeps.
- *The workspace.* The production overlay, `ExposeFrame`, the island, the
  preview and `ScrollshotSession`.
- *The observer.* The session's provider is the production provider wrapped
  in a pass-through observer. Before each capture it reads the loaded height,
  since rows below it existed when the picture was taken. It drops identical
  pictures with a restated copy of the session's filter. It finds each
  picture's true offset by an exact search in the published layer: the bar
  must match exactly, and every loaded row must match outside the scrollbar,
  the masks and the late pictures. Each picture also goes to a shadow
  `StitchEngine`, which places it as the session's engine does.
- *The observer's cost.* `Observe` runs inline on the session's worker, so it
  delays the session's next reading. Per examined picture it copies about
  1.4 MB (about 0.2 ms), searches for the offset (estimated 0.5 to 5 ms,
  nearest the last offset first) and runs a second `StitchEngine::Submit`.
  That is an estimated 1 to 10 ms per examined picture in Release, inside the
  session's reading period of at least 30 ms; Debug may stretch it. The
  estimate is not measured: the test prints the mean and the worst time it
  spent (`observe_ms_mean`, `observe_ms_worst`), and the first lab run
  replaces the estimate with those figures.
- *The input.* Six single notches, three pairs whose second notch arrives
  mid-animation, a click and two Page Downs, then notches to the end of the
  list; three batches load on the way.
- *What it asserts.* No written picture that is not the application; every
  written picture at its true offset; the image equal to the page outside the
  masks and scrollbar, with the bar exact; the shadow engine's canvas equal to
  the session's result pixel for pixel, and its count of placed pictures
  equal to the session's, so the observer saw what the session saw; the
  session still following; and, where Desktop Duplication works, no more than
  three pictures through GDI.
- *What it prints.* `EASNAP_CAPTURE_PATH=<Duplication|GDI>` and a `MEASURE`
  line.

## Media and licence

Nothing is stored. The tests write no picture unless `--dump <directory>` is
given; the directory to use is `build/Testing/scrollshot-realistic/`, which
is ignored with the rest of `build/`, and PNG files are refused everywhere
else. Nothing is added to `scripts/repository-media-allowlist.txt`. The fonts
are the system's own, used at run time; no font, text or picture of anyone
else is part of the tests.

## Relation to item 1.2 and to `scrollshot_real_desktop`

`scrollshot_real_desktop` stays as it is. It covers GDI presentation, instant
jumps, the screenshot taken with the shading split, and capture exclusion
around the frame. The new desktop test adds flip-model presentation, eased
animation, changes on every frame, late content, a growing list and a check
of every placement.

Item 1.2 changes the capture path that both tests use with the Scrollshot
options: `CaptureMonitorRegion`, `ImageLooksBlank` and `AcquireFrame`, with a
wait of 100 ms, `await_first_composed_frame` and `require_every_output`. Run
both tests in the same lab Full run after any change there. A first viewport
that is pure black was not modelled: the application starts with its bar on a
white page. That case stays with 1.2.

## Limits

- Text is rendered on WARP only: always offscreen, and also by the desktop
  process in a lab without a GPU.
- One monitor. The default lab runs at 125 %; the 150 % image
  (`-Variant Scale150`) runs the same test when it is used.
- It is not a real browser. Chromium and Edge rasterise tiles with their own
  rasteriser, present through DirectComposition, may present partial updates
  and scroll by fractional offsets at some zoom levels. Independent flip and
  multiplane overlays are not available in the lab.
- The notch distance, easing, delays and the look of the page are our
  choices.
- The scrollbar columns and the masked rectangles are never compared exactly.
- A late picture appears at once; a fade-in was not modelled.

## Validation

**Host, offscreen** (development PC, Windows 10 22H2 build 19045, Release,
2026-11-09). 257 checks passed; the whole run took 8 s (Debug: 59 s, with the
same figures). "Pictures" counts every picture taken. "Submitted" counts what
the engine received after identical pictures were dropped. "Mid-animation"
counts submitted pictures taken while an animation ran. "Wrong" counts
placements away from the true offset (more than one row for fractional
offsets). "Rows unlike" counts image rows that differ from the page, or from
the last presentation for a fixed gradient or texture.

| Scenario | Pictures | Submitted | Mid-animation | Refused | Wrong | Height (expected) | Rows unlike |
|---|---|---|---|---|---|---|---|
| wheel-eased | 197 | 118 | 113 | 0 | 0 | 3,835 (3,835) | 0 |
| page-down | 119 | 49 | 45 | 0 | 0 | 3,817 (3,817) | 0 |
| sticky-header | 214 | 127 | 125 | 0 | 0 | 3,805 (3,805) | 0 |
| caret-spinner | 247 | 149 | 134 | 0 | 0 | 3,886 (3,886) | 0 |
| overlay-scrollbar | 236 | 135 | 122 | 0 | 0 | 3,801 (3,801) | 0 (208 rows in the scrollbar columns) |
| lazy-at-rest | 236 | 118 | 110 | 0 | 0 | 3,857 (3,857) | 0 (4 `Refreshed`, one per late picture) |
| lazy-in-motion | 103 | 33 | 32 | 0 | 0 | 3,835 (3,835) | 0 (887 slot rows kept the placeholder) |
| growing-tail | 336 | 178 | 172 | 0 | 0 | 5,340 (5,340) | 0 (20 pictures taken while a batch loaded) |
| solid-background | 233 | 129 | 128 | 0 | 0 | 3,842 (3,842) | 0 |
| cleartype-immediate | 146 | 70 | 66 | 0 | 0 | 2,649 (2,649) | 0; 0 pixels unlike the layer in 146 renderings |
| realistic-app | 347 | 197 | 169 | 0 | 0 | 5,391 (5,391) | 0 (280 rows in the scrollbar columns) |
| gradient-background | 208 | 123 | 120 | 0 | 0 | 3,849 (3,849) | 1,325 |
| sticky-section | 247 | 139 | 135 | 0 | 0 | 3,852 (3,852) | 31, from row 1,225 |
| fractional-offsets | 221 | 154 | 138 | 59 | 68 of 90 | 3,873 (3,876) | 2,438 |
| textured-background | 184 | 116 | 115 | 0 | 58 of 116 | 3,730 (3,806) | 3,704 |
| animating-header | 217 | 217 | 138 | 0 | 0 | 4,063 (4,063) | 2,656 |

What the figures say:

- **Real text, pictures, eased scrolling, late content, the growing list, a
  fixed bar and a caret and spinner** are stitched exactly, although nearly
  every picture fell in the middle of an animation.
- **ClearType at whole-pixel offsets is translation-invariant** with these
  rendering parameters on WARP: 146 renderings, 70 of them distinct pictures,
  matched the layer pixel for pixel.
- **Fractional offsets (finding).** The research expected every placement
  within one row, or a refusal. 59 of 154 pictures were refused
  (`NoOverlap`), and 68 of the 90 written placements were more than one row
  away from the true position; the worst was 3.07 rows. The image came out
  three rows short. The first error was 1.08 rows (at 363 for 364.08); later
  placements agreed with the shifted canvas, so the error carried on and
  grew. The scenario stays measured. Placing such pictures correctly, or
  refusing them, needs a change to the engine and an amendment of ADR-027.
- **Textured fixed background (expected failure, confirmed).** 58 of 116
  placements were wrong, the worst by 76 rows; some were `Refreshed`, judged
  unmoved while the page moved. The engine took 41 and 165 columns of texture
  for side rails, and the image is 76 rows short. Nothing in the session's
  progress shows this: it is the silent failure the research predicted.
- **Animating bar (expected failure, confirmed).** Every placement was right,
  but the bar was never recognised as fixed and was written into the content:
  2,656 of 4,063 rows differ.
- **Sticky heading engaging mid-scroll (measured).** Every placement was
  right; the heading was stamped once into the content, as predicted (31
  rows from row 1,225). Kept measured. Removing the stamp is a change to the
  engine's band rule and belongs in its own slice.
- **Fixed gradient.** Every placement was right. 1,325 rows show the
  background from another viewport row than the last frame that showed them.
  ADR-027 writes the rows next to the detected bands from their first
  appearance (the engine found a 34-row footer band in the gradient), so the
  background behind the text is a patchwork of bands.
- **Overlay scrollbar.** As ADR-027 describes, its columns are content: 208
  and 280 rows of those columns show thumb remnants.

**Desktop** (`scrollshot_realistic_desktop`). It needs the real pointer and
the visible desktop and was not run on the host. First lab run: Full tier,
run `20261109-051443-full` (2026-11-09, tree `dd78926`), green 56 of 56, no
retry, no fallback. The test ran in 9.0 s on Desktop Duplication.

- 124 pictures were taken; 23 identical ones were dropped, so 101 were
  submitted, 60 of them while an animation ran.
- 0 pictures were refused and 0 placements were away from the true offset.
- The image height was 4,539 rows, as expected, with 0 rows differing from
  the page outside the masks and the scrollbar.
- The shadow engine's canvas was equal to the session's result.
- The observer's cost, which replaces the estimate above: 4.33 ms mean and
  14.52 ms worst per examined picture; the worst case is above the
  estimated 1 to 10 ms and below the session's reading period of at least
  30 ms.

Not run: the 150 % image and a host run.
