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
    Affected (default) builds tests and runs changed/uncached checks. Full
    ignores the pass cache. Build compiles the tests but runs none (the lab
    builds this way and runs the tests in its own desktop session). None
    explicitly builds without tests.

.PARAMETER Tier
    Which tests are eligible: Fast, Standard, Full or Soak (docs/testing.md).
    Defaults to Standard for Affected and Full for TestMode Full.

.PARAMETER DesktopTests
    Run or Defer tests that need the visible desktop. Defaults to Run for the
    Full and Soak tiers. Deferred tests are printed as pending, never cached.

.PARAMETER Monitor
    Display for visible-desktop tests (number, name such as SAMSUNG, device,
    Primary or Ask). Ask prompts when several displays are active.

.PARAMETER Clean
    Clean generated targets for Configuration using CMake before configuring.
    Requires an existing Easnap build inside this repository. Keeps the CMake
    cache and unrelated files, including validation evidence; never deletes a tree.

.PARAMETER Trust
    Lab: a direct build. If the latest lab verdict (build/Testing/lab/verdict.json,
    written by scripts/lab.ps1 run) is green for exactly this working tree, the
    build runs no tests and says why it trusts the verdict. If the tree differs,
    it lists what changed and runs the tests as usual. None (default) ignores
    the lab.

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

    [ValidateSet('Affected', 'Full', 'Build', 'None')]
    [string]$TestMode = 'Affected',

    [ValidateSet('Fast', 'Standard', 'Full', 'Soak')]
    [string]$Tier,

    [ValidateSet('Run', 'Defer')]
    [string]$DesktopTests,

    [string]$Monitor = 'Ask',

    [ValidateRange(1,16)][int]$TestJobs = 4,

    [switch]$Clean,

    [ValidateSet('None', 'Lab')]
    [string]$Trust = 'None'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if ($WithTests -and -not $PSBoundParameters.ContainsKey('TestMode')) { $TestMode = 'Full' }
if (-not $Tier) {
    $Tier = if ($TestMode -eq 'Full' -or $DesktopTests -eq 'Run') { 'Full' } else { 'Standard' }
}
if (-not $DesktopTests) { $DesktopTests = if ($Tier -in @('Full', 'Soak')) { 'Run' } else { 'Defer' } }
. (Join-Path $PSScriptRoot 'build-safety.ps1')

# Direct build: ask the lab whether its latest green verdict covers exactly this
# tree. Tests are still built (the configuration keeps them on) but not run.
$runTests = $TestMode -notin @('None', 'Build')
if ($Trust -eq 'Lab' -and $runTests) {
    $labScript = Join-Path $PSScriptRoot 'lab.ps1'
    $preference = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        $verifyOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $labScript verify -Json 2>&1 | ForEach-Object { "$_" })
        $verifyCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $preference }
    $verify = $null
    try { $verify = (($verifyOutput | Where-Object { $_ -notmatch '^\[\d\d:\d\d:\d\d\]' }) -join "`n") | ConvertFrom-Json } catch { $verify = $null }
    if ($verifyCode -eq 0 -and $verify -and $verify.trusted) {
        Write-Host "Direct build: no tests will run. $($verify.reason)" -ForegroundColor Green
        $runTests = $false
    } else {
        $reason = if ($verify -and $verify.PSObject.Properties['reason']) { $verify.reason } else { ($verifyOutput -join ' ') }
        Write-Host "Direct build refused: $reason" -ForegroundColor Yellow
        if ($verify) {
            foreach ($kind in @('changed', 'added', 'removed')) {
                $property = $verify.PSObject.Properties[$kind]
                $list = if ($property) { @($property.Value) } else { @() }
                if ($list.Count) {
                    Write-Host "  ${kind}:" -ForegroundColor Yellow
                    $list | Select-Object -First 40 | ForEach-Object { Write-Host "    $_" }
                    if ($list.Count -gt 40) { Write-Host "    ... and $($list.Count - 40) more" }
                }
            }
        }
        Write-Host "Running the $Tier tier ($TestMode) as usual." -ForegroundColor Yellow
    }
}
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

    if ($runTests) {
        Write-Host 'Running tests' -ForegroundColor Cyan
        & (Join-Path $PSScriptRoot 'test.ps1') -BuildDir $BuildDir -Configuration $Configuration `
            -Tier $Tier -Mode $TestMode -DesktopTests $DesktopTests -Monitor $Monitor -Jobs $TestJobs
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
