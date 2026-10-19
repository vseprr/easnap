# Testing

How the test suite is organised, where it runs, how to run only what you need,
and how it is kept away from the person using the computer. Decisions behind
this are in [ADR-025](decisions/ADR-025-incremental-test-execution.md),
[ADR-026](decisions/ADR-026-test-tiers-and-desktop-isolation.md) and
[ADR-037](decisions/ADR-037-isolated-test-lab.md).

The Standard and Full tiers run in the [lab](#lab), a virtual machine with a
desktop of its own. A run on the host desktop is the exception, made only
when the owner asks for it.

## Quick reference

```powershell
# In the lab (elevated Windows PowerShell 5.1)
.\scripts\lab.ps1 run -Tier Standard         # Standard tier in the VM, writes a verdict
.\scripts\lab.ps1 run -Tier Full             # adds the visible-desktop tests, on the VM's desktop
.\scripts\lab.ps1 run -Tier Soak             # everything, including long repeats; before a release
.\scripts\lab.ps1 status                     # VM state and the latest verdict
.\scripts\build.ps1 -Trust Lab               # build without tests if the verdict covers this tree

# On the host
.\scripts\build.ps1 -TestMode None           # build only
.\scripts\test.ps1 -Tier Fast                # unit tests, a few seconds
.\scripts\test.ps1 -HostOnly Only            # the tests listed under tests.hostOnly
.\scripts\test.ps1 -ListOnly                 # what would run, without running it
.\scripts\test.ps1 -Filter 'preview|trim'    # only tests whose name matches
.\scripts\test.ps1 -Mode Full                # ignore the pass cache
.\scripts\test.ps1 -Tier Full -Monitor SAMSUNG   # host run, only when the owner asks
```

`test.ps1` tests the binaries that are already built. A plain `build.ps1`
builds and then runs the Standard tier on the host.

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
those tests; they run in the lab.

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
ignore it; a lab run always does.

## Keeping tests away from your work

Windows has one pointer, one keyboard focus and one input queue per logon
session. A test that calls `SendInput` moves the real pointer, and there is no
supported way to give a process a second, private system pointer. The suite
therefore uses these mechanisms, in order of preference.

**1. No desktop at all.** Most tests never create a visible window.

**2. A private desktop.** Tests that drive production windows with window
messages run through `easnap_isolated_desktop`, which creates a separate
Windows desktop for the test process. That desktop is not the input desktop:
its windows are never shown, your mouse and keyboard cannot reach them, and
they cannot take focus or the pointer from you. These tests run in the
background, in parallel with the rest. Set `EASNAP_TEST_ISOLATION=off` to run
one on the visible desktop while debugging it.

**3. Another logon session: the lab.** A few tests must use the real desktop:
they verify real pointer input, the foreground window, or pixels as actually
composed and presented. Desktop Duplication and Windows.Graphics.Capture only
see the visible desktop, so these cannot be moved to a private one. In the lab
they run on the VM's own desktop, so nothing on yours is touched.

**4. A host run, on one chosen display.** When the owner asks for the desktop
tier on the host, `test.ps1`:

- asks which display to use (`-Monitor` with a number, a name such as `SAMSUNG`,
  a `\\.\DISPLAYn` device or `Primary`; set `EASNAP_TEST_MONITOR` to skip the
  question; `-ListMonitors` lists them) and fails rather than silently using
  another display;
- runs them last, one at a time, after everything else has finished;
- prints what is about to happen and an estimate from previous runs, counts
  down, and waits until the mouse and keyboard have been idle for a moment;
- restores the pointer position afterwards;
- runs a failed test once more. A pass on the second attempt is reported as
  **unstable** and is not cached.

Moving the mouse, typing, switching windows or covering the chosen display can
still fail them, because the input queue and focus are shared. Not every
desktop test honours the chosen display yet; `recording_pause_stop_stress`
captures a small region at the origin of the virtual screen.

On the host, audio and the clipboard are not isolated either.
`wasapi_audio_capture` plays a short test tone through the default output
device and records it back. `capture_workspace_production` (Standard tier)
copies a test image and `annotation_editing_native` (visible desktop) copies
the word "Paste"; whatever was on the clipboard before is gone.

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

The table-driven tests prove that layout stays inside the work area and that
controls do not overlap at those sizes. They do not prove rendering, DPI
transitions between monitors or behaviour on a particular GPU. Manual checks
cover what no test here observes: real pointer speed, mixed DPI and monitor
origins, tool gestures versus double-click completion, clipboard contention,
and audio and preview behaviour on the devices that matter. Refresh rate does
not appear in the matrix because nothing in the application assumes 60 Hz:
recording follows the selected frame rate on a steady clock and UI animation
is time-based.

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
- A test that needs a real GPU goes into `tests.gpuRequired` in
  `testenv.json`; one that must run on the host goes into `tests.hostOnly`.
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

The lab is the Hyper-V virtual machine `easnap-lab` on the development PC:
Windows 10 Pro 22H2 (Turkish), one 1920x1080 display at 125 %, 4 virtual
processors, 8 GB, a virtual audio cable (VB-CABLE) and no GPU. It is driven by
winlab (`C:\Projects\winlab`, a separate project), configured by
`testenv.json` and called through `scripts\lab.ps1` from an elevated Windows
PowerShell 5.1. Design and measured results are in
[ADR-037](decisions/ADR-037-isolated-test-lab.md).

| Command | What it does |
|---|---|
| `run -Tier Standard` (or `Full`, `Soak`) | Run the tests in the VM and write a verdict |
| `status` | VM state and the latest verdict |
| `verify` | Whether the latest verdict covers the current tree: exit 0 trusted, 2 tree differs, 3 not green, 4 no verdict |
| `probe` | Run the real-desktop, visual and audio tests with the WGC and the Duplication observer, with their capture paths |
| `parity` | Compare the guest with the host and list the tests each difference touches |
| `connect`, `screenshot`, `stop` | Open the VM console, save a picture of its screen, stop the VM |
| `create`, `provision`, `snapshot` | One time: create the VM, configure the guest, take the golden checkpoint |

Every `run` restores the golden checkpoint, syncs the sources, builds with
`scripts\build.ps1 -TestMode Build` and runs
`scripts\test.ps1 -Tier <tier> -Mode Full -Monitor Primary -NoCountdown -HostOnly Exclude`
in the VM's own desktop session. It writes `build\Testing\lab\verdict.json`,
the Turkish summary `build\Testing\lab-latest.md` and a toast. The Desktop
menu (`Easnap-menu.ps1`) has presets for these commands under "LABORATUVAR".

`scripts\build.ps1 -Trust Lab` (direct build) builds without tests when the
latest verdict is green for exactly the working tree: a SHA-256 manifest over
the files the ignore list in `testenv.json` leaves in, so documents and `*.md`
do not count. Otherwise it lists what changed and runs the tests.

**Capture paths.** The production capture provider falls back from Desktop
Duplication to GDI without saying so. The tests print
`EASNAP_CAPTURE_PATH=<Duplication|GDI|WGC>`; `test.ps1` records it in the JUnit
file and says "passed (GDI fallback - Duplication path not exercised)" when the
fallback ran, and the verdict marks such a test `VM-ok-but-fallback`. Only the
printed path is evidence of which path ran.

**Host and GPU lists.** `tests.hostOnly` in `testenv.json` names tests that
must run on the host; the lab leaves them out and `test.ps1 -HostOnly Only`
runs exactly them. `tests.gpuRequired` names tests that need a real GPU; winlab
warns early, since the VM has none. Both lists are empty: Desktop Duplication
works on the VM's video adapter.

**What a green verdict proves.** The listed tests passed for the recorded
commit and tree, in the recorded VM environment, and each used the capture
path it printed. It does not prove the host-only tests, real GPU behaviour,
more than one display, real speakers or microphones, or what the
[parity differences](decisions/ADR-037-isolated-test-lab.md#validation) touch.
Like the pass cache, it is a local aid, not a release gate.
