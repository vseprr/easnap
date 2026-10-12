# Security policy

Easnap handles screen and audio content. Use synthetic media when reproducing a
problem and report security vulnerabilities privately.

## Reporting a vulnerability

Use [GitHub private vulnerability reporting](https://github.com/vseprr/easnap/security/advisories/new).
Include the affected version, Windows version, reproduction steps and expected
impact. Do not include private captures, credentials or personal file paths.
Do not post exploitable vulnerabilities in public issues. Reports are handled
by the maintainer; no fixed response-time guarantee is offered.

The latest 1.x release receives fixes. Older releases are not maintained as
separate security branches.

## Data and execution

- Capture, editing and export run locally. The application has no telemetry,
  automatic upload or update service.
- The application runs as the current user without requesting elevation.
- Screenshot content remains in memory until the user saves or copies it.
- Recordings use session directories under `%LOCALAPPDATA%\Easnap\temp`, with
  permissions restricted to the current user and SYSTEM.
- Export stages output before publication. Failed or cancelled publication
  preserves the source for recovery.
- A finished recording that was neither saved nor discarded when Easnap ended
  stays in its session directory and is offered at the next start.
- A log is kept in `%LOCALAPPDATA%\Easnap\logs` (three files of one megabyte).
  Its lines carry no path, file name, window title or typed text.
- When Easnap or its preview helper fails, a crash report is written to
  `%LOCALAPPDATA%\Easnap\crashes`; the three newest are kept. A report has no
  copy of the process's memory heap, but it holds the call stack of every
  thread, which can contain fragments of what was being handled. Nothing is
  sent anywhere: look at a report before you attach it to an issue.
- Recording Copy places a file reference on the clipboard. Files still used by
  the clipboard are retained; unreferenced stale sessions can be cleaned later.
- Live sessions hold an exclusive lease. The preview helper uses private IPC,
  restricted handle inheritance and supervised lifetime management. The
  application range-checks what the helper reports before acting on it.
- Cleanup deletes only inside `%LOCALAPPDATA%\Easnap`, through checked handles.
  It refuses reparse points and verifies that every entry it opens still
  resolves to a direct child of the directory it was listed in.
- Both executables load libraries from the Windows system directory only. A DLL
  placed beside a portable copy is not loaded.
- Saving a screenshot writes a temporary file beside the destination and
  replaces the destination only after encoding succeeded.

These boundaries do not protect against malware running as the same user or
an administrator. Blur and pixelation are visual effects; review exported
images carefully before sharing sensitive information.

## Build and repository checks

The C++ build enables strict warnings as errors, SDL checks, Control Flow Guard,
high-entropy ASLR, DEP and CET shadow-stack compatibility, and resolves static
imports from the system directory (`/DEPENDENTLOADFLAG:0x800`). Binaries use the
static runtime and Windows SDK libraries only.

Release binaries and the installer are **unsigned**. Each package has a SHA-256
file, which detects a corrupted download but not a replaced one, because it is
published alongside the package. From v1.0.1 each package also has a build
provenance attestation that ties it to the workflow run that built it:
`gh attestation verify <file> --repo vseprr/easnap`.

CI builds Debug and Release with third-party actions pinned to commits and a
read-only token, runs the tests suitable for hosted Windows runners, and checks
the tracked-file inventory. Tests that need a real display, pointer or audio
device are validated separately; see [testing](docs/testing.md) and
[project status](docs/project-status.md).

`.gitignore` excludes local tooling configuration, credentials, private keys,
recordings, diagnostic dumps, build output and historical capture evidence.
`scripts/check_repository_hygiene.py` also rejects these paths if they are
force-added. Only exact reviewed image paths are allowed by
`scripts/repository-media-allowlist.txt`. This path check is not a substitute
for inspecting content for secrets before publication.

See [recording storage and clipboard lifetime](docs/decisions/ADR-011-recording-completion-and-clipboard-lifetime.md)
for the storage contract.
