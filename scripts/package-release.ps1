<#
.SYNOPSIS
    Packages an existing Release x64 build as a portable ZIP, a per-user
    installer and SHA-256 files.
.DESCRIPTION
    Uses CMake install, verifies the application and matching preview helper,
    and includes the license and portable instructions. The installer is built
    from the same verified package directory with Inno Setup 6. Does not
    build, test, sign or publish. Existing package outputs are never overwritten.
.PARAMETER Installer
    Auto (default) builds the installer when Inno Setup's ISCC.exe is found and
    says so when it is not. Require fails without it. Skip builds the ZIP only.
.EXAMPLE
    .\scripts\package-release.ps1 -BuildDir build -OutputDir dist/releases
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$OutputDir = 'dist/releases',
    [ValidateSet('Auto', 'Require', 'Skip')][string]$Installer = 'Auto'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')

function Get-RepositoryPath {
    param([string]$Path, [switch]$Output)
    $fullPath = if ([IO.Path]::IsPathRooted($Path)) { [IO.Path]::GetFullPath($Path) }
                else { [IO.Path]::GetFullPath((Join-Path $repoRoot $Path)) }
    $fullPath = $fullPath.TrimEnd('\', '/')
    if (-not $fullPath.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar,
                                  [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build and package output directories must be inside this repository.'
    }
    $parts = $fullPath.Substring($repoRoot.Length + 1) -split '[\\/]'
    if ($Output -and $parts[0] -ne 'dist') {
        throw 'Package output must be inside the repository dist directory.'
    }
    $current = $repoRoot
    foreach ($part in @('') + $parts) {
        if ($part) { $current = Join-Path $current $part }
        if (Test-Path -LiteralPath $current) {
            $entry = Get-Item -LiteralPath $current -Force
            if (-not $entry.PSIsContainer -or
                ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Build and package paths must contain only ordinary directories.'
            }
        }
    }
    return $fullPath
}

function Assert-ReleaseExecutable {
    param([string]$Path, [string]$ExpectedVersion, [string]$ExpectedName)
    $file = Get-Item -LiteralPath $Path
    if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Expected an ordinary executable: $Path"
    }
    $info = $file.VersionInfo
    if ($info.FileVersion -ne "$ExpectedVersion.0" -or
        $info.ProductVersion -ne "$ExpectedVersion.0" -or
        $info.OriginalFilename -ne $ExpectedName -or $info.IsDebug) {
        throw "Executable metadata does not match Release $ExpectedVersion : $Path"
    }
    $stream = [IO.File]::OpenRead($Path)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a Windows executable: $Path" }
        $stream.Position = 0x3C
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 6) { throw "Invalid PE header: $Path" }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "Expected an x64 Windows executable: $Path"
        }
    }
    finally { $reader.Dispose() }
}

$build = Get-RepositoryPath $BuildDir
$output = Get-RepositoryPath $OutputDir -Output
$cache = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')) {
    if ($line -match '^(CMAKE_HOME_DIRECTORY|CMAKE_CACHEFILE_DIR|CMAKE_PROJECT_NAME|CMAKE_PROJECT_VERSION):[^=]+=(.*)$') {
        $cache[$matches[1]] = $matches[2]
    }
}
$projectText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($projectText -notmatch 'project\(Easnap\s+VERSION\s+(\d+\.\d+\.\d+)') {
    throw 'Cannot read the Easnap project version.'
}
$version = $matches[1]
if ($cache['CMAKE_PROJECT_NAME'] -ne 'Easnap' -or $cache['CMAKE_PROJECT_VERSION'] -ne $version -or
    -not $cache['CMAKE_HOME_DIRECTORY'] -or -not $cache['CMAKE_CACHEFILE_DIR'] -or
    [IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY']).TrimEnd('\', '/') -ne $repoRoot -or
    [IO.Path]::GetFullPath($cache['CMAKE_CACHEFILE_DIR']).TrimEnd('\', '/') -ne $build) {
    throw 'Configure and build this Easnap source version before packaging.'
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'CMake was not found on PATH.' }

$executables = @('Easnap.exe', 'EasnapPreviewHost.exe')
$binaryDir = Join-Path $build 'bin/Release'
foreach ($name in $executables) {
    Assert-ReleaseExecutable (Join-Path $binaryDir $name) $version $name
}
$packageName = "Easnap-v$version-windows-x64"
$package = Join-Path $output $packageName
$archive = Join-Path $output "$packageName.zip"
$checksum = "$archive.sha256"
foreach ($path in @($package, $archive, $checksum)) {
    if (Test-Path -LiteralPath $path) { throw "Package output already exists; choose another OutputDir: $path" }
}
New-Item -ItemType Directory -Path $package -Force | Out-Null
& cmake --install $build --config Release --prefix $package
if ($LASTEXITCODE -ne 0) { throw "CMake install failed with exit code $LASTEXITCODE." }

$expected = @('bin/Easnap.exe', 'bin/EasnapPreviewHost.exe', 'LICENSE', 'README.txt')
$installed = @(Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
    $_.FullName.Substring($package.Length + 1).Replace('\', '/')
})
if (Compare-Object ($expected | Sort-Object) ($installed | Sort-Object)) {
    throw 'CMake installed an unexpected package inventory.'
}
foreach ($name in $executables) {
    $source = Join-Path $binaryDir $name
    $destination = Join-Path $package "bin/$name"
    Assert-ReleaseExecutable $destination $version $name
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash) {
        throw "Installed executable differs from the build: $name"
    }
}
$fileChecksums = foreach ($relative in $expected) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $package $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $relative"
}
[IO.File]::WriteAllLines((Join-Path $package 'SHA256SUMS.txt'), [string[]]$fileChecksums,
                         (New-Object Text.UTF8Encoding($false)))

Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($package, $archive,
    [IO.Compression.CompressionLevel]::Optimal, $true)
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $archiveFiles = @($zip.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('\', '/') })
    $expectedFiles = @($expected + 'SHA256SUMS.txt' | ForEach-Object { "$packageName/$_" })
    if (Compare-Object ($expectedFiles | Sort-Object) ($archiveFiles | Sort-Object)) {
        throw 'The ZIP contents do not match the installed package.'
    }
}
finally { $zip.Dispose() }
$archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($checksum, "$archiveHash  $packageName.zip" + [char]10, (New-Object Text.UTF8Encoding($false)))
Write-Host "Portable package: $archive"
Write-Host "SHA-256 checksum: $checksum"

if ($Installer -ne 'Skip') {
    $compiler = @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue | ForEach-Object Source),
        (Join-Path $repoRoot '.local/tools/innosetup/ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6/ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 6/ISCC.exe')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
    if (-not $compiler) {
        if ($Installer -eq 'Require') { throw 'Inno Setup 6 (ISCC.exe) was not found; the installer cannot be built.' }
        Write-Warning 'Inno Setup 6 (ISCC.exe) was not found. The installer was NOT built; only the ZIP exists.'
    } else {
        $setup = Join-Path $output "$packageName-setup.exe"
        $setupChecksum = "$setup.sha256"
        foreach ($path in @($setup, $setupChecksum)) {
            if (Test-Path -LiteralPath $path) { throw "Package output already exists; choose another OutputDir: $path" }
        }
        & $compiler /Qp "/DAppVersion=$version" "/DSourceDir=$package" "/DOutputDir=$output" (Join-Path $repoRoot 'packaging/easnap.iss')
        if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE." }
        if (-not (Test-Path -LiteralPath $setup -PathType Leaf)) { throw "Inno Setup reported success but $setup is missing." }
        $setupHash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
        [IO.File]::WriteAllText($setupChecksum, "$setupHash  $packageName-setup.exe" + [char]10, (New-Object Text.UTF8Encoding($false)))
        Write-Host "Installer: $setup"
        Write-Host "SHA-256 checksum: $setupChecksum"
    }
}
Write-Host 'Packaging completed. This command does not run tests, sign binaries or publish a release.'

