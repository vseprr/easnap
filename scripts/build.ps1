<#
.SYNOPSIS
    Configures and builds Easnap.

.DESCRIPTION
    Configures and builds with CMake, then runs affected or full CTest checks.
    CI uses the same CMake build; physical desktop checks run locally.

.PARAMETER Configuration
    Debug or Release. Defaults to Release.

.PARAMETER Generator
    CMake generator. Defaults to "Visual Studio 17 2022".

.PARAMETER BuildDir
    Build directory. Defaults to "build" in the repository root.

.PARAMETER WithTests
    Compatibility shortcut for -TestMode Full (includes desktop checks).

.PARAMETER TestMode
    Affected (default) builds tests and runs changed/uncached checks. Full always
    runs all checks. None explicitly builds without tests.

.PARAMETER DesktopTests
    Run or Defer physical input/capture checks. Defaults to Defer for Affected,
    Run for Full. Deferred checks are printed as pending and never cached.

.PARAMETER Clean
    Clean generated targets for Configuration using CMake before configuring.
    Requires an existing Easnap build inside this repository. Keeps the CMake
    cache and unrelated files, including validation evidence; never deletes a tree.

.EXAMPLE
    .\scripts\build.ps1
.EXAMPLE
    .\scripts\build.ps1 -Configuration Debug -WithTests
#>

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$Generator = 'Visual Studio 17 2022',

    [string]$BuildDir = 'build',

    [switch]$WithTests,

    [ValidateSet('Affected', 'Full', 'None')]
    [string]$TestMode = 'Affected',

    [ValidateSet('Run', 'Defer')]
    [string]$DesktopTests,

    [ValidateRange(1,16)][int]$TestJobs = 4,

    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if ($WithTests -and -not $PSBoundParameters.ContainsKey('TestMode')) { $TestMode = 'Full' }
if (-not $DesktopTests) { $DesktopTests = if ($TestMode -eq 'Full') { 'Run' } else { 'Defer' } }
. (Join-Path $PSScriptRoot 'build-safety.ps1')
Push-Location $repoRoot

try {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw 'cmake was not found on PATH. Install CMake 3.20 or newer, or run this from a Developer PowerShell.'
    }

    if ($Clean) {
        $cleanDirectory = Get-EasnapBuildDirectory -RepositoryRoot $repoRoot -BuildDirectory $BuildDir
        Write-Host "Cleaning generated $Configuration targets in $cleanDirectory" -ForegroundColor Yellow
        & cmake --build $cleanDirectory --config $Configuration --target clean
        if ($LASTEXITCODE -ne 0) { throw "CMake clean failed with exit code $LASTEXITCODE." }
    }

    Write-Host "Configuring Easnap ($Configuration, $Generator)" -ForegroundColor Cyan
    $configureArgs = @(
        '-S', '.',
        '-B', $BuildDir,
        '-G', $Generator,
        '-A', 'x64',
        '-DEASNAP_WARNINGS_AS_ERRORS=ON',
        "-DEASNAP_BUILD_TESTS=$(if ($TestMode -ne 'None') { 'ON' } else { 'OFF' })"
    )
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed with exit code $LASTEXITCODE." }

    Write-Host "Building $Configuration" -ForegroundColor Cyan
    & cmake --build $BuildDir --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

    if ($TestMode -ne 'None') {
        Write-Host 'Running tests' -ForegroundColor Cyan
        & (Join-Path $PSScriptRoot 'test.ps1') -BuildDir $BuildDir -Configuration $Configuration `
            -Mode $TestMode -DesktopTests $DesktopTests -Jobs $TestJobs
    }

    $outputRoot = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repoRoot $BuildDir }
    $exe = Join-Path $outputRoot "bin/$Configuration/Easnap.exe"
    if (Test-Path $exe) {
        $sizeMb = [math]::Round((Get-Item $exe).Length / 1MB, 2)
        Write-Host ''
        Write-Host "Build succeeded: $exe ($sizeMb MB)" -ForegroundColor Green
    }
    else {
        throw "Build reported success but $exe is missing."
    }
}
finally {
    Pop-Location
}
