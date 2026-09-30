<# Runs the real CTest registrations; cached, deferred and newly passed are distinct. #>
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [string]$BuildDir='build',
    [ValidateSet('Affected','Full')][string]$Mode='Affected',
    [ValidateSet('Run','Defer')][string]$DesktopTests='Defer',
    [ValidateRange(1,16)][int]$Jobs=4,
    [switch]$ListOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'test-selection.ps1')
$repo=Split-Path -Parent $PSScriptRoot
$build=if ([IO.Path]::IsPathRooted($BuildDir)) { [IO.Path]::GetFullPath($BuildDir) }
       else { [IO.Path]::GetFullPath((Join-Path $repo $BuildDir)) }
if (-not (Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') -Pattern '^EASNAP_BUILD_TESTS:BOOL=ON$' -Quiet)) {
    throw 'Tests are not enabled in this build. Run scripts/build.ps1 with TestMode Affected or Full first.'
}
if ($Mode -eq 'Full' -and -not $PSBoundParameters.ContainsKey('DesktopTests')) { $DesktopTests='Run' }
$registered = & ctest --test-dir $build -C $Configuration --show-only=json-v1
if ($LASTEXITCODE -ne 0) { throw 'CTest enumeration failed.' }
$tests=@(($registered -join "`n" | ConvertFrom-Json).tests)
if (-not $tests.Count) { throw 'No registered tests. Configure with EASNAP_BUILD_TESTS=ON.' }
$runtime=Get-Content -LiteralPath (Join-Path $build "easnap-test-inputs-$Configuration.json") -Raw | ConvertFrom-Json
$hashes=@{}
$global=Get-EasnapGlobalTestDigest $repo $runtime $hashes
# Hash environment values in memory; never write potentially sensitive values.
$environment=Get-EasnapDigest ((Get-ChildItem Env: | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join "`n")
$context=Get-EasnapDigest ($global + $environment + [Environment]::OSVersion.VersionString +
    (Get-EasnapFileDigest (Join-Path $build 'CMakeCache.txt') $hashes))
$fingerprints=@{}
foreach ($test in $tests) { $fingerprints[$test.name]=Get-EasnapTestFingerprint $test $runtime $Configuration $context $hashes }
$testing=Join-Path $build 'Testing'
$cachePath=Join-Path $testing "easnap-passed-$Configuration.json"
$passed=@{}
if (Test-Path -LiteralPath $cachePath) {
    try {
        $cache=Get-Content -LiteralPath $cachePath -Raw | ConvertFrom-Json
        foreach ($entry in $cache.PSObject.Properties) { $passed[$entry.Name]=$entry.Value }
    } catch { Write-Warning 'Unreadable test cache; tests will run again.'; $passed=@{} }
}
$versionText=(& ctest --version | Select-Object -First 1)
$junitAvailable=$versionText -match '(\d+)\.(\d+)' -and [version]"$($matches[1]).$($matches[2])" -ge [version]'3.21'
if (-not $junitAvailable) { $Mode='Full'; Write-Host 'CTest < 3.21: run uncached tests; CMake 3.20 remains supported.' }
$work=Select-EasnapTestWork $tests $fingerprints $passed $Mode $DesktopTests
Write-Host "Tests: $($work.Run.Count) selected, $($work.Cached.Count) unchanged with a pass <24h old, $($work.Deferred.Count) pending desktop checks."
if ($work.Deferred.Count) { Write-Warning ("PENDING (not passed): " + ($work.Deferred -join ', ') + '. Run scripts/test.ps1 -DesktopTests Run when the desktop is available.') }
if ($ListOnly) { $work | ConvertTo-Json -Depth 4; return }
if (-not $work.Run.Count) { Write-Host 'No new test execution. Previous passes and pending checks are listed above.'; return }
New-Item -ItemType Directory -Path $testing -Force | Out-Null
function Save-Passes {
    $temporary=$cachePath + ".$PID.tmp"
    $passed | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $cachePath -Force
}
# Invalidate selected results before execution: failure/interruption of a forced
# rerun must never leave an older pass eligible for reuse.
foreach ($name in $work.Run) { $passed.Remove($name) }
Save-Passes
$report=Join-Path $testing ("iteration-$Configuration-"+[guid]::NewGuid().ToString('N')+'.xml')
$pattern='^('+(($work.Run | ForEach-Object { [regex]::Escape($_) }) -join '|')+')$'
$arguments=@('--test-dir',$build,'-C',$Configuration,'--output-on-failure','--parallel',"$Jobs",'-R',$pattern,'--no-tests=error')
if ($junitAvailable) { $arguments+=@('--output-junit',$report) }
& ctest @arguments
$testExit=$LASTEXITCODE
$actualPasses=@(Get-EasnapJUnitPasses $report $work.Run)
foreach ($name in $actualPasses) { $passed[$name]=@{fingerprint=$fingerprints[$name]; utc=[datetime]::UtcNow.ToString('o')} }
Save-Passes
if (Test-Path -LiteralPath $report) {
    Move-Item -LiteralPath $report -Destination (Join-Path $testing "iteration-$Configuration.xml") -Force
}
Write-Host "This run: $($actualPasses.Count) recorded passes; $($work.Deferred.Count) desktop checks remain pending."
if ($testExit -ne 0) { throw "CTest failed (exit $testExit). Failed/unrun tests were not cached." }
if ($junitAvailable -and $actualPasses.Count -ne $work.Run.Count) { throw 'Some selected tests did not pass (possibly skipped); they remain pending.' }
