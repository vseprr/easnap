# Distribution

How an Easnap version becomes something people can install. Building and
testing are in [development](development.md) and [testing](testing.md).
Nothing described here happens automatically: no script in this repository
pushes, tags, publishes a release or submits a package manifest.

## Version

The version lives in the root `CMakeLists.txt`:

```cmake
project(Easnap VERSION 1.1.0 ...)
set(EASNAP_VERSION_PRERELEASE "beta.1")   # "" for a stable release
```

Together they give the version users see (`1.1.0-beta.1`): About, the product
version of both executables, the log, the package names and the release tag
`v1.1.0-beta.1`. The file version of the executables stays numeric
(`1.1.0.0`), and a prerelease sets the prerelease flag in the version
resource.

## Packages

After a Release build that has passed the tests that apply:

```powershell
.\scripts\package-release.ps1 -BuildDir build -OutputDir dist/releases
```

produces, without overwriting anything:

| File | Contents |
|---|---|
| `Easnap-v<version>-windows-x64.zip` | Portable copy with `bin\Easnap.exe`, `bin\EasnapPreviewHost.exe`, the license and a readme |
| `Easnap-v<version>-windows-x64-setup.exe` | Per-user installer (Inno Setup 6), English and Turkish |
| `Easnap-v<version>-windows-x64-symbols.zip` | `Easnap.pdb` and `EasnapPreviewHost.pdb` of exactly these executables |
| `*.sha256` | One checksum file per package |

The script checks that both executables carry this version, are x64 Release
builds, are identical to the build output and name their symbol file without a
directory. The binaries and the installer are unsigned.

The symbol files are not for users. A crash report
(`%LOCALAPPDATA%\Easnap\crashes\*.dmp`) is a minidump, and a debugger can read
it only with the symbol files of the build that wrote it. Keep the symbols ZIP
of every published version; the Release workflow attaches it to the release.
To read a report, open the `.dmp` in WinDbg or Visual Studio and add the
folder with the extracted `.pdb` files to the symbol path.

The installer needs Windows 10 version 2004 (build 19041), installs to
`%LOCALAPPDATA%\Programs\Easnap` without administrator rights, and refuses to
replace files while Easnap runs. Uninstalling removes the sign-in entry that
"Start Easnap when I sign in to Windows" wrote, and asks whether settings, the
log and crash reports under `%LOCALAPPDATA%\Easnap` should be removed as well;
a silent uninstall keeps them. A recording that was not saved is never removed
by the uninstaller.

A prerelease is not a second channel. It has the same installer identity, the
same single-instance rule and the same data folder as a stable version:
installing a beta replaces an installed stable version and uses its settings,
and installing the stable version afterwards replaces the beta.

## Release

1. Write `docs/releases/v<version>.md`.
2. Tag the commit `v<version>` and push the tag.
3. Run the **Release** workflow with that tag. It checks that the tag matches
   the version in `CMakeLists.txt`, builds and tests on a hosted runner,
   packages, attests the packages and creates a **draft** release. A tag with
   a prerelease label creates a draft that is marked as a prerelease, so
   GitHub does not offer it as the latest release.
4. Review the draft, the local desktop test results and the notes, then
   publish it by hand.

## Package managers

The manifests point at the files of a published GitHub release, so they can
only be submitted after step 4.

```powershell
.\scripts\package-manifests.ps1 -PackageDir dist/releases -OutputDir dist/manifests
```

writes `dist/manifests/winget/*.yaml` and `dist/manifests/scoop/easnap.json`
from the packages and their checksum files.

### winget

- A stable release uses the identifier `vseprr.Easnap`. A prerelease uses
  `vseprr.Easnap.Beta`, so that `winget upgrade` never moves a stable
  installation to a beta.
- Check the files: `winget validate --manifest dist\manifests\winget` and
  `winget install --manifest dist\manifests\winget` on a test machine. Neither
  has been run so far: the manifests were generated and read, not validated
  by winget.
- Both identifiers describe the same installed program (see above). Do not
  keep a beta and a stable version of the same release in winget at once.
- Submit them as a pull request to
  [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) under
  `manifests/v/vseprr/Easnap/<version>/` (or `.../Easnap/Beta/<version>/`).
  Its validation installs the package on Microsoft's machines; an unsigned
  installer can be held for manual review there.

### Scoop

- Scoop installs the portable ZIP and creates a Start menu shortcut.
- A prerelease does not belong in the main or extras bucket. Either keep the
  manifest in a bucket of your own, or submit it to
  [ScoopInstaller/Versions](https://github.com/ScoopInstaller/Versions) as
  `easnap-beta.json`. A stable release goes to
  [ScoopInstaller/Extras](https://github.com/ScoopInstaller/Extras) as
  `easnap.json`.
- Check it first: `scoop install .\dist\manifests\scoop\easnap.json`.

Neither submission has been made for any version so far.
