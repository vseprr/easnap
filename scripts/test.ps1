<#
.SYNOPSIS
    Runs the registered CTest tests by tier, reusing unchanged passes.

.DESCRIPTION
    Tiers (see docs/testing.md):
      Fast      unit tests only; seconds.
      Standard  unit + integration. Tests that need the visible desktop are
                listed as pending and are not run. This is the default.
      Full      Standard plus the visible-desktop tests.
      Soak      everything, including the long repeat variants.

    Tests run in two phases. Background tests (including those on a private,
    non-input desktop) run in parallel and never touch the pointer, focus or
    screen. Visible-desktop tests then run one at a time on the chosen
    display, after a warning and a short countdown. A visible-desktop test
    that fails is run once more and reported as unstable if it then passes.

    Cached, pending, excluded and newly passed tests are reported separately.

.PARAMETER Monitor
    Display for visible-desktop tests: a number from the list, part of the
    monitor name (for example SAMSUNG), a \\.\DISPLAYn device, Primary, or Ask
    (default; prompts when several displays are active).

.PARAMETER Filter
    Regular expression; only tests whose name matches are considered.

.PARAMETER HostOnly
    How the tests listed under tests.hostOnly in testenv.json are treated:
    Include (default) runs them like any other test, Exclude leaves them out
    (the lab run), Only runs exactly them (the host run after a lab run).

.EXAMPLE
    .\scripts\test.ps1
.EXAMPLE
    .\scripts\test.ps1 -Tier Full -Monitor SAMSUNG
.EXAMPLE
    .\scripts\test.ps1 -Tier Fast -Mode Full
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [string]$BuildDir='build',
    [ValidateSet('Fast','Standard','Full','Soak')][string]$Tier='Standard',
    # Affected reuses passes whose inputs are unchanged; Full ignores the cache.
    [ValidateSet('Affected','Full')][string]$Mode='Affected',
    [ValidateSet('Run','Defer')][string]$DesktopTests,
    [string]$Monitor='Ask',
    [string]$Filter,
    [ValidateSet('Include','Exclude','Only')][string]$HostOnly='Include',
    [ValidateRange(1,16)][int]$Jobs=4,
    [switch]$NoCountdown,
    [switch]$ListMonitors,
    [switch]$ListOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'test-selection.ps1')
. (Join-Path $PSScriptRoot 'test-monitors.ps1')
if ($ListMonitors) { Get-EasnapTestMonitors | ForEach-Object { Format-EasnapTestMonitor $_ }; return }
if (-not $DesktopTests) { $DesktopTests = if ($Tier -in @('Full','Soak')) { 'Run' } else { 'Defer' } }
$started=[Diagnostics.Stopwatch]::StartNew()
$repo=Split-Path -Parent $PSScriptRoot
$build=if ([IO.Path]::IsPathRooted($BuildDir)) { [IO.Path]::GetFullPath($BuildDir) }
       else { [IO.Path]::GetFullPath((Join-Path $repo $BuildDir)) }
if (-not (Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') -Pattern '^EASNAP_BUILD_TESTS:BOOL=ON$' -Quiet)) {
    throw 'Tests are not enabled in this build. Run scripts/build.ps1 with TestMode Affected or Full first.'
}
$registered = & ctest --test-dir $build -C $Configuration --show-only=json-v1
if ($LASTEXITCODE -ne 0) { throw 'CTest enumeration failed.' }
$tests=@(($registered -join "`n" | ConvertFrom-Json).tests)
if (-not $tests.Count) { throw 'No registered tests. Configure with EASNAP_BUILD_TESTS=ON.' }
if ($Filter) {
    $tests=@($tests | Where-Object { $_.name -match $Filter })
    if (-not $tests.Count) { throw "No registered test matches '$Filter'." }
}
# The lab spec names the tests that stay on the host and how a test's output
# tells which capture path it used. Both are optional on the host.
$labSpec=Get-EasnapTestEnvSpec $repo
$hostOnlyTests=@(Get-EasnapHostOnlyTests $labSpec)
$captureRules=Get-EasnapCaptureRules $labSpec
$capturePaths=@{}
$hostOnlyExcluded=@()
if ($HostOnly -eq 'Exclude' -and $hostOnlyTests.Count) {
    $hostOnlyExcluded=@($tests | Where-Object { $_.name -in $hostOnlyTests } | ForEach-Object { $_.name })
    $tests=@($tests | Where-Object { $_.name -notin $hostOnlyTests })
    if (-not $tests.Count) { throw 'Every selected test is host-only (tests.hostOnly in testenv.json); nothing is left to run here.' }
} elseif ($HostOnly -eq 'Only') {
    if (-not $hostOnlyTests.Count) { throw 'testenv.json lists no host-only tests (tests.hostOnly), so -HostOnly Only selects nothing.' }
    $tests=@($tests | Where-Object { $_.name -in $hostOnlyTests })
    if (-not $tests.Count) { throw ("None of the host-only tests is registered in this build or matches the filter: " + ($hostOnlyTests -join ', ')) }
}
$byName=@{}; foreach ($test in $tests) { $byName[$test.name]=$test }
$interactive=[Environment]::UserInteractive -and -not $env:CI -and -not [Console]::IsInputRedirected

# The display is chosen before fingerprinting: it is part of a desktop test's inputs.
$needsDisplay=$DesktopTests -eq 'Run' -and $Tier -ne 'Fast' -and
    @($tests | Where-Object { (Test-EasnapNeedsDesktop $_) -and ($Tier -eq 'Soak' -or (Get-EasnapTestTier $_) -ne 'soak') }).Count
$display=$null
if ($needsDisplay -and -not $ListOnly) {
    if ($env:EASNAP_TEST_MONITOR -and $Monitor -eq 'Ask') { $Monitor=$env:EASNAP_TEST_MONITOR }
    $display=Resolve-EasnapTestMonitor @(Get-EasnapTestMonitors) $Monitor $interactive
    $env:EASNAP_TEST_MONITOR=$display.Device
}

$runtime=Get-Content -LiteralPath (Join-Path $build "easnap-test-inputs-$Configuration.json") -Raw | ConvertFrom-Json
$hashes=@{}
$global=Get-EasnapGlobalTestDigest $repo $runtime $hashes
$context=Get-EasnapDigest ($global + (Get-EasnapTestEnvironmentDigest) + [Environment]::OSVersion.VersionString +
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
$work=Select-EasnapTestWork $tests $fingerprints $passed $Mode $DesktopTests ([datetime]::UtcNow) $Tier
$background=@($work.Run | Where-Object { -not (Test-EasnapNeedsDesktop $byName[$_]) })
$desktop=@($work.Run | Where-Object { Test-EasnapNeedsDesktop $byName[$_] })

# Previous durations give the warning an honest estimate.
$costs=@{}
$costFile=Join-Path $testing 'Temporary/CTestCostData.txt'
if (Test-Path -LiteralPath $costFile) {
    foreach ($line in Get-Content -LiteralPath $costFile) {
        if ($line -match '^(\S+) \d+ ([\d.]+)$') { $costs[$matches[1]]=[double]$matches[2] }
    }
}
function Get-Estimate([string[]]$Names) {
    $known=@($Names | Where-Object { $costs.ContainsKey($_) })
    if (-not $known.Count) { return 'unknown duration' }
    $seconds=[int](($known | ForEach-Object { $costs[$_] } | Measure-Object -Sum).Sum)
    return "about $seconds s" + $(if ($known.Count -lt $Names.Count) { ' (some tests have no history)' } else { '' })
}

Write-Host ("Tier $Tier ($Configuration): $($background.Count) background, $($desktop.Count) visible-desktop, $($work.Cached.Count) unchanged (<24 h), $($work.Deferred.Count) pending, $($work.Excluded.Count) outside this tier" +
    $(if ($hostOnlyExcluded.Count) { ", $($hostOnlyExcluded.Count) host-only excluded" } else { '' }) + '.')
if ($hostOnlyExcluded.Count) { Write-Host ("HOST-ONLY (not run here; run them on the host with -HostOnly Only): " + ($hostOnlyExcluded -join ', ')) -ForegroundColor Yellow }
if ($work.Deferred.Count) {
    Write-Warning ("PENDING (not run, not passed): " + ($work.Deferred -join ', ') +
        '. Run scripts/test.ps1 -Tier Full when the desktop can be used for tests.')
}
if ($ListOnly) {
    [PSCustomObject]@{ Background=$background; Desktop=$desktop; Cached=$work.Cached
                       Deferred=$work.Deferred; Excluded=$work.Excluded; HostOnlyExcluded=$hostOnlyExcluded } | ConvertTo-Json -Depth 4
    return
}
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

# Runs one CTest invocation and returns the names that actually passed. Tests
# that reported they cannot run here are collected in $skipped.
$skipped=@()
function Invoke-EasnapTests([string[]]$Names, [int]$Parallel, [string]$Phase) {
    $report=Join-Path $testing ("iteration-$Configuration-$Phase-"+[guid]::NewGuid().ToString('N')+'.xml')
    $pattern='^('+(($Names | ForEach-Object { [regex]::Escape($_) }) -join '|')+')$'
    $arguments=@('--test-dir',$build,'-C',$Configuration,'--output-on-failure','--parallel',"$Parallel",'-R',$pattern,'--no-tests=error')
    # The JUnit file keeps a passed test's output only up to CTest's 1 KB default,
    # which cuts the capture-path line; raise it (CTest 3.21+ has the options).
    if ($junitAvailable) { $arguments+=@('--output-junit',$report,'--test-output-size-passed','65536','--test-output-size-failed','1048576') }
    # CTest reports failures on stderr. Under ErrorActionPreference Stop a
    # redirected native stderr line becomes a terminating error, which would
    # skip the result parsing and the retry below.
    $preference=$ErrorActionPreference; $ErrorActionPreference='Continue'
    try { & ctest @arguments 2>&1 | ForEach-Object { "$_" } | Out-Host; $exit=$LASTEXITCODE }
    finally { $ErrorActionPreference=$preference }
    $succeeded=if ($junitAvailable) { @(Get-EasnapJUnitPasses $report $Names) } elseif ($exit -eq 0) { $Names } else { @() }
    if ($junitAvailable) { $script:skipped+=@(Get-EasnapJUnitSkips $report $Names | Where-Object { $_ -notin $script:skipped }) }
    # Capture-sensitive tests say which path they used; the JUnit file gets it
    # as a property and the result lines below word the pass accordingly.
    if ($junitAvailable -and $captureRules.Tests.Count) {
        $captureNames=@($Names | Where-Object { Test-EasnapCaptureTest $_ $captureRules })
        if ($captureNames.Count) {
            $outputs=Get-EasnapJUnitOutputs $report $captureNames
            $found=@{}
            foreach ($name in $captureNames) {
                if ($outputs.ContainsKey($name)) { $found[$name]=Get-EasnapCapturePath $captureRules.Rules $outputs[$name] $name }
            }
            Add-EasnapJUnitCaptureProperties $report $found
            foreach ($name in $found.Keys) { $script:capturePaths[$name]=$found[$name] }
        }
    }
    if (Test-Path -LiteralPath $report) {
        Move-Item -LiteralPath $report -Destination (Join-Path $testing "iteration-$Configuration-$Phase.xml") -Force
    }
    return ,@($succeeded)
}
function Add-Passes([string[]]$Names) {
    foreach ($name in $Names) { $passed[$name]=@{fingerprint=$fingerprints[$name]; utc=[datetime]::UtcNow.ToString('o')} }
    Save-Passes
}

$newPasses=@(); $unstable=@(); $failed=@()
if ($background.Count) {
    Write-Host "`nBackground tests ($(Get-Estimate $background) of test time, $Jobs in parallel). These do not use your pointer, keyboard or screen." -ForegroundColor Cyan
    $ok=Invoke-EasnapTests $background $Jobs 'background'
    Add-Passes $ok; $newPasses+=$ok
    $failed+=@($background | Where-Object { $_ -notin $ok -and $_ -notin $skipped })
}

if ($desktop.Count) {
    if (-not ('Easnap.TestInput' -as [type])) {
        Add-Type -Namespace Easnap -Name TestInput -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO info);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT point);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
public static uint IdleMilliseconds() {
    LASTINPUTINFO info = new LASTINPUTINFO(); info.cbSize = 8;
    return GetLastInputInfo(ref info) ? unchecked((uint)Environment.TickCount - info.dwTime) : 0;
}
'@
    }
    Write-Host ''
    Write-Warning ("VISIBLE-DESKTOP TESTS: $($desktop.Count) test(s), $(Get-Estimate $desktop), on " +
        (Format-EasnapTestMonitor $display).Trim() + '.')
    Write-Host 'They show test windows on that display and some move the real pointer, click and type.' -ForegroundColor Yellow
    Write-Host 'Windows has one pointer and one input queue per session, so moving the mouse, typing,' -ForegroundColor Yellow
    Write-Host 'changing focus or covering that display during these tests can fail them. Use another' -ForegroundColor Yellow
    Write-Host 'display for reading only, and leave the mouse and keyboard alone until they finish.' -ForegroundColor Yellow
    if ($interactive -and -not $NoCountdown) {
        foreach ($remaining in 8..1) {
            Write-Host -NoNewline "`rStarting in $remaining s - press Ctrl+C to skip the desktop tests. "
            Start-Sleep -Seconds 1
        }
        Write-Host ''
        # Do not start in the middle of the user's typing or a drag.
        $deadline=[datetime]::UtcNow.AddSeconds(30)
        while ([Easnap.TestInput]::IdleMilliseconds() -lt 1500 -and [datetime]::UtcNow -lt $deadline) {
            Write-Host -NoNewline "`rWaiting for the mouse and keyboard to be idle...            "
            Start-Sleep -Milliseconds 250
        }
        Write-Host ''
    }
    $cursor=New-Object Easnap.TestInput+POINT
    $haveCursor=[Easnap.TestInput]::GetCursorPos([ref]$cursor)
    try {
        $ok=Invoke-EasnapTests $desktop 1 'desktop'
        $retry=@($desktop | Where-Object { $_ -notin $ok -and $_ -notin $skipped })
        if ($retry.Count) {
            Write-Warning ("Retrying once (a disturbed desktop is the usual cause): " + ($retry -join ', '))
            $second=Invoke-EasnapTests $retry 1 'desktop-retry'
            # A pass on the second attempt is real but not stable: report it
            # and leave it out of the cache so it runs again next time.
            $unstable+=$second
            $failed+=@($retry | Where-Object { $_ -notin $second -and $_ -notin $skipped })
        }
        Add-Passes $ok; $newPasses+=$ok
    }
    finally {
        if ($haveCursor) { [void][Easnap.TestInput]::SetCursorPos($cursor.X, $cursor.Y) }
    }
}

Write-Host ''
if ($capturePaths.Count) {
    Write-Host 'Capture paths (which screen-reading path each capture-sensitive test used here):' -ForegroundColor Cyan
    $captureSummary=@{}
    foreach ($name in ($capturePaths.Keys | Sort-Object)) {
        $status=if ($name -in $failed) { 'failed' } elseif ($name -in $skipped) { 'skipped' } elseif ($name -in $newPasses -or $name -in $unstable) { 'passed' } else { 'not run' }
        $line=Format-EasnapCaptureResult $status $capturePaths[$name]
        $captureSummary[$name]=@{ status=$status; capturePath=$capturePaths[$name]; result=$line }
        Write-Host ('  {0,-36} {1}' -f $name, $line) -ForegroundColor $(if ($capturePaths[$name] -eq 'GDI' -and $status -eq 'passed') { 'Yellow' } elseif ($status -eq 'failed') { 'Red' } else { 'Gray' })
    }
    $captureSummary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $testing "easnap-capture-paths-$Configuration.json") -Encoding UTF8
}
Write-Host ("Result: {0} passed, {1} passed only on retry, {2} failed, {3} skipped, {4} unchanged, {5} pending, {6} outside tier{8}. Wall time {7:n0} s." -f
    $newPasses.Count, $unstable.Count, $failed.Count, $skipped.Count, $work.Cached.Count, $work.Deferred.Count, $work.Excluded.Count, $started.Elapsed.TotalSeconds,
    $(if ($hostOnlyExcluded.Count) { ", $($hostOnlyExcluded.Count) host-only excluded" } else { '' }))
if ($unstable.Count) { Write-Warning ("UNSTABLE (failed, then passed on retry; not cached): " + ($unstable -join ', ')) }
if ($skipped.Count) { Write-Warning ("SKIPPED (could not run in this environment; not passed, not cached): " + ($skipped -join ', ')) }
if ($failed.Count) { throw ("Tests failed: " + ($failed -join ', ') + '. Failed tests are never cached.') }
