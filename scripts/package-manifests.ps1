<#
.SYNOPSIS
    Writes winget and Scoop manifests for packages that package-release.ps1
    has already produced.
.DESCRIPTION
    Reads the version from CMakeLists.txt and the SHA-256 files beside the
    packages, and writes
        <OutputDir>/winget/<identifier>.yaml (three files)
        <OutputDir>/scoop/easnap.json
    The download addresses point at the GitHub release of that version, which
    must exist before either manifest is submitted. Nothing is uploaded or
    submitted by this script; docs/distribution.md describes the steps.
.PARAMETER PackageDir
    Directory with Easnap-v<version>-windows-x64.zip, ...-setup.exe and their
    .sha256 files.
.PARAMETER OutputDir
    Where the manifests are written. Must be inside the repository's dist
    directory. Existing files are not overwritten.
.EXAMPLE
    .\scripts\package-manifests.ps1 -PackageDir dist/releases -OutputDir dist/manifests
#>
[CmdletBinding()]
param(
    [string]$PackageDir = 'dist/releases',
    [string]$OutputDir = 'dist/manifests'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')

function Get-DistPath([string]$Path) {
    $full = if ([IO.Path]::IsPathRooted($Path)) { [IO.Path]::GetFullPath($Path) }
            else { [IO.Path]::GetFullPath((Join-Path $repoRoot $Path)) }
    $full = $full.TrimEnd('\', '/')
    if (-not $full.StartsWith((Join-Path $repoRoot 'dist') + [IO.Path]::DirectorySeparatorChar,
                              [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Package and manifest directories must be inside the repository dist directory.'
    }
    return $full
}

function Read-Checksum([string]$File) {
    $name = Split-Path -Leaf $File
    $line = (Get-Content -LiteralPath "$File.sha256" -Raw).Trim()
    if ($line -notmatch '^([a-f0-9]{64})  (.+)$' -or $matches[2] -ne $name) {
        throw "Invalid checksum file for $name."
    }
    $hash = $matches[1]
    if ((Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) {
        throw "$name does not match its checksum file."
    }
    return $hash
}

function Write-NewFile([string]$Path, [string]$Text) {
    if (Test-Path -LiteralPath $Path) { throw "Manifest already exists; choose another OutputDir: $Path" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
    Write-Host "Wrote $Path"
}

$projectText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($projectText -notmatch 'project\(Easnap\s+VERSION\s+(\d+\.\d+\.\d+)') { throw 'Cannot read the Easnap project version.' }
$version = $matches[1]
if ($projectText -notmatch '(?m)^set\(EASNAP_VERSION_PRERELEASE\s+"([0-9A-Za-z.]*)"\)') { throw 'Cannot read the prerelease label.' }
$prerelease = $matches[1]
$versionText = if ($prerelease) { "$version-$prerelease" } else { $version }

$packages = Get-DistPath $PackageDir
$output = Get-DistPath $OutputDir
$base = "Easnap-v$versionText-windows-x64"
$setup = Join-Path $packages "$base-setup.exe"
$zip = Join-Path $packages "$base.zip"
foreach ($file in @($setup, $zip)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing package: $file. Run package-release.ps1 first." }
}
$setupHash = (Read-Checksum $setup).ToUpperInvariant()
$zipHash = Read-Checksum $zip
$download = "https://github.com/vseprr/easnap/releases/download/v$versionText"

# A prerelease has its own winget identifier, so that `winget upgrade` never
# moves a stable installation to a beta.
$identifier = if ($prerelease) { 'vseprr.Easnap.Beta' } else { 'vseprr.Easnap' }
$name = if ($prerelease) { 'Easnap (Beta)' } else { 'Easnap' }
$schema = '1.6.0'
$winget = Join-Path $output 'winget'

Write-NewFile (Join-Path $winget "$identifier.yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.${schema}.schema.json
PackageIdentifier: $identifier
PackageVersion: $versionText
DefaultLocale: en-US
ManifestType: version
ManifestVersion: $schema
"@

Write-NewFile (Join-Path $winget "$identifier.installer.yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.${schema}.schema.json
PackageIdentifier: $identifier
PackageVersion: $versionText
MinimumOSVersion: 10.0.19041.0
InstallerType: inno
Scope: user
InstallModes:
  - interactive
  - silent
  - silentWithProgress
UpgradeBehavior: install
ProductCode: '{6F2B7C4E-0D53-4E0B-9C2A-7A7E5B1D4F31}_is1'
Installers:
  - Architecture: x64
    InstallerUrl: $download/$base-setup.exe
    InstallerSha256: $setupHash
ManifestType: installer
ManifestVersion: $schema
"@

Write-NewFile (Join-Path $winget "$identifier.locale.en-US.yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.${schema}.schema.json
PackageIdentifier: $identifier
PackageVersion: $versionText
PackageLocale: en-US
Publisher: Easnap contributors
PublisherUrl: https://github.com/vseprr/easnap
PublisherSupportUrl: https://github.com/vseprr/easnap/issues
PackageName: $name
PackageUrl: https://github.com/vseprr/easnap
License: MIT
LicenseUrl: https://github.com/vseprr/easnap/blob/v$versionText/LICENSE
ShortDescription: Screenshots with annotations, scrolling screenshots, MP4 recording and GIF export for Windows.
Description: |-
  Easnap is a local-first screen capture tool. Select a region, then take a
  screenshot and annotate it, capture a scrolling page as one tall image, or
  record an MP4 or GIF and trim it. It makes no network connection and
  collects nothing. The binaries are unsigned.
Tags:
  - screenshot
  - screen-capture
  - screen-recorder
  - scrolling-screenshot
  - gif
  - annotation
ReleaseNotesUrl: https://github.com/vseprr/easnap/releases/tag/v$versionText
ManifestType: defaultLocale
ManifestVersion: $schema
"@

# Scoop installs the portable ZIP. Its autoupdate block lets the bucket's
# maintainers follow later releases without this script.
$scoop = [ordered]@{
    version      = $versionText
    description  = 'Screenshots with annotations, scrolling screenshots, MP4 recording and GIF export.'
    homepage     = 'https://github.com/vseprr/easnap'
    license      = 'MIT'
    notes        = 'Easnap runs in the notification area. Settings, log and temporary recordings are kept in %LOCALAPPDATA%\Easnap. The binaries are unsigned.'
    architecture = [ordered]@{
        '64bit' = [ordered]@{
            url         = "$download/$base.zip"
            hash        = $zipHash
            extract_dir = $base
        }
    }
    shortcuts    = @(, @('bin\Easnap.exe', 'Easnap'))
    checkver     = [ordered]@{ github = 'https://github.com/vseprr/easnap' }
    autoupdate   = [ordered]@{
        architecture = [ordered]@{
            '64bit' = [ordered]@{
                url         = 'https://github.com/vseprr/easnap/releases/download/v$version/Easnap-v$version-windows-x64.zip'
                extract_dir = 'Easnap-v$version-windows-x64'
                hash        = [ordered]@{ url = '$url.sha256' }
            }
        }
    }
}
Write-NewFile (Join-Path $output 'scoop/easnap.json') (($scoop | ConvertTo-Json -Depth 8) + "`n")

Write-Host 'Manifests written. Nothing was submitted; see docs/distribution.md.'
