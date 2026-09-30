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
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DEASNAP_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure --parallel 4
.\build\bin\Release\Easnap.exe
```

The helper builds first and runs changed or uncached tests:

```powershell
.\scripts\build.ps1 -Configuration Release
```

Tests that need the physical pointer, focus or desktop capture are reported as
**pending**, not passed, while you use the computer. To run every test, leave the
desktop idle and use `-TestMode Full` (`-WithTests` is a compatibility alias).
`-DesktopTests Run` includes pending interaction tests in an affected run.
`-TestMode None` explicitly builds without tests.

Add `-Clean` to clean the selected configuration's generated targets before
rebuilding. The helper verifies an existing in-repository Easnap CMake tree;
it preserves unrelated evidence and the cache. Reuse `build/` for Debug/Release.
Local backups and investigation working files belong under ignored `.local/`.

## Tests

CTest covers production selection and pointer policy, native overlay/editor
rendering and clipboard behavior, capture fallback, recording workers, audio
mixing/resampling, MP4/GIF encoding and trim, preview recovery, and publication
lifetime. Native media tests can take several minutes and require a suitable
Windows desktop and media environment. Capability skips and environment failures
are not passed hardware tests.

The default affected mode reuses a successful result for at most 24 hours when
the compiled test, runtime helpers, CTest command/properties, build configuration
and environment are unchanged. Dependencies come from CMake's target graph.
Shared headers/resources/tooling and production files outside that graph
conservatively invalidate all results. Failed, missing, interrupted or skipped
results are never reused as passes. This local iteration cache does not replace
the full suite. CMake 3.20 runs uncached; CTest 3.21+ supplies the result format
used by the cache.

After building, inspect or run pending checks:

```powershell
.\scripts\test.ps1 -Configuration Release -ListOnly
.\scripts\test.ps1 -Configuration Release -DesktopTests Run
.\scripts\test.ps1 -Configuration Release -Mode Full
```

Direct `test.ps1` and CTest commands test the already-built binaries; use
`build.ps1` after source edits. Independent checks use four CTest slots by default
(`-TestJobs` in the build helper). Physical input/capture checks remain serial.
Display reconfiguration can invalidate capture or input in progress; an
environment-blocked desktop check is not a pass.

The `mfplay_preview_continuous_seek_*` cases keep Play requested during repeated
random seeking. `trim_timeline_seek_stress` uses real pointer input and desktop
pixels with 6/10 s videos in both themes. It requires an idle pointer and selects
the DXGI observer through CTest. The preview pixel test uses a nonactivating,
topmost tool window and actual desktop capture.

`trim_endpoint_presentation` observes the complete production trim window during
playback end and replay in both themes, after maximize/restore preparation. The
opt-in `--resize-edges` gate observes resize/maximize motion; an endpoint pass
does not substitute for it. To collect endpoint frames with generated media:

```powershell
.\build\bin\Release\test_ui_modernization.exe --trim-endpoint .local\trim-endpoint
```

Its pixel assertions target the generated fixture and the original regression
clip, not arbitrary video content. The original clip is no longer in the checkout.
Current results and remaining visual failures are recorded in
[project status](project-status.md).

The Windows CI workflow builds Debug and Release, runs the uncached CTest suite
and packages both executables. Automated tests complement manual multi-monitor,
DPI, audio-device and visual interaction checks.

## Portable installation

```powershell
cmake --install build --config Release --prefix dist
```

The supervised preview helper is part of the application. Deploy
`Easnap.exe` and `EasnapPreviewHost.exe` together from the same build.

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
| `scripts`, `.github/workflows` | Local tooling and CI |

Before adding or changing any window, follow the
[window implementation guide](ui/window-implementation-guide.md). It records the
shared frame, layout, painting and validation requirements.
