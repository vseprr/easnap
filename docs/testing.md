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
  monitors, a frame across two monitors and a small work area.

These prove that layout stays inside the work area and that controls do not
overlap at those sizes. They do not prove rendering, DPI transitions between
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
- Send real input only after verifying which window will receive it
  (`WindowFromPoint`, `GetForegroundWindow`), so it can never land in a window
  of the person using the PC. `test_scrollshot_desktop.cpp` shows the pattern,
  including a second process as the application being captured.
- A test that cannot run in the environment exits with 77 and declares
  `SKIP_RETURN_CODE 77`. CTest then reports it as skipped, not as passed.
- Do not synchronise with a fixed sleep. Wait for the condition with a
  deadline; a fixed sleep is a timing assumption that fails under load.
- Label long repeats `soak` and keep a short variant in the normal tiers.

## Diagnostics that are not registered tests

`test_ui_modernization` has opt-in modes (`--resize-edges`, `--resize-controls`,
`--trim-checks`, `--text-checks`, `--window-lifetime`) for the resize
investigation described in
[project status](project-status.md). The strict resize gate currently fails
and is not part of any tier; a green run of the registered suite does not
close it.
