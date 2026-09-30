Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build-safety.ps1')

# Only disposable fixtures are passed to the guard. No production directory is deleted.
$workspace = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$fixture = Join-Path $workspace ('.local/clean-guard-' + [guid]::NewGuid().ToString('N'))
$root = Join-Path $fixture 'repo'
$build = Join-Path $root 'output'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$checks = 0
function Assert-Rejected([string]$Path) {
    $rejected = $false
    try { Get-EasnapBuildDirectory $root $Path | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw "Unsafe fixture accepted: $Path" }
    $script:checks++
}
function Write-Cache([string]$Source, [string]$Binary, [string]$Project = 'Easnap') {
    @("CMAKE_HOME_DIRECTORY:INTERNAL=$Source", "CMAKE_CACHEFILE_DIR:INTERNAL=$Binary",
      "CMAKE_PROJECT_NAME:STATIC=$Project") | Set-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
}

Assert-Rejected $root
Assert-Rejected '..'
Assert-Rejected (Join-Path $fixture 'outside')
Assert-Rejected (Join-Path $root '../outside')
Assert-Rejected (Join-Path $root 'missing')
Assert-Rejected $build # no cache
Write-Cache $root $build 'OtherProject'
Assert-Rejected $build
Write-Cache $fixture $build
Assert-Rejected $build
Write-Cache $root $fixture
Assert-Rejected $build
Write-Cache $root $build
foreach ($protected in @('apps', 'docs', 'scripts', '.git', '.agents', '.serena', '.github')) {
    Assert-Rejected (Join-Path $root $protected)
}
if ((Get-EasnapBuildDirectory $root 'output') -ne $build) { throw 'Valid build rejected.' }
$checks++
'unique evidence' | Set-Content -LiteralPath (Join-Path $build 'evidence.txt')
Get-EasnapBuildDirectory $root $build | Out-Null
if ((Get-Content -LiteralPath (Join-Path $build 'evidence.txt')) -ne 'unique evidence') {
    throw 'Guard modified evidence.'
}
$checks++
$junction = Join-Path $root 'linked'
New-Item -ItemType Junction -Path $junction -Target $build | Out-Null
Assert-Rejected $junction
$nested = Join-Path $build 'nested-link'
New-Item -ItemType Junction -Path $nested -Target $fixture | Out-Null
Assert-Rejected $build
# Remove links themselves, never recurse through their targets.
[IO.Directory]::Delete($nested)
[IO.Directory]::Delete($junction)
$resolved = [IO.Path]::GetFullPath($fixture)
if (-not $resolved.StartsWith($workspace + '\.local\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture cleanup escaped workspace.'
}
Remove-Item -LiteralPath $resolved -Recurse -Force
Write-Host "Build safety: $checks checks passed."
