# Development

Read [project status](project-status.md) and the [roadmap](../EASNAP_ROADMAP.md)
before starting a development slice. The [documentation index](README.md)
links to implementation contracts, decisions and validation records.

## Build and run

Requirements:

- Windows 10 or later, with native display, capture, audio and codec support.
- Visual Studio 2022 with **Desktop development with C++** and a Windows SDK.
- CMake 3.20 or later. There are no third-party runtime dependencies.

From the repository root:

```powershell
.\scripts\build.ps1 -Configuration Release
.\build\bin\Release\Easnap.exe
```

`build.ps1` configures, builds and then runs the tests affected by the change.
The development build runs directly from `build\bin`; no packaging or
installation step is involved in the edit-build-run loop. `-TestMode None`
builds without tests. `-TestMode Full` ignores the pass cache and, unless
`-Tier` says otherwise, raises the tier to Full, which runs the
visible-desktop tests; `test.ps1 -Mode Full` only ignores the cache. `-Tier`
and `-Monitor` are passed through to the test runner. Plain CMake works too:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DEASNAP_BUILD_TESTS=ON
cmake --build build --config Release
```

Exit a running Easnap before rebuilding: the linker cannot replace an
executable that is in use.

Add `-Clean` to clean the selected configuration's generated targets before
rebuilding. The helper verifies an existing in-repository Easnap CMake tree;
it preserves unrelated evidence and the cache. Reuse `build/` for Debug/Release.
Local backups and investigation working files belong under ignored `.local/`.

## Tests

The [testing guide](testing.md) is the reference for tiers, the pass cache,
display selection and isolation from the person using the computer. In short:

```powershell
.\scripts\test.ps1                              # Standard tier, changed tests only
.\scripts\test.ps1 -Tier Full -Monitor SAMSUNG  # adds the visible-desktop tests
.\scripts\test.ps1 -Tier Soak                   # everything, before a release
```

CTest covers production selection and pointer policy, native overlay/editor
rendering and clipboard behavior, capture fallback, recording workers, audio
mixing/resampling, MP4/GIF encoding and trim, preview recovery, publication
lifetime, cleanup containment and layout across display sizes and scale
factors. Capability skips and environment failures are not passed hardware
tests: a test that cannot run where it is exits with 77, and `test.ps1` lists
it as skipped, apart from passes and failures. CMake 3.20 runs uncached;
CTest 3.21+ supplies the result format used by the pass cache.

The tests of the trim window and their opt-in diagnostic modes are described
in the [window implementation guide](ui/window-implementation-guide.md);
current results and remaining visual failures are in
[project status](project-status.md).

The Windows CI workflow builds Debug and Release on the `windows-2022` image,
runs the tests a hosted runner can execute (see [testing](testing.md)) uncached
and packages the Release build. A second job builds the same tests with
AddressSanitizer; a third workflow runs CodeQL on the application sources.
Automated tests complement manual multi-monitor, DPI, audio-device and visual
interaction checks.

To run the tests under AddressSanitizer locally:

```powershell
cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DEASNAP_BUILD_TESTS=ON -DEASNAP_SANITIZE_ADDRESS=ON
cmake --build build-asan --config Release --parallel
# The sanitizer runtime is a DLL beside the compiler; put its folder on PATH:
$env:PATH = "<Visual Studio>\VC\Tools\MSVC\<version>\bin\Hostx64\x64;" + $env:PATH
ctest --test-dir build-asan -C Release --label-exclude "interactive-desktop|desktop-visual|soak|preview" --exclude-regex "^diagnostics_log_and_crash_reports$"
```

The crash-report test is excluded: the sanitizer takes its deliberate access
violations before the application's own handler. Sanitized binaries are never
packaged.

## Packaging

```powershell
.\scripts\package-release.ps1 -BuildDir build -OutputDir dist/releases
```

This produces the portable ZIP, a ZIP with the symbol files of both
executables and, when Inno Setup 6 is available, the per-user installer
(`packaging/easnap.iss`), each with a SHA-256 file. The installer needs no
administrator rights and installs to `%LOCALAPPDATA%\Programs\Easnap`. The
supervised preview helper is part of the application: `Easnap.exe` and
`EasnapPreviewHost.exe` must always come from the same build.
[Distribution](distribution.md) describes the version text, the release steps,
reading a crash report with the symbol files, and the winget and Scoop
manifests that `scripts/package-manifests.ps1` writes.

## Source layout

| Path | Purpose |
|---|---|
| `apps/easnap-desktop/src/app` | Session coordination, settings and recording/trim workflows |
| `apps/easnap-desktop/src/ui` | Capture workspace, native controls and trim window |
| `apps/easnap-desktop/src/editor` | Screenshot tools, annotation history, rendering and export |
| `apps/easnap-desktop/src/capture` | Desktop capture providers and worker |
| `apps/easnap-desktop/src/recording` | Recording session and active-time audio mapping |
| `apps/easnap-desktop/src/media` | Audio, codecs, trim/export and supervised preview |
| `apps/easnap-desktop/src/platform` | Shared Windows utilities and resource ownership |
| `apps/easnap-desktop/tests` | Unit, integration and native regression tests |
| `docs` | Architecture, flows, decisions and validation evidence |
| `packaging` | Installer definition |
| `scripts`, `.github/workflows` | Local tooling and CI |

Before adding or changing any window, follow the
[window implementation guide](ui/window-implementation-guide.md). It records the
shared frame, layout, painting and validation requirements.
