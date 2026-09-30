<p align="center">
  <img src="apps/easnap-desktop/resources/easnap-logo.png" alt="Easnap" width="360">
</p>

<p align="center">Screenshots, annotations and screen recordings for Windows.</p>

<p align="center">
  <a href="https://github.com/vseprr/easnap/releases/latest">Download</a> ·
  <a href="https://github.com/vseprr/easnap/releases/tag/v1.0.0">Release notes</a> ·
  <a href="#planned">Planned features</a>
</p>

Easnap lives in the system tray. Select part of your screen, add a few annotations,
and copy the result. Or record a short clip, trim it, and save it as MP4 or GIF.
Capture, editing and export run locally, with no account, telemetry or network
connection required.

![Easnap screenshot editor with its floating annotation toolbar](docs/images/editor.png)

## Capture and annotate

Select a monitor or draw a custom region. Adjust the frame before taking the
screenshot, then work at the image's original resolution.

- Draw with pen, lines, arrows, rectangles and ellipses.
- Add text and tags, mark an area with Spotlight, or apply a repeated watermark.
- Use Blur or Pixelate with a brush or an editable region.
- Move and edit annotations, undo changes, and copy or save as PNG/JPEG.
- Read a pixel's RGB or HEX value with the selection magnifier.

## Record, trim and export

Choose MP4 or GIF before recording. Include the cursor when useful, pause while
you prepare the next step, and stop when the clip is ready. Paused time is left
out of the recording.

| Option | Available in v1 |
|---|---|
| MP4 | H.264 video with optional AAC audio |
| Audio | System audio, microphone, both, or neither |
| GIF | Silent animation with High quality and Low size presets |
| Trim | Preview playback, seek, and set In/Out points before export |
| Finish | Save to a chosen location or copy the recording file to the clipboard |

![Easnap recording configuration with format, audio and cursor options](docs/images/recording.png)

*Recording options stay available before you press Start.*

![Easnap recording preview with a timeline and trim controls](docs/images/trim.png)

*Preview the recording and adjust its start and end before saving.*

## Download and run

The portable build targets **Windows 10 and Windows 11, x64**. No installer is
required.

1. Download **[Easnap-v1.0.0-windows-x64.zip](https://github.com/vseprr/easnap/releases/download/v1.0.0/Easnap-v1.0.0-windows-x64.zip)** from the release.
2. Extract the entire ZIP to a folder you can write to.
3. Open the `bin` folder and run `Easnap.exe`. Keep its matching
   `EasnapPreviewHost.exe` beside it; the preview helper is part of the application.
4. Press **Ctrl+Shift+A**, or choose **Capture** from the tray menu.

The build is unsigned, so Windows may display a publisher or reputation warning.
Download `Easnap-v1.0.0-windows-x64.zip.sha256` from the same release to check the ZIP:

```powershell
Get-FileHash .\Easnap-v1.0.0-windows-x64.zip -Algorithm SHA256
```

Compare the result with the value in the `.zip.sha256` file. The extracted ZIP also contains `SHA256SUMS.txt`, which lists checksums for the packaged files.

To update, exit Easnap and extract the new release into a separate folder. Always
use the application and preview helper from the same release. To remove the
portable application, exit it and delete its extracted folder. User settings and
temporary data are stored separately; **Open data folder** in the tray shows
their location.

## First capture

1. Press **Ctrl+Shift+A**, then click a monitor or drag a region.
2. Adjust the frame. Choose an annotation tool to capture the image and begin
   editing, or choose **Record** before starting a screenshot edit.
3. For a screenshot, use **Ctrl+C** to copy or **Ctrl+S** to save. For a
   recording, press **Start**, then **Stop** to open the preview and trim window.

| Shortcut | Action |
|---|---|
| Ctrl+Shift+A | Open or return to the capture workspace |
| Ctrl+C / Ctrl+S | Copy / save the screenshot |
| Ctrl+Z / Ctrl+Y | Undo / redo an annotation edit |
| Esc | Cancel capture; stop and finalize an active recording |

Text editing and active gestures handle these keys in their own context. See
the [full input reference](docs/user-flows.md#keyboard-reference) for details.
Light and Dark appearance are available from the tray and capture controls.

## Known limitations

- Transient redraw artifacts can occur while resizing or maximizing the Trim
  window.
- The recorded physical display baseline is **125% scaling**. The wider
  Windows, GPU, audio-device, accessibility and mixed-DPI compatibility matrix
  is not complete.
- GIF preview plays the source recording. Its final palette and frame cadence
  are applied during export; GIF has no audio.
- MP4 cut points follow video/audio sample granularity. A trim near the end of a
  long recording can take longer because decoding starts at the source beginning.

See [release notes](https://github.com/vseprr/easnap/releases/tag/v1.0.0) and
[project status](docs/project-status.md) for validation results and remaining work.

## Planned

| Feature | Direction |
|---|---|
| **Scrollshot** | Capture a scrolling region and stitch it into a single image; planned for v1.1. |
| **OCR** | Extract text from screenshots; planned after v1, with no release date set. |
| Compatibility and accessibility | Extend testing across Windows versions, displays, audio devices and keyboard workflows. |

Scrollshot and OCR are **not included in v1**. The
[roadmap](EASNAP_ROADMAP.md) records the current priorities and release boundaries.

## Development

Easnap is written in **C++20** using the Windows SDK, with no third-party runtime
dependencies. Build with Visual Studio 2022, the Desktop development with C++
workload, a Windows SDK and CMake 3.20 or later:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DEASNAP_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure --parallel 4
```

Native media and desktop tests require an interactive Windows session; leave
the pointer and desktop idle while they run. See the
[development guide](docs/development.md) for incremental testing and packaging,
[architecture](docs/architecture.md) for the code structure, and
[project status](docs/project-status.md) before starting changes.

## Privacy and license

Settings and private temporary recordings stay in the current user's local
application-data directory. Copied recording files remain available while the
clipboard references them, including after the application closes. Saving
creates a file at the destination you choose.

For security details and reporting, see [SECURITY.md](SECURITY.md).
Easnap is available under the [MIT License](LICENSE).

