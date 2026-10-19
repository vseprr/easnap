# Testing

How the test suite is organised, how to run only what you need, and how it is
kept away from the person using the computer. Decisions behind this are in
[ADR-025](decisions/ADR-025-incremental-test-execution.md) and
[ADR-026](decisions/ADR-026-test-tiers-and-desktop-isolation.md).

## Quick reference

```powershell
.\scripts\build.ps1                          # build, then the Standard tier, changed tests only
.\scripts\test.ps1 -Tier Fast                # unit tests, a few seconds
.\scripts\test.ps1 -Tier Full -Monitor SAMSUNG   # adds the visible-desktop tests on that display
.\scripts\test.ps1 -Tier Soak                # everything, including long repeats
.\scripts\test.ps1 -ListMonitors             # displays you can choose from
.\scripts\test.ps1 -ListOnly                 # what would run, without running it
.\scripts\test.ps1 -Filter 'preview|trim'    # only tests whose name matches
.\scripts\test.ps1 -Mode Full                # ignore the pass cache
```

`test.ps1` tests the binaries that are already built. Use `build.ps1` after
editing source; it builds first and then calls `test.ps1`.

## Tiers

Every registered test belongs to exactly one tier, derived from its CTest labels.

| Tier | Label | What it contains | When it runs |
|---|---|---|---|
| fast | `unit` | Deterministic logic: state machines, layout, queues, mixing, cleanup containment, the display matrix | Every run |
| integration | (none of the others) | Real Media Foundation encoding and trimming, WASAPI, the preview helper, production windows on a private desktop | `Standard` and above |
| desktop | `interactive-desktop` or `desktop-visual` | Tests that need the real pointer, the foreground window or pixels actually presented on a monitor | `Full` and above |
| soak | `soak` | Long repeats of paths the other tiers already cover: extra random seeds, 30-second seek runs, the 72-click timeline run | `Soak` only |

`-Tier Standard` is the default. It reports desktop tests as **pending**: they
were not run and are not counted as passed. Tests outside the requested tier
are reported as such and are never taken from the cache.

Hosted CI runs the fast tier and the part of the integration tier a hosted
Windows Server image can execute. It has no real display or input queue and
cannot open an MFPlay player, so it excludes the desktop and soak tiers, every
test labelled `preview`, and `capture_overlay_interaction`, whose click targets
assume a larger desktop than the runner's. A green CI run says nothing about
those tests; they must be run locally.

## Running only what changed

After a build, each test gets a fingerprint of its executable, the helper
executables it starts, its command line and CTest properties, the build
configuration, the `EASNAP_*` environment variables and a digest of shared
inputs (headers, resources, build files, scripts and production sources that
no test target compiles). A test whose fingerprint matches a pass from the
last 24 hours is not run again.

In practice:

- Editing one `.cpp` file reruns the tests whose executables contain it.
- Editing a header, a resource, a CMake file or a script reruns everything.
  This is deliberately conservative.
- Editing documentation reruns nothing.
- A failed, interrupted, skipped or retried test is never cached.

The cache is a local iteration aid, not a release gate. Use `-Mode Full` to
ignore it.

## Keeping tests away from your work

Windows has one pointer, one keyboard focus and one input queue per logon
session. A test that calls `SendInput` moves the real pointer, and there is no
supported way to give a process a second, private system pointer. The suite
therefore uses three different mechanisms, in order of preference.

**1. No desktop at all.** Most tests never create a visible window.

**2. A private desktop.** Tests that drive production windows with window
messages run through `easnap_isolated_desktop`, which creates a separate
Windows desktop for the test process. That desktop is not the input desktop:
its windows are never shown, your mouse and keyboard cannot reach them, and
they cannot take focus or the pointer from you. These tests run in the
background, in parallel with the rest. Set `EASNAP_TEST_ISOLATION=off` to run
one on the visible desktop while debugging it.

**3. One chosen display, with a warning.** A few tests must use the real
desktop: they verify real pointer input, the foreground window, or pixels as
actually composed and presented. Desktop Duplication and Windows.Graphics.Capture
only see the visible desktop, so these cannot be moved to a private one. For
them `test.ps1`:

- asks which display to use (`-Monitor` with a number, a name such as `SAMSUNG`,
  a `\\.\DISPLAYn` device or `Primary`; set `EASNAP_TEST_MONITOR` to skip the
  question) and fails rather than silently using another display;
- runs them last, one at a time, after everything else has finished;
- prints what is about to happen and an estimate from previous runs, counts
  down, and waits until the mouse and keyboard have been idle for a moment;
- restores the pointer position afterwards;
- runs a failed test once more. A pass on the second attempt is reported as
  **unstable** and is not cached.

While they run you can read on another display. Moving the mouse, typing,
switching windows or covering the chosen display can still fail them, because
the input queue and focus are shared. Not every desktop test honours the
chosen display yet; `recording_pause_stop_stress` captures a small region at
the origin of the virtual screen.

Audio is not isolated by any of this. `wasapi_audio_capture` plays a short
test tone through the default output device and records it back.

Neither is the clipboard: there is one per logon session.
`capture_workspace_production` (Standard tier) copies a test image and
`annotation_editing_native` (visible desktop) copies the word "Paste". Whatever
was on the clipboard before is gone after either of them.

### A separate machine

The application is Win32, uses Desktop Duplication, Media Foundation and
WASAPI, and the build rejects non-Windows targets. A Linux server (including a
Raspberry Pi) cannot run these tests. It could run the repository hygiene
checks or act as a controller, nothing more. Complete isolation of the desktop
tier needs a second Windows session: a virtual machine, a second PC, or a
self-hosted Windows runner with a logged-in desktop.

## Display coverage

The physical baseline is two 1920x1080 displays at 125 % (144 Hz and 60 Hz).
Other environments are covered by table-driven unit tests over pure layout and
geometry functions, which need no monitor:

- trim window sizing and control layout across work areas from 1280x680 to
  3840x2120, ultrawide and portrait, at scale factors from 100 % to 350 %;
- selection-handle hit areas across scale factors and negative-origin layouts;
- placement of the Scrollshot island and preview on one, two and stacked
  monitors, a frame across two monitors and a small work area;
- placement of the tools island, the recording panel and the mode island
  around frames of every position and size on five display and island sizes.

Manual checks cover what no test here observes: real pointer speed, mixed DPI
and monitor origins, tool gestures versus double-click completion, clipboard
contention, and audio and preview behaviour on the devices that matter.
Passing deterministic tests is not a substitute for those observations.

The table-driven tests prove that layout stays inside the work area and that
controls do not overlap at those sizes. They do not prove rendering, DPI transitions between
monitors or behaviour on a particular GPU; that still needs a physical check.
Refresh rate does not appear in the matrix because nothing in the application
assumes 60 Hz: recording follows the selected frame rate on a steady clock and
UI animation is time-based.

## Writing tests

- Exercise production code. Do not reimplement the logic under test.
- Prefer a pure function with a table of inputs over a window.
- If a window is needed, drive it with messages so it can run on a private
  desktop: wrap the command in `easnap_isolated_desktop` and add the
  `isolated-desktop` label.
- Use the visible desktop only for what cannot be observed otherwise, add the
  test to `interactive_desktop_tests`, and place windows with `OnTestMonitor`
  so they follow the chosen display.
- A simulated gesture has to end the way the real one does. A double-click
  that is sent without the button release leaves a native control holding the
  mouse, and every movement of the real pointer then changes its state.
- Send real input only after verifying which window will receive it
  (`WindowFromPoint`, `GetForegroundWindow`), so it can never land in a window
  of the person using the PC. `test_scrollshot_desktop.cpp` shows the pattern,
  including a second process as the application being captured.
- A test that cannot run in the environment exits with 77 and declares
  `SKIP_RETURN_CODE 77`. CTest then reports it as skipped, not as passed.
- Do not synchronise with a fixed sleep. Wait for the condition with a
  deadline; a fixed sleep is a timing assumption that fails under load.
- Label long repeats `soak` and keep a short variant in the normal tiers.
- Code that writes to the registry takes the key as a parameter and is tested
  against a key the test creates and removes (`test_startup_entry.cpp`).
- Text that a window shows is checked in every language by
  `text_fits_in_every_language`; a new window or island belongs in it.
- `executable_imports` reads the built `Easnap.exe`. A new import that Windows
  N editions lack has to be delay-loaded, or that test fails.

## Diagnostics that are not registered tests

`test_ui_modernization` has opt-in modes that are not registered tests:
`--resize-edges` and `--resize-controls` observe the trim window while it is
resized and maximized, `--trim-checks`, `--text-checks` and
`--window-lifetime` exercise one area each. The strict resize gate
(`--resize-edges`) currently fails, see
[project status](project-status.md); a green run of the registered suite does
not close it.

The resize gates print three counts per case. `mixed_geometry` counts captured
frames in which the observed control patches do not all match their reference
pixels at one recent window geometry; a one-pixel offset is allowed. `holes`
counts frames in which at least 16 sampled pixels inside the client area show
the test's green backdrop. A settled sample is invalid when either condition
appears in a frame captured after the gesture has ended.
`EASNAP_TEST_RESIZE_SLOW=1` makes each resize case use one 3,200 ms
shrink/grow cycle instead of four 400 ms cycles.

## Lab

The lab is a Hyper-V virtual machine on the development PC: a second Windows
session that the suite can use without sharing the pointer, the focus or the
input queue with you. It is driven by `winlab`, a project-independent tool in
`C:\Projects\winlab` (`winlab.ps1`). This repository configures it with
`testenv.json` and calls it through `scripts/lab.ps1`. The decision is in
[ADR-037](decisions/ADR-037-isolated-test-lab.md).

The guest runs Windows 10 Pro 22H2 (Turkish) with one 1920x1080 display at
125 %, 4 virtual processors at a low CPU weight and 8 GB of fixed memory. The
lab user `lab` logs on automatically. The VM has no GPU and no audio device of
its own; a virtual audio cable (VB-CABLE) is installed so that the WASAPI tests
have an endpoint.

Windows has one pointer, one focus and one input queue per logon session, so the
visible-desktop tests cannot be isolated on the machine someone is using. In the
VM they run in the VM's own console session, through a one-shot scheduled task
with an interactive logon type. PowerShell Direct sessions live in session 0 and
cannot see the desktop, which is why the task is needed.

| Command | What it does |
|---|---|
| `.\scripts\lab.ps1 create`, `provision`, `snapshot` | One time: create the VM, configure the guest, take the golden checkpoint |
| `.\scripts\lab.ps1 status` | Show the VM state and the latest verdict |
| `.\scripts\lab.ps1 run -Tier Standard` (or `Full`, `Soak`) | Run the tests in the VM and write a verdict |
| `.\scripts\lab.ps1 probe` | Run the capture-path probe |
| `.\scripts\lab.ps1 parity` | Compare the guest with the host |
| `.\scripts\lab.ps1 verify` | Tell whether the latest verdict covers the current tree |
| `.\scripts\lab.ps1 connect`, `stop` | Open the VM console, stop the VM |

The Desktop menu (`Easnap-menu.ps1`) has a "LABORATUVAR" section with presets
for these commands.

Every `run` restores the golden checkpoint, copies the sources (the ignore list
is in `testenv.json`; `build/` is excluded), builds with
`scripts/build.ps1 -TestMode Build` over PowerShell Direct (compiles the tests, runs none), and runs
`scripts/test.ps1 -Tier <tier> -Mode Full -Monitor Primary -NoCountdown` in the
interactive session. It then copies back `build/Testing/*.xml` and the logs, and
writes `build/Testing/lab/verdict.json` with the commit, a tree hash and file
manifest, the status and capture path of each test, and a digest of the VM
environment. A Turkish summary goes to `build/Testing/lab-latest.md`, and a
Windows toast reports the result.

### Direct build

`.\scripts\build.ps1 -Trust Lab` asks winlab (`lab.ps1 verify`) whether the latest
verdict is green for exactly the current working tree: SHA-256 per file, over the
same ignore list. If so, it builds without running the tests and prints why. If
not, it lists the changed, added and removed files and runs the tests as usual.

Exit codes of `lab.ps1 verify`: 0 trusted, 2 the tree differs, 3 not green or
too old, 4 no verdict.

### Capture paths

The production capture provider tries Desktop Duplication and silently falls
back to GDI. A pass on GDI does not prove the Duplication path, which the
development PC uses. The tests now print `EASNAP_CAPTURE_PATH=<Duplication|GDI|WGC>`:
the recording tests through a provider probe at the end, the UI tests through the
presented-desktop observer in `tests/presented_desktop_capture.h`.
`scrollshot_real_desktop` and `mfplay_preview_startup` already named it.

`test.ps1` reads these lines by the rules in `testenv.json`
(`tests.capturePathRules`), writes them as an `easnap_capture_path` property into
the JUnit file and into `build/Testing/easnap-capture-paths-<Configuration>.json`,
and words the result as "passed (GDI fallback - Duplication path not exercised)"
when the fallback was used.

### Host-only tests, audio and parity

- `tests.hostOnly` in `testenv.json` lists the tests that must run on the host.
  `test.ps1 -HostOnly Exclude` (used by the lab run) leaves them out and lists
  them; `-HostOnly Only` runs exactly them on the host. The list is empty:
  the probe of 2026-10-19 passed every capture and audio test in the VM.
- `recording_pause_stop_stress` exits 77 (skipped) when no audio source could be
  covered. winlab reports a pass of an audio test in a VM without an audio
  endpoint as skipped, never as passed.
- `lab.ps1 parity` compares the OS build and UBR, displays, scale, GPU, audio
  endpoints, MSVC, CMake and SDK versions, locale and time zone, and lists the
  tests that `tests.parity` maps to each difference.

### Probe

`lab.ps1 probe` runs the Group 5 and Group 6 tests and `wasapi_audio_capture`
twice: with the default WGC observer and with `EASNAP_TEST_CAPTURE=duplication`.
It prints the test, variant, result, capture path and a verdict: `VM-ok`,
`VM-ok-but-fallback` or `host-only`. On 2026-10-19 all ten tests passed in
both variants and Desktop Duplication worked on the Hyper-V video adapter;
see the [validation table in ADR-037](decisions/ADR-037-isolated-test-lab.md#validation).

### Limitations

- No GPU in the VM, so Desktop Duplication and WGC may fall back. The probe
  measures this; the printed capture path is the evidence.
- One display only.
- The VM's audio endpoint is a virtual cable, not a speaker or a microphone.
- The lab verdict is a local aid, like the pass cache. It is not a release gate
  for the host-only tests, which still have to be run on the host.
