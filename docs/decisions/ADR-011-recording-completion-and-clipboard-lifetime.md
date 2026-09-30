# ADR-011 — Durable recording completion and clipboard-file lifetime

**Status:** Accepted  
**Date:** 2026-09-02

This decision records the MP4 completion contract. The implemented GIF extension
in [ADR-014](ADR-014-unified-recording-configuration-and-format-completion.md)
shares publication and lifetime rules, while requiring conversion from the MP4
working source even for a new full-range result. MP4 source-reuse rules below
do not bypass GIF conversion.

## Context

After trimming, Easnap must either publish a permanent MP4, expose the video for
clipboard paste, or discard the recording. Windows has no broadly interoperable
standard clipboard format for raw MP4 media. Shell file transfer is widely
supported, but a `CF_HDROP` payload contains paths to existing files, so deleting
the referenced temporary file when the trim window closes would corrupt the
clipboard contract.

## Decision

1. Exact full-range Save reuses the finalized recording and publishes it through
   the existing staged, atomic persistence path without trim re-encoding. A new
   interior Save tells `Mp4TrimExporter` the approved permanent destination, so
   its one encoded staging file is atomically published there without a second
   complete-file clone. An already cached matching trim is persisted normally.
   Existing destinations are rejected unless the native `IFileSaveDialog`
   overwrite prompt approved replacement. A destination inside Easnap's
   temporary root is not considered durable. The private finalized source is
   not moved for Save because preserving retry ownership and destination
   security semantics is more important than a same-volume rename.
2. Video Copy publishes one existing MP4 path as Unicode `CF_HDROP` and adds
   `CFSTR_PREFERREDDROPEFFECT = DROPEFFECT_COPY`. Easnap does not advertise raw
   MP4 clipboard bytes or promise that every application accepts file paste.
3. Private recording/source/trim sessions use a protected allow-list containing
   the concrete current process-user SID and LocalSystem. This is deterministic
   even when Windows `TokenDefaultOwner` differs from `TokenUser`, as can happen
   for elevated administrator tokens. Copy publication uses the same principals
   but reapplies its contract explicitly to both publication directory and final
   MP4. No Everyone or broad authenticated-user grant is used. Applying the file
   DACL is mandatory because a move or `CopyFileW` operation is not assumed to
   inherit the destination directory's security descriptor.
4. `WM_CLIPBOARDUPDATE` releases that session only after the clipboard can be
   inspected and no longer references its file. Clipboard-busy errors retain the
   file. On shutdown, a still-referenced session is released from process
   ownership without deletion. The next launch protects any session directory
   referenced by the current `CF_HDROP` payload while cleaning other stale
   sessions; once the clipboard changes, the directory becomes stale and is
   removed on a later launch.
5. A successful trim artifact is identified by path plus exact half-open
   `[in, out)` bounds. Save or Copy may reuse it only for an equal range and a
   still-existing non-empty file. A different range starts a new background trim
   and can never publish the stale result.
6. Save/Copy/export failure preserves the active recording session for retry.
   Save, Copy and confirmed Discard release preview/media handles, delete the
   active session when safe, and transition to `Idle`. Window close and `Esc`
   show the same explicit destructive-discard confirmation; close during export
   cancels and joins before deletion.
7. Copy minimizes whole-file work without weakening rollback:
   - exact full-range Copy uses the already standalone finalized recording as
     its input, avoiding decode/re-encode, but clones it once because the private
     source is the only recoverable recording and must not be moved or relabeled;
   - a trim requested specifically by Copy is encoded directly into prepared
     publication storage, avoiding a second full-file clone;
   - an existing derived trim may move into publication storage because the
     finalized source remains recoverable; move failure falls back to one copy;
   - clipboard publication failure retains the prepared readable artifact for a
     retry, while cancellation/export failure removes incomplete publication
     storage and leaves the private source unchanged.
8. Full-range Save and Copy do not enter trim export or show export progress.
   Real interior work uses one terminal encode and a compact delayed progress
   treatment with cancellation, avoiding a large indicator flash for fast work.

## Consequences

- File-oriented paste works in targets that accept Windows Shell file transfer;
  application-specific rich-media paste is intentionally not claimed.
- Clipboard-backed MP4s can outlive the process without becoming permanent
  leaks: current clipboard references protect them, clipboard replacement makes
  them eligible for deterministic or next-launch cleanup.
- Durable publication is separate from temporary trim generation, so a Save
  failure cannot destroy the only recoverable recording.
- Re-selecting the exact bounds can reuse the already generated artifact; any
  other bounds require a correctly matching export.
- The publication artifact remains disk-backed and bounded with video size; no
  MP4 payload is loaded into clipboard memory.
