# Project status

**Updated:** 2026-09-30
Read with the [roadmap](../EASNAP_ROADMAP.md) before implementation work.

## Current slice: initial v1 publication

This is the source baseline for the initial public **v1.0.0** release in
`vseprr/easnap`, with English documentation and a portable Windows x64 package. This supersedes the previous sequencing rule that
release preparation had not started; it does not imply acceptance of unresolved
Trim resize findings or completion of the wider compatibility matrix.

Implemented in this slice:

- CMake version 1.0.0 shared by both executable version resources and app metadata.
- Portable packaging of the matched application/helper, license, instructions,
  per-file hashes and a separate ZIP checksum; no signing or installer.
- English product README and release notes, with Scrollshot and OCR explicitly
  identified as future work.
- Public-repository hygiene: local tool configuration, prompts, private evidence,
  captures, credentials and signing material are excluded. A tracked-file check
  rejects force-added private paths and unreviewed media.
- Cleanup containment: canonical descendant checks, pinned directory handles,
  refusal to traverse reparse points and deletion through checked handles.
  Regression coverage uses fresh synthetic directories and sentinel files.
- Hosted CI builds Debug/Release and routes interactive/visual desktop checks
  to local validation. Hosted success does not assert physical device coverage.

## Validation for the publication package

- Release x64 build succeeded with warnings as errors. The final application
  and preview helper both report 1.0.0.0 and are packaged together.
- The registered suite was stopped at the user's request: **29 of 37 tests had
  completed and passed**, including continuous-seek and endpoint presentation.
  The in-progress test and remaining tests were not completed. This is not a
  full-suite pass; the interrupted runner did not produce a complete JUnit report.
- The final ancestor-sharing correction passed its focused production cleanup
  regression before the stop request. Its isolated old-guard comparison failed
  five assertions, confirming that the regression detects the defect.
- The final protected binaries were rebuilt after that correction and screenshot
  work. No further tests were run. Initial publication CI is intentionally skipped.
- The portable ZIP inventory, x64/version metadata, installed executable hashes
  and archive checksum were checked. No clean-machine installation or broad
  physical compatibility claim is made.
- Three reviewed screenshots were captured from the real application using a
  synthetic workspace and silent demo recording. Capture exclusion was disabled
  only for the photo build, then all 12 calls were restored byte-for-byte before
  the final build. The published source and package keep capture exclusion on.
- The public inventory excludes local settings, tool configuration, raw capture
  evidence, recordings and credentials. Selected credential/private-path scans
  found no matches. Public image files contain no added personal metadata.

Distribution: `Easnap-v1.0.0-windows-x64.zip` and its `.zip.sha256` file. The
archive contains the paired executables, MIT license, portable instructions and
`SHA256SUMS.txt` for packaged files. Binaries are unsigned. Private vulnerability
reporting is enabled on the repository.

## Retained product and validation boundaries

Screenshot capture/editing, MP4 recording, GIF export, audio options, pause/resume,
trim, clipboard publication and Light/Dark appearance are implemented. The
accepted Phase 3.3 and September 21 corrections remain accepted.

The September 30 seek fixes suppress duplicate gesture requests and reset the
preview liveness baseline after completed native commands. Earlier focused
Release/Debug timeline repeats covered 576 native clicks and 15,687 frames;
the separate 120 s backend run completed 539 seeks with the same helper.
The earlier full Release run was 35/36, followed by a passing affected 4/4
rerun. These historical numbers are distinct from the new release suite.

Strict resize diagnostics remain failed: 220 cases, 61,095 desktop frames,
59 visual-failing cases and 12 minimum-width assertions. All 4,185 settled
samples were clean. Transient video/control overlap and duplicated caption
pixels were observed during motion/maximize. Reboot cleared one reproduced
subset, but no underlying cause has been established. These failures are not
included in the ordinary registered suite and must not be erased by a green
CTest result. See the [Trim validation report](trim-endpoint-artifact-validation.md).
Raw media and machine-specific evidence manifests are retained locally.

The physical baseline is 125% scaling. Additional Windows/GPU/audio-device,
mixed-DPI, display-layout and accessibility environments remain unverified.
No indefinite leak-freedom claim is made.

Other retained limits: native startup/pause/drain can wait inside Windows;
full-range publication is synchronous; undo history is uncapped; late MP4 trims
decode from source start; codec endpoints and GIF timing/palette have granularity.
Low size GIF can omit tiny visual changes. Failed/cancelled publication retains
the source; a temporary output is never reported as a successful export.

Camera, Mouse Effects, device pickers, Scrollshot and OCR are not implemented.
Scrollshot is intended for v1.1; OCR remains post-v1 without a promised date.

## Next slice

Address the reproducible Trim resize findings and extend the physical
compatibility matrix. Resume a complete automated validation run when requested.
Preserve the native frame, shared rendering/control path and asynchronous
preview boundaries. See [ADR-024](decisions/ADR-024-native-trim-frame.md),
[architecture](architecture.md) and the [window guide](ui/window-implementation-guide.md).
