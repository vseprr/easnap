# ADR-040 — Opening an external video in the trim view

**Status:** Proposed; nothing in this record is implemented yet  
**Date:** 2026-11-09

## Context

The trim view was built for one source: the recording Easnap has just made.
The flow in `src/app/trim_flow.cpp` and the window in `src/ui/trim_window.cpp`
rely on that in six places. Opening a video the user already owns breaks each
of them.

- **Temporary results go beside the source.** The trimmed copy is written to
  `trimmed-N.ext` in the folder of the source (`trim_flow.cpp`). For a user's
  file that would write into the user's own folder.
- **The exporters accept one shape.** `mp4_trim_exporter.cpp` and
  `gif_exporter.cpp` take an absolute `.mp4` path with H.264 video and AAC
  audio, and refuse anything else with a generic failure. The preview has no
  such check: a playable HEVC file or a `.mov` previews fine and then every
  export fails.
- **A second instance starts a capture.** `main.cpp` posts the registered
  message `Easnap.StartCapture` with no data and the running instance calls
  `OnGlobalHotkey`. "Open with" on a running Easnap would therefore start a
  screen capture. When Easnap is not running, the file argument is ignored.
  The only command-line test is a `wcsstr` for `--startup`, so a path that
  contains that text would start Easnap silently.
- **Discard deletes.** The close button is "Discard" (Turkish "Sil") and asks a
  destructive question; a preview that fails to open offers Save, Delete or
  Keep, and its messages say the recording "was kept"
  (`trim_flow.cpp`, `strings_en.rc`, `strings_tr.rc`).
- **Exit and sign-out treat the video as unsaved work.** `PendingWorkFor` in
  `session_transitions.cpp` reports `UnsavedRecording` while trimming and
  `Export` while exporting; the texts say "If you exit now, it is deleted"
  ([ADR-031](ADR-031-unsaved-work-survives.md)). `PrepareForSessionEnd` marks
  the recording recoverable and the next start offers to recover it.
- **The state machine has one way in.** `TrimFlow::Begin` is called only from
  `FinishRecordingWorkflow`, and `session_transitions.cpp` allows only
  `RecordingFinalizing → RecordingTrimming`.

Two more facts shape the design. The trim window is not yet in
`text_fits_in_every_language`. Both exporters decode from time zero and never
seek; the GIF exporter does it twice.

## Decision

An external video opens in the same trim view as a recording. Easnap checks it
first, tells the user in plain words when it cannot open it, and never changes
or deletes the file.

### One support table

| Item | Decision |
|---|---|
| Where | One table in `src/media`, a header and its source; nothing else lists extensions or codecs. |
| What it maps | Extension → container → allowed video subtypes and allowed audio subtypes → the refusal reason for a file that does not fit. |
| Who reads it | The probe, both exporters, and a Fast test that compares the installer's extension list with it. |

Contents in v1:

| Extension | Container (source) | Video subtypes | Audio subtypes |
|---|---|---|---|
| `.mp4`, `.m4v`, `.mov` | MPEG-4 | H.264; MPEG-4 Part 2 (MP4V, M4S2); MJPEG | AAC, MP3, PCM, or none |
| `.wmv`, `.asf` | ASF | WMV3, WVC1 (VC-1) | WMA v8/v9, or none |
| `.avi` | AVI | H.264; MPEG-4 Part 2; MJPEG | AAC, MP3, PCM, or none |

These are the combinations the table allows and the tests cover (see
Validation). A subtype that Windows can decode but the table does not list for
that container is refused as "format not supported"; widening the table is one
edit plus one test file.

Deliberately not in v1: `.mkv`, `.webm`, HEVC, VP9 and AV1. The table names
them so the probe can answer "format not supported" instead of "unknown".
HEVC is the one exception in wording: when the extension is present but the
Windows decoder is missing, the answer is "needs a Windows codec extension".

### Probe and reasons

The probe runs on a worker thread inside `Easnap.exe`. It opens the file with
the Source Reader on a handle Easnap opened itself, with a deadline of 5
seconds, and posts the result to the host window. The trim view opens only
after a successful probe. A helper-process probe (a new packet in the preview
helper protocol) would keep a parser crash out of `Easnap.exe`; it is recorded
as a later option, not part of v1.

The probe checks, in this order: Media Foundation is available; the path is
absolute, local, not a URL, a regular file, not on a network or offline
location; the extension is in the table; the file can be opened for reading;
the Source Reader opens it; there is a video stream; the video and audio
subtypes are in the table; the first frame decodes. It records the file's
identity, size and modification time, and the facts the window needs.

What the user is told:

| Reason | Meaning in the user's words |
|---|---|
| Unsupported extension | Easnap does not open this kind of file. |
| Format or codec not supported | The extension is supported but the video or audio inside is not. Includes "this needs a Windows codec extension". |
| Damaged or unreadable | The file is damaged or is not a video. |
| Protected | The file is copy-protected. |
| No video | The file has sound but no picture. |
| Not supported yet | Rotation, a pixel shape that is not square, interlaced video, or 10-bit/HDR. |
| Too large for MP4 | Over the H.264 size limit; GIF is still allowed and the message says so. |
| Network or offline file | Copy the file to this PC first. |
| In use | Another program is using the file. |

The mapping from a result to a reason is one pure function over the probe's
facts and an HRESULT, so it is tested in the Fast tier without a file. The
HRESULT mapping:

| Result | Reason |
|---|---|
| `MF_E_UNSUPPORTED_BYTESTREAM_TYPE` (0xC00D36C4), `MF_E_INVALID_FILE_FORMAT` (0xC00D36BE) on a supported extension | Damaged or not a video |
| No `MFMediaType_Video` stream | No video |
| Subtype not in the table | Format not supported |
| Subtype in the table, `SetCurrentMediaType` returns `MF_E_TOPO_CODEC_NOT_FOUND` (0xC00D5212) or `MF_E_INVALIDMEDIATYPE` | Needs a Windows codec extension |
| `MF_SD_PROTECTED`, `MFRequireProtectedEnvironment`, `MF_E_DRM_UNSUPPORTED` (0xC00D3700), `MF_E_LICENSE_*` | Protected |
| First-frame decode fails with `MF_E_INVALID_STREAM_DATA` | Damaged or unreadable |
| `ERROR_SHARING_VIOLATION` (0x80070020) from `CreateFileW` | In use |
| `ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND`, `ERROR_ACCESS_DENIED` | Own wording: "file not found" or "no access" |
| Probe deadline passes | Network or offline file when the location says so; otherwise damaged or unreadable |

The wording of the last two rows: "The file could not be found." and "Easnap
is not allowed to read this file." The HRESULT values are verified against
real files in the probe tests; a value that turns out to differ is corrected
here.

Rotation, a pixel aspect other than 1:1, interlaced video and 10-bit or HDR
profiles are refused because nothing in the exporters handles them: they would
export unrotated, with the wrong shape, undeinterlaced or not at all, and the
preview would not match the result. A file whose first frame is not at time
zero is refused only if the preview cannot be fixed cheaply; the first
implementation slice decides and records it.

### Source ownership

`TrimFlow::Begin` takes a `TrimSource`:

| Field | Meaning |
|---|---|
| kind | `Owned` (a recording Easnap made) or `External` (a file the user opened) |
| path | absolute path of the video |
| identity | volume serial and file index, size and modification time, taken at probe |
| display size, monitor anchor | where and how large to open the window |
| probe facts | duration, size, frame rate, audio presence, subtypes |

An external source also carries a work session from the temporary store. Its
temporary results (trimmed copies, GIF output, staged copy-to-clipboard files)
live there and never beside the source. The work session is never recoverable
and is removed when the flow ends.

**Invariant: the user's file is never changed or deleted.** The source path of
an external flow never reaches delete, move or access-list code. Today this
holds for the reuse path only because `allow_move = artifact != source`; the
new code makes it structural and a test asserts it. Save refuses the source as
a destination by file identity, not by comparing path strings, and shows its
own message.

| | Owned | External |
|---|---|---|
| Temporary results | the recording's session folder | the work session |
| Close button | "Discard" / "Sil", destructive question | "Close" / "Kapat", no question unless an export is running |
| Preview fails, then close | Save / Delete / Keep question | the window just closes |
| Exit or sign-out | asks whether to exit; the recording is kept | nothing pending; during an export it asks, and says the export stops and the original is unchanged |
| Recovery at next start | offered | never |
| Save default name | `Easnap-<timestamp>` | `<name> (trimmed)` |
| Full-range Save or Copy | reuses the file | reuses it only for MP4 + H.264 + AAC or no audio + no rotation; otherwise exports |
| Format | fixed by the recording | chosen per action: the Save dialog offers MP4 and GIF types; Copy offers an MP4 or GIF choice |
| `release_recording` | releases the session | does nothing |

A cached export result is keyed by format so that switching between MP4 and
GIF does not reuse the wrong file. The window title shows the file name. The
strings that say "recording kept" or "deleted" are neutral or have an external
variant; new string IDs are 21300–21399 in both tables
([ADR-032](ADR-032-interface-text-in-string-tables.md)).

### Session states and pending work

The state machine gains an `Idle → RecordingTrimming` edge, and `PendingWorkFor`
takes the source kind. The single table of pending work in
[ADR-031](ADR-031-unsaved-work-survives.md) stays the one place that answers
"what would Exit lose": for an external source it answers none while trimming
and `Export` while exporting. No new state is added for external videos.

### Export behaviour

Both exporters decode every table entry through the Source Reader, asking for
NV12 video and float audio. Two behaviours change:

- An interval that holds no audio, in a file that has an audio track, exports
  silence or no audio track instead of failing (the MP4 exporter fails today).
- The bitrate default stays the one in `RecordingOptions` (8 Mbps). A
  high-quality source is re-encoded at that rate; this is a known quality limit.

Decoding still starts at time zero and does not seek, so trimming the end of a
long file costs the time to decode up to that point (twice for GIF). This is a
documented limit; keyframe seeking is a later slice
([ADR-009](ADR-009-mp4-trim-boundaries-and-publication.md) allows it).
Variable frame rate is supported: timestamps are kept and the nominal rate is
the rounded average. Several audio tracks are supported only if a test shows
the preview and the export use the same one.

### Entry points

| Entry | Behaviour |
|---|---|
| Tray "Open video…" | Placed after Capture, enabled only when Idle. The file dialog declares a `platform::ModalLoopScope`. |
| `Easnap.exe --open <path>` and a bare path | What Explorer's "Open with" runs. Real argument parsing with `CommandLineToArgvW` replaces the `--startup` substring check. |
| Second instance | Hands the path to the running one with `WM_COPYDATA` and exits. If no instance runs, the new one opens the file. |
| Installer "Open with" | HKCU entries only (ProgID `Easnap.Video`, `OpenWithProgids` per extension, `Applications\Easnap.exe\SupportedTypes`). Offered, never made the default, `UserChoice` never touched, removed at uninstall. The portable ZIP writes nothing. |

The hand-over message is a request from any process on the desktop, so the
receiver validates it: one absolute, local, existing file, with a bounded
length. It is treated as a user request, not a trusted command: it is posted
to the receiver's own window and acted on only when the state is Idle and no
dialog is open; otherwise it is dropped with a log line. The existing
`Easnap.StartCapture` path is unchanged for the capture shortcut.

The installer's extension list is the table's list. A Fast test reads
`packaging/easnap.iss` and fails when the two differ.

There is no drag-and-drop in v1.

### Not supported in v1

| What | Why |
|---|---|
| `.mkv`, `.webm`, HEVC, VP9, AV1 | Decoders are optional or paid on Windows 10; the table can grow later. |
| Rotation metadata | Exporters ignore it, so trimmed and untrimmed results would differ. |
| Non-square pixels, interlaced, 10-bit and HDR | Exporters have no pixel-aspect, deinterlacing or tone handling. |
| Network drives, offline and cloud-placeholder files | A blocked read stalls the export worker, and `CancelAndWait` at Exit would block the UI. |
| Non-AAC audio in a re-muxed full-range reuse | Reuse is only for MP4 + H.264 + AAC or none; other audio is re-encoded. |
| Keyframe seeking | Later slice. |
| Drag-and-drop | Not decided for v1. |
| Probe in the helper process | Recorded as a later option. |

## Amendments

- [ADR-010](ADR-010-trim-view-ownership-and-orchestration.md) decision 1: the
  flow has two source kinds, `Owned` and `External`; ownership rules in this
  record apply to the second.
- [ADR-031](ADR-031-unsaved-work-survives.md): an external file is not unsaved
  work. Only an export in progress is pending, and it loses nothing but its own
  result.
- [ADR-034](ADR-034-media-foundation-on-first-use.md): Record is no longer the
  only way into Media Foundation. The probe and the trim view start it too, on
  first use, with the same availability check.

## Consequences

- One table replaces three implicit lists; adding a format is one edit and the
  installer test keeps the shell entries honest.
- Easnap parses untrusted files inside `Easnap.exe` on a worker thread. A
  parser crash ends the process; the helper-process probe is the mitigation
  kept for later.
- Large or slow sources decode from zero; the full-range reuse path still
  copies on the UI thread (`CopyFileW` in `recording_completion.cpp`), which
  stays a limit for large files.
- The 8 Mbps default lowers the quality of high-bitrate sources, and 4K GIFs
  are large.
- The window and the strings now carry two source kinds; both are measured in
  the text-fit test.
- Registered file types are offered, never default, so installing changes no
  existing association.

## Validation

Nothing below has been run; this is the plan the implementation slices follow.

- **Fast tier.** Table contents; the classifier from facts and HRESULTs,
  including protected and a missing codec extension; the installer list equals
  the table; argument parsing (path containing `--startup`, quoted paths,
  `--open`, bare path); the hand-over validator (relative, UNC, non-existing,
  overlong and malformed payloads rejected).
- **Probe tests (Standard)**, with files written by the production writer:
  640×360 at 30 fps, 1920×1080 at 60 fps, 360×640, 24 fps, different
  durations, with and without audio, a one-frame file, a long file (Soak), and
  a `.wmv` written with the sink writer. Bad input: random bytes with a `.mp4`
  extension, an unsupported extension, an audio-only file written with an
  AAC-only sink writer. Access: a file locked exclusively and a read-only file
  (read-only must open).
- **`trim_flow_production`** cases for external sources: Copy of the whole
  range and of a part, GIF Copy, Cancel, Close and Destroy while exporting,
  and the Exit answer. After every case: the source folder holds no new file,
  and the source's bytes, timestamps and ACL are unchanged; the work session is
  removed.
- **Hand-over test.** The test executable relaunches itself with `--send
  <path>` using the production sender; the receiver acts only when idle.
- **Text fit.** The reason dialog and the trim window, both source kinds, in
  `text_fits_in_every_language`.
- **Owner checklist** (test-created files only, evidence kept out of the
  repository): open from the tray; open with "Open with" while Easnap runs and
  while it does not; Close and Exit during an export; Save with the source's
  own name refused; Save as MP4 and as GIF; the source file untouched
  afterwards.
- **What the lab can and cannot verify.** The lab can run everything above
  that needs no extra decoder. It has no HEVC extension unless one is
  installed there, so "needs a Windows codec extension" is verified by the
  classifier only and not on a real file. It has no network drive, so the
  network and offline refusals are verified by the classifier and by path
  rules only. The installer entries and Explorer's "Open with" are not run in
  the lab. The measured figures (probe time, export time for a long file) are
  recorded here when the slices are validated.
