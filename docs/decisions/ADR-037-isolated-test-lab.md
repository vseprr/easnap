# ADR-037 — Isolated test lab in a Hyper-V virtual machine

**Status:** Accepted
**Date:** 2026-10-19
**Relates to:** [ADR-025](ADR-025-incremental-test-execution.md), [ADR-026](ADR-026-test-tiers-and-desktop-isolation.md)

## Context

[ADR-026](ADR-026-test-tiers-and-desktop-isolation.md) moved the message-driven
window tests onto a private desktop and kept the rest on the developer's own
desktop, with a warning, a chosen display and a countdown. Some tests cannot
leave the visible desktop: they verify the real pointer, the foreground window,
or pixels as actually presented on a monitor. Windows gives one pointer, one
keyboard focus and one input queue per logon session, and Desktop Duplication
and Windows.Graphics.Capture only see the visible desktop. On the machine someone
is using, these tests can therefore only be made less disruptive, not isolated.
ADR-026 names a second Windows session (a virtual machine or another PC) as the
complete answer.

The test suite falls into six groups, by the kind of environment each needs:

- Group 1: 22 unit tests.
- Group 2: 7 isolated-desktop tests (a private desktop, ADR-026).
- Group 3: 14 Media Foundation and preview tests (11 in the Standard tier and
  the 3 soak seeds of the continuous-seek test).
- Group 4: 3 audio tests.
- Group 5: 3 real-desktop-capture tests.
- Group 6: 6 desktop-visual and interactive tests.

Decisions taken with the owner:

- Hyper-V on the development PC first. VirtualBox only for a Windows Home
  edition, which the development PC is not.
- A spare laptop as a possible second provider later, not now.
- The tool is a separate, reusable repository, `C:\Projects\winlab`, not part of
  Easnap.
- Each project keeps one `testenv.json` that describes its guest, its commands
  and its test rules.
- Commands reach the guest through PowerShell Direct. The lab needs no network
  connection to the guest.
- Desktop commands run in the guest's console session through an interactive
  scheduled task.
- Every run starts from a golden checkpoint.
- The VM has a low CPU weight and fixed memory, so the host stays usable while
  it runs.

## Decision

The lab is a Hyper-V VM with Windows 10 Pro 22H2 (Turkish), one 1920x1080
display at 125 %, 4 virtual processors at a low CPU weight, 8 GB of fixed memory,
and automatic logon of the user `lab`. It has no GPU. VB-CABLE, a virtual audio
cable, is installed so that WASAPI tests have an endpoint.

`winlab` creates, provisions and snapshots the VM, restores the golden checkpoint,
copies the sources (ignore list from `testenv.json`, `build/` excluded), builds with
`scripts/build.ps1 -TestMode Build` over PowerShell Direct (compiles the tests, runs none), and runs
`scripts/test.ps1 -Tier <tier> -Mode Full -Monitor Primary -NoCountdown` in the
interactive session. The commands are described in
[testing.md](../testing.md#lab).

**Verdict.** Each run writes `build/Testing/lab/verdict.json` with the commit, a
tree hash, a file manifest (SHA-256 per file), the status and capture path of
each test, and a digest of the VM environment. It also writes a Turkish summary,
`build/Testing/lab-latest.md`, and a Windows toast.

**Direct build.** `scripts/build.ps1 -Trust Lab` asks winlab (`lab.ps1 verify`)
whether the latest verdict is green for exactly the current working tree. If it is,
the build runs without the tests and prints why. If not, the build lists the
changed, added and removed files and runs the tests as usual. Exit codes of
`verify`: 0 trusted, 2 tree differs, 3 not green or too old, 4 no verdict.

**Capture-path reporting.** The production provider tries Desktop Duplication and
falls back to GDI without saying so. The tests print
`EASNAP_CAPTURE_PATH=<Duplication|GDI|WGC>`. `test.ps1` reads these lines by the
rules in `testenv.json` (`tests.capturePathRules`), writes an
`easnap_capture_path` property into the JUnit file and a summary to
`build/Testing/easnap-capture-paths-<Configuration>.json`, and words a GDI
fallback as "passed (GDI fallback - Duplication path not exercised)".

**Host-only tests.** `tests.hostOnly` in `testenv.json` lists tests that must run
on the host. `test.ps1 -HostOnly Exclude` leaves them out and lists them; the lab
run uses it. `-HostOnly Only` runs exactly them on the host. The list is empty:
the probe of 2026-10-19 passed every capture and audio test in the VM. A test
that needs a real GPU is listed under `tests.gpuRequired`; winlab warns early,
because the VM has none. That list is empty too.

**Where tests run** (2026-10-20). The Standard and Full tiers run in the lab.
A run on the host desktop is made only when the owner asks for one.

**Audio.** `recording_pause_stop_stress` exits 77 (skipped) when no audio source
could be covered. winlab reports a pass of an audio test in a VM without an audio
endpoint as skipped, never as passed.

**Order for Groups 5 and 6.** First, the plain VM as described above, probed with
`lab.ps1 probe` (default WGC observer, then `EASNAP_TEST_CAPTURE=duplication`).
Second, GPU partitioning, and only with the owner's approval. Whatever still fails
after these steps is classified as host-only and runs on the host.

## Consequences

- Tests are run with `lab.ps1 run`. A build after an unchanged tree can use
  `-Trust Lab` and skip the tests, provided the latest lab run was green for
  that tree.
- The verdict proves that the listed tests passed for the recorded commit and
  tree, in the recorded VM environment, and which capture path each one used. It
  does not prove the host-only tests, real GPU behaviour, more than one display,
  or real speakers and microphones.
- **Group 5 fallback caveat.** The production provider falls back from Desktop
  Duplication to GDI silently. The VM has no GPU, so a Group 5 test can pass on
  GDI and look like a pass of the Duplication path. Such a result is reported as
  `VM-ok-but-fallback`, and it does not prove the Duplication path that the
  development PC uses. Only the printed capture path counts as evidence of which
  path ran.
- The VM is a local aid, like the pass cache in ADR-025. It is not a release gate
  for the host-only tests, which still need a run on the host. ADR-026 already
  asks for a Soak run before a release.

## Validation

Measured on 2026-10-19 in the lab VM (Windows 10 Pro 22H2 build 19045.2965,
Turkish, Microsoft Hyper-V Video adapter, 1920x1080 at 125 %, VB-CABLE
endpoints, 4 vCPU at weight 50, 8 GB; MSVC 14.44.35207, CMake 4.4.4).

**What the lab found first.** The first Standard run on the unchanged tree
failed the five MFPlay preview tests, and the first Full run failed those plus
`mfplay_preview_startup`, `real_desktop_recording_session`,
`recording_pause_stop_stress`, `ui_modernization_native` and
`trim_endpoint_presentation` (both on a modal "Preview failed while opening
(HRESULT 0xC00D36B2)" dialog until their timeout) and
`trim_timeline_seek_stress`: 11 of 47. The cause was in the product, not in the
lab: without a hardware H.264 encoder the Microsoft software encoder, asked
for Main profile with no encoder parameters, reorders frames, and the MP4 sink
stamps the first frame one frame late (33 ms at 30 fps). The preview's open
check then refuses the file with `MF_E_INVALIDREQUEST`, and the recording
tests' timestamp checks fail. Every PC without a hardware encoder was
affected. The writer now passes `CODECAPI_AVEncMPVDefaultBPictureCount = 0`
and `CODECAPI_AVLowLatencyMode` to the encoder
(`media_foundation_writer.cpp`). On the development PC (NVIDIA encoder) the
four Media Foundation background tests still pass with the change.

**Probe on the corrected tree** (`lab.ps1 probe`, tree `60e31538d0b4`, both
variants, every test passed; the capture path is the one the test printed):

| Test | WGC variant | Duplication variant | Capture path | Verdict |
|---|---|---|---|---|
| real_desktop_recording_session | passed, 49 s | passed, 50 s | not printed in this run (probe added afterwards) | VM-ok |
| recording_pause_stop_stress | passed, 16 s | passed, 16 s | not printed in this run | VM-ok |
| scrollshot_real_desktop | passed, 2 s | passed, 2 s | Duplication | VM-ok |
| ui_modernization_native | passed, 38 s | passed, 38 s | no observer in this mode | VM-ok |
| annotation_editing_native | passed, 19 s | passed, 18 s | no observer in this mode | VM-ok |
| trim_endpoint_presentation | passed, 54 s | passed, 54 s | WGC / Duplication | VM-ok |
| trim_timeline_seek_stress | passed, 60 s | passed, 59 s | Duplication | VM-ok |
| trim_timeline_seek_soak | passed, 164 s | passed, 164 s | Duplication | VM-ok |
| mfplay_preview_startup | passed, 25 s | passed, 25 s | not printed in this run | VM-ok |
| wasapi_audio_capture | passed, 11 s | passed, 11 s | (audio) | VM-ok |

Desktop Duplication works on the Hyper-V video adapter: no test fell back to
GDI, so GPU partitioning was not needed and no test is host-only.
`tests.hostOnly` stays empty.

**Timings.**

- Unattended installation from the ISO: 2 min 22 s to the first PowerShell
  Direct logon (file copy on an NVMe disk).
- Provisioning: 8 min for Build Tools, packages and the audio cable; a repeat
  run verifies in under a minute.
- Build in the VM (`build.ps1 -TestMode Build`, 4 vCPU at weight 50): 2 min
  58 s to 3 min 3 s.
- Standard tier in the VM: 36 s of tests; the whole run, including the
  checkpoint restore and the sync, 3 min 51 s.
- Full tier in the VM (unchanged tree, with the failures above): 12 min 49 s of
  tests. Probe: 9 min 44 s (WGC) and 7 min 24 s (Duplication) for ten tests
  including the soak seek run.

**Parity** (`lab.ps1 parity`): the VM differs from the development PC in the
update revision (19045.2965 against 19045.7807), one display instead of two,
the Hyper-V video adapter instead of the RTX 4060 Ti, four audio endpoints
instead of seven, CMake 4.4.4 against 4.0.0 and 4 logical processors against
16. OS build, edition, locale, time zone, scale, MSVC toolset and Windows SDK
agree.

**Suite runs on the corrected tree.** Standard: 39 of 39 passed (1 min 36 s
of tests, 4 min 59 s for the whole run). Full: 46 of 47 passed (6 min 38 s of
tests, 10 min 5 s for the run); `trim_timeline_seek_stress` failed twice in
that run at its pointer check ("Seek case aborted: pointer authentication
failed") after passing in both probe variants, so in the VM it is unstable in
the same way the host run of 2026-10-13 was. It is not host-only: it passes
in the VM, and the pointer check is the test's own guard. A further Full run on
the final tree passed 47 of 47 (6 min of tests, 9 min 20 s for the run) and
was the first to carry the recording tests' capture-path line, printed at the
start of each test because CTest keeps only the head of a long output:
`recording_pause_stop_stress` reported Duplication, while
`real_desktop_recording_session` reported GDI, so the verdict marked it
`VM-ok-but-fallback`. That is the provider's documented behaviour on a still
desktop (no new Duplication frame, GDI for the picture), observed on a desktop
nobody touches; it is exactly the caveat this ADR asks the runner to surface,
not a VM limitation.

## Not validated

- The lab verdict has not yet been compared with a Full run on the host for
  the same tree; the host-side Full run after the writer change is pending.
- The VM has not received Windows updates (update revision 2965); whether the
  later revision changes any result is not measured.
- `mfplay_preview_startup`, `ui_modernization_native` and
  `annotation_editing_native` report no capture path: those modes use no
  capture observer, so "capture path unknown" is the honest reading.
- Windows 11, N editions and other scale factors remain unmeasured; the lab
  runs one Windows 10 22H2 image.
