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
- Recording Copy places a file reference on the clipboard. Files still used by
  the clipboard are retained; unreferenced stale sessions can be cleaned later.
- Live sessions hold an exclusive lease. The preview helper uses private IPC,
  restricted handle inheritance and supervised lifetime management.

These boundaries do not protect against malware running as the same user or
an administrator. Blur and pixelation are visual effects; review exported
images carefully before sharing sensitive information.

## Build and repository checks

The C++ build enables strict warnings, SDL checks, control-flow protection,
ASLR and DEP. Portable binaries use the static runtime and Windows SDK libraries.
The release package is unsigned.

CI builds Debug and Release, runs checks suitable for hosted Windows runners,
and checks the tracked-file inventory. Interactive desktop and device coverage
must be validated separately; see [development](docs/development.md) and
[project status](docs/project-status.md).

`.gitignore` excludes local tooling configuration, credentials, private keys,
recordings, diagnostic dumps, build output and historical capture evidence.
`scripts/check_repository_hygiene.py` also rejects these paths if they are
force-added. Only exact reviewed image paths are allowed by
`scripts/repository-media-allowlist.txt`. This path check is not a substitute
for inspecting content for secrets before publication.

See [recording storage and clipboard lifetime](docs/decisions/ADR-011-recording-completion-and-clipboard-lifetime.md)
for the storage contract.
