# ADR-034 — Media Foundation is loaded on first use

**Status:** Accepted
**Date:** 2026-10-13

## Context

`Easnap.exe` imported `mfplat.dll` and `mfreadwrite.dll`. The N and KN
editions of Windows do not contain Media Foundation until the Media Feature
Pack is installed. There the loader refused to start Easnap at all, with a
system message about a missing DLL, although screenshots and Scrollshot need
nothing from Media Foundation.

## Decision

- `Easnap.exe` links both libraries with `/DELAYLOAD`. They are bound when the
  recorder, a trim export or the GIF exporter first calls into them.
- `platform::MediaFoundationAvailable()` loads both from the system directory
  once and keeps them. The coordinator passes the answer to the overlay
  (`OverlayController::SetRecordingAvailable`).
- Where it is false, the Record button stays in the mode bar and can be
  chosen. Choosing it does not change the mode; a message says that recording
  needs the Media Feature Pack and that screenshots and Scrollshot work
  without it. A greyed button would not say why.
- Record is the only way into the recorder, the trim window and the
  exporters. A recording recovered from an earlier run is saved by copying the
  file and needs no Media Foundation.
- `EasnapPreviewHost.exe` keeps its ordinary imports. It is started only for
  the trim window.

## Consequences

- `executable_imports` reads the import tables of the built `Easnap.exe` and
  fails when a Media Foundation library is needed at start, or when the two
  libraries are no longer in the delay-load table.
- `scrollshot_workspace_production` checks the refusal with
  `SetRecordingAvailable(false)`: the mode stays, the owner is told, and the
  same button enters Record once recording is available.
- A call into Media Foundation that bypassed this check on such a system would
  raise the delay-load exception and end in a crash report. There is no such
  path today.
- **Not verified on an N edition.** No such system was available; the
  behaviour there rests on the two tests above and on a real recording made
  with the delay-loaded build on a standard edition, where both libraries were
  absent from the process until the recording started.
