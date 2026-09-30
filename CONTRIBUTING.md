# Contributing to Easnap

Easnap is a native Windows application built with C++20 and the Windows SDK.
Use synthetic screens and audio when reporting a problem. Keep personal
captures, machine paths and tokens out of issues and pull requests.

## Build and test

Install Visual Studio 2022 with Desktop development with C++, a Windows 10 or
11 SDK, and CMake 3.20 or newer. Use a Developer PowerShell from the repository
root:

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Debug
```

The default affected-test run reports physical desktop checks as pending.
For a full local check, leave the desktop unlocked and input idle, then run:

```powershell
.\scripts\build.ps1 -Configuration Release -TestMode Full
```

Native capture, pointer, display and audio checks need a usable Windows desktop
and relevant hardware. Hosted CI excludes tests labeled `interactive-desktop`
or `desktop-visual` and reports them as pending. Its remaining media/audio
checks still depend on the runner's available Windows components. A hosted
green check does not validate physical display or audio configurations.

## Changes

Keep fixes focused. Explain the triggering behavior, the resulting change and
the checks actually run. Include Windows version, scaling, monitor layout and
audio configuration when they affect a bug. Mark unavailable checks as pending.

Before changing a window's layout, painting or lifetime, read
[the window implementation guide](docs/ui/window-implementation-guide.md) and
[the native-frame decision](docs/decisions/ADR-024-native-trim-frame.md). Preserve
the shared window paths and the application/preview-helper boundary.

Do not commit captures, recordings, build output or local settings. Approved
documentation images use exact paths in both `.gitignore` and
`scripts/repository-media-allowlist.txt`. Run the repository checks before a PR:

```powershell
python scripts/check_repository_hygiene.py
python scripts/test_repository_hygiene.py
```

For sensitive reports, follow [SECURITY.md](SECURITY.md).

## Packages and releases

After a Release build and its applicable validation:

```powershell
.\scripts\package-release.ps1 -BuildDir build -OutputDir dist/releases
```

This installs the matched application and preview helper, license and usage
instructions, verifies the executables' version and x64 architecture, and
creates a ZIP with SHA-256 checksums. It does not run tests or sign binaries.
Existing outputs are not overwritten; use another directory under `dist/`
when preparing a replacement candidate.

The Release workflow takes an existing `vMAJOR.MINOR.PATCH` tag, runs the hosted
Build workflow for that tag, and creates a draft release from the tested
Release package. The tag must match the CMake project version and have notes
at `docs/releases/<tag>.md`. Review local desktop results, known limitations,
release notes and package contents before publishing the draft.

