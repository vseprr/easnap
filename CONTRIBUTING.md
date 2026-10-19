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

The default run executes only the tests your change affects and reports tests
that need the visible desktop as pending. Before a pull request that touches
windows, capture or recording, run them too and say which display you used:

```powershell
.\scripts\test.ps1 -Tier Full
```

The [testing guide](docs/testing.md) explains the tiers, the pass cache, how
tests are kept away from your pointer and keyboard, and how to write a test
that does not need the visible desktop. Where a Hyper-V test lab is set up,
the Standard and Full tiers run there instead of on your desktop; see
[the lab](docs/testing.md#lab). Hosted CI runs the fast tier and the
integration tests a hosted runner can execute; it excludes the desktop and soak
tiers and all preview tests. A green hosted check does not validate preview
playback, a physical display, pointer or audio device.

## Changes

Keep fixes focused. Explain the triggering behavior, the resulting change and
the checks actually run. Include Windows version, scaling, monitor layout and
audio configuration when they affect a bug. Mark unavailable checks as pending.

Text the user reads goes into both string tables,
`apps/easnap-desktop/resources/strings_en.rc` and `strings_tr.rc`, never into
the C++ sources; see
[ADR-032](docs/decisions/ADR-032-interface-text-in-string-tables.md). If you
do not write Turkish, say so in the pull request and leave the English text in
the Turkish table: it will be translated before a release.

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

The first also reports relative links between documents that lead nowhere: to
a file that is not tracked, or to a heading that does not exist.

For sensitive reports, follow [SECURITY.md](SECURITY.md).

## Packages and releases

After a Release build and its applicable validation:

```powershell
.\scripts\package-release.ps1 -BuildDir build -OutputDir dist/releases
```

This installs the matched application and preview helper, license and usage
instructions, verifies the executables' version and x64 architecture, and
creates a portable ZIP, a ZIP with the symbol files and, when Inno Setup 6 is
installed, a per-user installer from the same verified files, each with a
SHA-256 file. `-Installer Require` fails instead of skipping the installer. The
script does not run tests or sign binaries. Existing outputs are not
overwritten; use another directory under `dist/` when preparing a replacement
candidate.

The Release workflow takes an existing tag, `vMAJOR.MINOR.PATCH` or with a
prerelease label such as `v1.1.0-beta.1`, runs the hosted Build workflow for
that tag, attests the packages and creates a draft release from the tested
ZIP, installer and symbol files. A tag with a label becomes a prerelease. The
tag must match the version in the root `CMakeLists.txt` and have notes at
`docs/releases/<tag>.md`. Review local desktop results, known limitations,
release notes and package contents before publishing the draft.
[Distribution](docs/distribution.md) has the complete steps.

