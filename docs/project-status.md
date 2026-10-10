# Project status

**Updated:** 2026-10-10
Read with the [roadmap](../EASNAP_ROADMAP.md) before implementation work.

## Current slice: v1.0.1 maintenance and hardening

v1.0.1 follows the published v1.0.0 with fixes, hardening and test tooling.
No feature was added and no phase was started. Distribution is a per-user
installer and a portable ZIP built from the same two executables, both
unsigned, each with a SHA-256 file and a build provenance attestation.

### Implemented in this slice

Defects fixed:

- Both-source audio health is read directly from the endpoints instead of
  waiting for the mixer thread's poll; a failed Both session cannot be
  restarted before `Stop()`. This removed the intermittent
  `wasapi_audio_capture` failure.
- A failed WASAPI `GetBuffer` is classified before the empty-buffer exit, so
  endpoint loss is recorded as a capture failure.
- The recording audio gate fails the session after 10 s without video progress
  instead of blocking Pause/Stop/Abort; `Failed` is no longer overwritten by
  Pause, Resume or Stop.
- A black frame after real content no longer demotes Desktop Duplication to
  the GDI blit; the region staging texture is reused; the cursor is composited
  through a cursor-sized surface and skipped when outside the region.
- Editor: stale selection after Esc/Delete/undo no longer indexes past the
  annotation list; property controls commit only transactions they opened.
- `SaveImage` stages beside the destination and replaces it only on success.
- The tray menu ignores the duplicate right-click message; the idle trim
  timeline is not repainted every tick.

Displays and scaling:

- Both executables declare per-monitor v2 in their manifests. `VS_DPI_AWARE`
  had embedded per-monitor v1, which the run-time request could not raise, so
  the v1.0.0 binaries ran in a different DPI mode than the tests.
- A work-area-only change no longer cancels screenshot editing; a recording
  whose region left the desktop ends through the Stop path; a maximized or
  minimized trim window is not resized on display changes; the recording
  island adopts the suggested rectangle on a DPI change; the editor context
  island is re-fonted; selection-handle hit areas scale.

Security:

- Deletion verifies that each opened entry still resolves to a direct child of
  the directory it was listed in (junction race).
- Library search is restricted to System32 at run time and for static imports
  (`/DEPENDENTLOADFLAG:0x800`); `/CETCOMPAT` and `/HIGHENTROPYVA` are enabled.
- The helper's stderr no longer shares the protocol pipe; reported snapshot
  values are range-checked.
- CI pins third-party actions to commits, does not persist the token, and
  attests release packages.

Tests and tooling ([testing](testing.md),
[ADR-026](decisions/ADR-026-test-tiers-and-desktop-isolation.md)):

- Tiers: fast, integration, desktop, soak. Repeated stress seeds, 30-second
  seek runs and the 72-click timeline run are soak; no assertion was removed.
- Three message-driven window tests run on a private, non-input desktop.
- Visible-desktop tests run last on a chosen display with a warning,
  countdown, pointer restore and one reported retry.
- The pass cache hashes only `EASNAP_*` environment variables.
- New table-driven tests cover trim layout across 10 work areas and 9 scale
  factors, and selection hit areas across scales and negative origins.
- Per-user installer (`packaging/easnap.iss`) built by `package-release.ps1`.

## Validation

Environment: one development PC, Windows 10, two 1920x1080 displays at 125 %
(144 Hz primary, 60 Hz secondary). Visible-desktop tests ran on the secondary
display while the PC was in use.

- Release x64 build with warnings as errors succeeded. Both executables report
  1.0.1.0, and a running `Easnap.exe` was confirmed per-monitor v2 through
  `GetDpiAwarenessContextForProcess`.
- **All 39 registered tests passed on the final binaries, but not in a single
  run.** The complete Soak run passed 37; `trim_timeline_seek_soak` and
  `annotation_editing_native` failed, and failed again on the automatic retry.
  After the UI test executable was relinked, the affected rerun (6 tests,
  including those two) passed 6 of 6.
  - `trim_timeline_seek_soak` failed with "pointer authentication failed": the
    real pointer was moved while the test was using it. That is the test
    working as designed, not a product failure.
  - `annotation_editing_native` is intermittent. Within this slice it failed
    in four suite runs and passed in every standalone repetition (11 of 11)
    and in the final rerun. Its failing assertions compare the presented text
    box with the source-backed renderer. See the open item below.
- Timing on this machine: the Full tier ran 26 background tests in 91 s and 7
  visible-desktop tests in 274 s. The previous suite of 36 took 790 s. The
  Soak tier ran 31 background tests in 195 s and 8 visible-desktop tests in
  390 s. After a rebuild that changed one test executable, 6 tests ran and 33
  were reused.
- The intermittent `wasapi_audio_capture` failure was not reproduced on demand
  before the change (12 of 12 passes); after it the test passed in every run.
  Its fix is established by reading the code, not by a reproduced failure.
- The installer was built, installed silently into a scratch directory, the
  installed application started and reported 1.0.1.0, and the uninstaller
  removed the files and the registration.
- Hosted CI (`windows-2022`) built Debug and Release and passed the 20 tests
  it can execute. Hosted tests had never run for this repository before; the
  first run failed on the preview tests (the image cannot open an MFPlay
  player), on `capture_overlay_interaction` (click targets assume a larger
  desktop) and on the selection self-test (`Get-FileHash` unavailable). The
  first two are now excluded from hosted CI and remain local-only; the third
  was fixed.
- v1.0.1 was built by the Release workflow from the tag and published. The
  published ZIP and installer were downloaded again and checked: SHA-256 files
  match, `gh attestation verify` succeeds for both, both executables report
  1.0.1.0, and the published installer installed, ran and uninstalled cleanly.

Not validated:

- Endpoint loss was not exercised by unplugging a device. Display changes
  (disconnecting a monitor, changing scale or resolution during a session)
  were not exercised physically; they are covered by code review and by the
  table-driven layout tests only.
- The installer was not tried on a clean machine or another Windows version.
- No scale factor other than 125 % and no mixed-DPI layout was used physically.

Open item from validation: when the editor test was changed to wait about
30 ms before a key press in a reopened text box, the presented text box stayed
different from the source-backed renderer for more than 1.5 s in 4 of 5 runs.
That change was discarded because it made the test fail, but the observation
may point at a real repaint gap after reopening a text annotation and has not
been investigated.

## Known limitations and unresolved risks

Carried over from v1.0.0:

- Strict resize diagnostics remain failed: 220 cases, 61,095 desktop frames,
  59 visual-failing cases and 12 minimum-width assertions; all 4,185 settled
  samples were clean. Transient video/control overlap and duplicated caption
  pixels occur during motion/maximize. No cause is established. These
  diagnostics are opt-in modes, not registered tests; a green CTest run does
  not close them. See the
  [Trim validation report](trim-endpoint-artifact-validation.md).
- Native startup/pause/drain can wait inside Windows; full-range publication
  is synchronous; undo history is uncapped; late MP4 trims decode from source
  start; codec endpoints and GIF timing/palette have granularity. Low size GIF
  can omit tiny visual changes.

Found in the v1.0.1 review and not fixed:

- The editor rebuilds the whole redacted image on every pointer move while a
  Blur/Pixelate/Spotlight effect is previewed or transformed. On a 4K capture
  that is tens of megabytes of copying per move on the UI thread.
- No size pre-check before H.264 encoding: a region beyond the encoder limit
  fails at start with a general message.
- The capture path still allocates a frame image and an NV12 buffer per frame.
- MP4 trim reads the frame size once and does not apply the decoder display
  aperture the way GIF and preview do. A 160x120 source (padded by the
  decoder) trims to the correct size on the GPU path, which is now tested;
  pixel correctness on the system-memory NV12 path is unverified.
- The application's UI thread writes acknowledgements to the helper pipe
  synchronously; a helper that never reads could block it. The helper is the
  application's own process, so this needs a faulty helper.
- `CreateSession` follows a reparse-point storage root that deletion would
  refuse. A session lease that cannot be reacquired after a failed delete is
  only logged.
- Mixed-DPI drags, HDR outputs, L-shaped monitor layouts, a top-docked taskbar
  over the mode toolbar and the configuration island at 200 % on a 768-row
  display are unverified or known to be imperfect.
- Not every visible-desktop test follows the chosen display.
- `capture_overlay_interaction` fails three checks on the hosted runner's
  small desktop. Whether that is only a test assumption or also a layout
  problem at low resolution is not established.
- CI is pinned to `windows-2022` because the project uses the Visual Studio
  17 2022 generator; moving to a newer toolchain is pending.
- `annotation_editing_native` is intermittent (see Validation).

Camera, Mouse Effects, device pickers, Scrollshot and OCR are not implemented.
Scrollshot is intended for v1.1; OCR remains post-v1 without a promised date.

## Next slice

Hardening continues before feature work; the ordered list is in the
[roadmap](../EASNAP_ROADMAP.md). Preserve the native frame, shared
rendering/control path and asynchronous preview boundaries. See
[ADR-024](decisions/ADR-024-native-trim-frame.md),
[architecture](architecture.md) and the
[window guide](ui/window-implementation-guide.md).
