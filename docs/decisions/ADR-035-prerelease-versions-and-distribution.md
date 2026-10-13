# ADR-035 — Prerelease versions, one channel, published symbol files

**Status:** Accepted
**Date:** 2026-10-13

## Context

The version was three numbers in `CMakeLists.txt`, repeated in the version
resource, the package names, the installer and the release workflow. None of
them could say "beta". A beta published under a plain version number would
have been offered as the latest release and could not have been told apart
from the final version in a bug report. Release builds had no symbol files, so
the crash reports of [ADR-030](ADR-030-log-file-and-crash-reports.md) could
not have been read.

## Decision

### One place for the version

```cmake
project(Easnap VERSION 1.1.0 ...)
set(EASNAP_VERSION_PRERELEASE "beta.1")   # "" for a stable release
```

- The **version text** is `1.1.0-beta.1`: About, the product version of both
  executables, the first line of the log, the package names, the release tag
  `v1.1.0-beta.1`.
- The **file version** stays numeric (`1.1.0.0`). Windows compares it, and the
  installer uses it.
- A prerelease sets `VS_FF_PRERELEASE` in the version resource.
  `package-release.ps1` refuses executables whose product version, file
  version or prerelease flag disagree with `CMakeLists.txt`.
- The Release workflow accepts a tag with a label, requires it to equal the
  version text, and creates the draft as a prerelease, which GitHub does not
  offer as the latest release.

### One channel

A beta is the next version of the same program, not a second program. It
keeps the installer identity, the single-instance mutex and the data folder.
Installing a beta replaces an installed stable version and reads its settings;
going back means installing the stable version again. Settings written by the
beta are read by v1.0.2 as far as it knows the keys.

Two programs side by side would need a second identity for the installer, the
mutex, the shortcut, the notification icon and the data folder, and a way to
move recordings between them. Nothing in the beta needs that isolation.

### Package managers

`scripts/package-manifests.ps1` writes winget and Scoop manifests from the
packages and their checksum files; it submits nothing. A prerelease gets the
winget identifier `vseprr.Easnap.Beta`, so that `winget upgrade` never moves a
stable installation to a beta. See [distribution](../distribution.md).

### Symbol files

Release builds of the two executables produce symbol files. The executables
record the bare file name, not the build directory. `package-release.ps1`
checks that, packs the two files into `...-symbols.zip` and the Release
workflow attaches it to the release. Symbol files are not installed.

### Uninstalling

The uninstaller removes the sign-in entry that Settings may have written and
asks whether settings, log and crash reports should go as well; the default,
and the answer of a silent uninstall, is to keep them. A recording that was
not saved is never removed. The installer is in English and Turkish and
requires Windows 10 build 19041.

## Consequences

- A version is changed in one file. The package script and the workflow read
  that file; nothing else carries a version number.
- A user can tell from About, and a maintainer from a bug report, which build
  is running.
- Publishing symbol files makes the binaries easier to inspect. The sources
  are public; nothing is given away.
- **Not verified:** the Release workflow with a prerelease tag (no workflow
  was run), the installer's uninstall questions and removal of the sign-in
  entry (the installer was compiled, not run: installing would replace the
  copy of the person who owns the development PC), and the manifests against
  `winget validate` or Scoop.
