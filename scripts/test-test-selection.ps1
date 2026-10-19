$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-selection.ps1')
$repo=Split-Path -Parent $PSScriptRoot
$fixture=Join-Path $repo ('.local/test-selection-'+[guid]::NewGuid().ToString('N'))
$checks=0
function Assert-Selection([bool]$Condition, [string]$Description) {
    if (-not $Condition) { throw "FAIL: $Description" }
    $script:checks++
}
try {
    foreach ($directory in @('apps/easnap-desktop/src','scripts')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $directory) -Force | Out-Null
    }
    foreach ($file in @('CMakeLists.txt','a.bin','b.bin','helper.bin','apps/easnap-desktop/src/covered.cpp','apps/easnap-desktop/src/shared.h')) {
        Set-Content -LiteralPath (Join-Path $fixture $file) -Value 'first input'
    }
    $a=Join-Path $fixture 'a.bin'; $b=Join-Path $fixture 'b.bin'; $helper=Join-Path $fixture 'helper.bin'
    $runtime=@([PSCustomObject]@{executable=$a; runtime=@($helper); sources=@((Join-Path $fixture 'apps/easnap-desktop/src/covered.cpp'))})
    $tests=@(
        [PSCustomObject]@{name='preview'; command=@($a,'--preview'); properties=@()},
        [PSCustomObject]@{name='visual'; command=@($a,'--visual'); properties=@([PSCustomObject]@{name='LABELS'; value=@('interactive-desktop')})},
        [PSCustomObject]@{name='pure'; command=@($b); properties=@()}
    )
    $fingerprints=@{}
    foreach ($test in $tests) { $fingerprints[$test.name]=Get-EasnapTestFingerprint $test $runtime Release context @{} }
    $empty=Select-EasnapTestWork $tests $fingerprints @{} Affected Run
    Assert-Selection ($empty.Run.Count -eq 3) 'first run executes every registered test'
    $passed=@{}
    foreach ($test in $tests) { $passed[$test.name]=@{fingerprint=$fingerprints[$test.name]; utc=[datetime]::UtcNow.ToString('o')} }
    $same=Select-EasnapTestWork $tests $fingerprints $passed Affected Run
    Assert-Selection ($same.Run.Count -eq 0 -and $same.Cached.Count -eq 3) 'unchanged actual input fingerprints reuse prior passes'
    Assert-Selection ((Select-EasnapTestWork $tests $fingerprints $passed Full Run).Run.Count -eq 3) 'Full bypasses cache'
    $deferred=Select-EasnapTestWork $tests $fingerprints @{} Affected Defer
    Assert-Selection ($deferred.Run.Count -eq 2 -and $deferred.Deferred.Count -eq 1 -and $deferred.Cached.Count -eq 0) 'desktop deferral is not recorded as a pass'
    $tiered=@(
        [PSCustomObject]@{name='logic'; command=@($b); properties=@([PSCustomObject]@{name='LABELS'; value=@('unit')})},
        [PSCustomObject]@{name='media'; command=@($b,'--media'); properties=@([PSCustomObject]@{name='LABELS'; value=@('integration')})},
        [PSCustomObject]@{name='pixels'; command=@($b,'--pixels'); properties=@([PSCustomObject]@{name='LABELS'; value=@('desktop-visual')})},
        [PSCustomObject]@{name='repeat'; command=@($b,'--repeat'); properties=@([PSCustomObject]@{name='LABELS'; value=@('integration','soak')})},
        [PSCustomObject]@{name='clicks'; command=@($b,'--clicks'); properties=@([PSCustomObject]@{name='LABELS'; value=@('interactive-desktop','soak')})}
    )
    $tierPrints=@{}; foreach ($test in $tiered) { $tierPrints[$test.name]='x' }
    $now=[datetime]::UtcNow
    Assert-Selection ((Get-EasnapTestTier $tiered[0]) -eq 'fast' -and (Get-EasnapTestTier $tiered[1]) -eq 'integration' -and
        (Get-EasnapTestTier $tiered[2]) -eq 'desktop' -and (Get-EasnapTestTier $tiered[3]) -eq 'soak') 'labels map to one tier each'
    $fast=Select-EasnapTestWork $tiered $tierPrints @{} Affected Defer $now Fast
    Assert-Selection ($fast.Run.Count -eq 1 -and $fast.Run[0] -eq 'logic' -and $fast.Excluded.Count -eq 4 -and $fast.Deferred.Count -eq 0) 'Fast runs unit tests only and defers nothing'
    $standard=Select-EasnapTestWork $tiered $tierPrints @{} Affected Defer $now Standard
    Assert-Selection (($standard.Run -join ',') -eq 'logic,media' -and ($standard.Deferred -join ',') -eq 'pixels' -and ($standard.Excluded -join ',') -eq 'repeat,clicks') 'Standard defers visible-desktop tests and excludes soak'
    $full=Select-EasnapTestWork $tiered $tierPrints @{} Affected Run $now Full
    Assert-Selection (($full.Run -join ',') -eq 'logic,media,pixels' -and $full.Excluded.Count -eq 2) 'Full runs desktop tests but not soak'
    $soak=Select-EasnapTestWork $tiered $tierPrints @{} Affected Run $now Soak
    Assert-Selection ($soak.Run.Count -eq 5 -and $soak.Excluded.Count -eq 0) 'Soak runs everything'
    $soakDeferred=Select-EasnapTestWork $tiered $tierPrints @{} Affected Defer $now Soak
    Assert-Selection (($soakDeferred.Deferred -join ',') -eq 'pixels,clicks' -and 'repeat' -in $soakDeferred.Run) 'a soak test that needs the desktop is deferred with the desktop'
    $excludedPass=@{ repeat=@{fingerprint='x'; utc=$now.ToString('o')} }
    Assert-Selection ('repeat' -notin (Select-EasnapTestWork $tiered $tierPrints $excludedPass Affected Run $now Full).Cached) 'an excluded test is never reported as a cached pass'
    $launcher=Join-Path $fixture 'launcher.bin'; Set-Content -LiteralPath $launcher -Value 'runner'
    $wrapped=[PSCustomObject]@{name='wrapped'; command=@($launcher,$a,'--native'); properties=@()}
    $wrappedBefore=Get-EasnapTestFingerprint $wrapped $runtime Release context @{}
    Set-Content -LiteralPath $helper -Value 'helper changed under a wrapped test'
    Assert-Selection ((Get-EasnapTestFingerprint $wrapped $runtime Release context @{}) -ne $wrappedBefore) 'a wrapped test depends on its real executable helpers'
    Set-Content -LiteralPath $helper -Value 'first input'
    $env:EASNAP_SELECTION_PROBE='1'; $withVariable=Get-EasnapTestEnvironmentDigest
    Remove-Item Env:EASNAP_SELECTION_PROBE; $env:UNRELATED_SELECTION_PROBE='1'
    Assert-Selection ((Get-EasnapTestEnvironmentDigest) -ne $withVariable) 'test variables are part of the fingerprint'
    $withoutVariable=Get-EasnapTestEnvironmentDigest; $env:UNRELATED_SELECTION_PROBE='2'
    Assert-Selection ((Get-EasnapTestEnvironmentDigest) -eq $withoutVariable) 'unrelated environment changes do not rerun tests'
    Remove-Item Env:UNRELATED_SELECTION_PROBE
    $passed.Remove('pure')
    Assert-Selection ((Select-EasnapTestWork $tests $fingerprints $passed Affected Run).Run[0] -eq 'pure') 'missing/failed result must run'
    $passed['pure']=@{fingerprint=$fingerprints['pure']; utc=[datetime]::UtcNow.AddDays(-2).ToString('o')}
    Assert-Selection ((Select-EasnapTestWork $tests $fingerprints $passed Affected Run).Run.Count -eq 1) 'old pass expires'
    $passed['pure'].utc=[datetime]::UtcNow.AddDays(1).ToString('o')
    Assert-Selection ((Select-EasnapTestWork $tests $fingerprints $passed Affected Run).Run.Count -eq 1) 'future timestamps cannot authorize reuse'
    $passed['pure'].utc='invalid cache timestamp'
    Assert-Selection ((Select-EasnapTestWork $tests $fingerprints $passed Affected Run).Run.Count -eq 1) 'malformed cache entry forces a rerun'
    Set-Content -LiteralPath $helper -Value 'new helper binary bytes'
    $changed=@{}
    foreach ($test in $tests) { $changed[$test.name]=Get-EasnapTestFingerprint $test $runtime Release context @{} }
    Assert-Selection ($changed['preview'] -ne $fingerprints['preview'] -and $changed['visual'] -ne $fingerprints['visual']) 'helper changes invalidate every dependent test'
    Assert-Selection ($changed['pure'] -eq $fingerprints['pure']) 'unrelated executable stays unchanged'
    Assert-Selection ((Get-EasnapTestFingerprint $tests[2] $runtime Debug context @{}) -ne $fingerprints['pure']) 'configurations cannot share passes'
    $tests[2].command=@($b,'--new-scenario')
    Assert-Selection ((Get-EasnapTestFingerprint $tests[2] $runtime Release context @{}) -ne $fingerprints['pure']) 'command/scenario changes invalidate pass'
    $globalBefore=Get-EasnapGlobalTestDigest $fixture $runtime @{}
    Set-Content -LiteralPath (Join-Path $fixture 'notes.md') -Value 'unrelated documentation'
    Assert-Selection ((Get-EasnapGlobalTestDigest $fixture $runtime @{}) -eq $globalBefore) 'documentation does not rerun code tests'
    Set-Content -LiteralPath (Join-Path $fixture 'apps/easnap-desktop/src/shared.h') -Value 'new shared header'
    Assert-Selection ((Get-EasnapGlobalTestDigest $fixture $runtime @{}) -ne $globalBefore) 'headers conservatively invalidate all tests'
    $globalBefore=Get-EasnapGlobalTestDigest $fixture $runtime @{}
    Set-Content -LiteralPath (Join-Path $fixture 'apps/easnap-desktop/src/unmapped.cpp') -Value 'new application caller'
    Assert-Selection ((Get-EasnapGlobalTestDigest $fixture $runtime @{}) -ne $globalBefore) 'unmapped production changes cannot bypass regression tests'
    $xml=Join-Path $fixture 'results.xml'
    Set-Content -LiteralPath $xml -Value '<testsuite><testcase name="preview"/><testcase name="visual"><skipped/></testcase><testcase name="pure"><failure/></testcase><testcase name="unexpected"/></testsuite>'
    $actual=@(Get-EasnapJUnitPasses $xml @('preview','visual','pure'))
    Assert-Selection ($actual.Count -eq 1 -and $actual[0] -eq 'preview') 'only selected, actually passed JUnit cases may be cached'
    Assert-Selection (@(Get-EasnapJUnitPasses (Join-Path $fixture 'missing.xml') @('preview')).Count -eq 0) 'missing result never implies success'
    $skips=@(Get-EasnapJUnitSkips $xml @('preview','visual','pure'))
    Assert-Selection ($skips.Count -eq 1 -and $skips[0] -eq 'visual') 'a skipped case is neither a pass nor a failure'
    Assert-Selection (@(Get-EasnapJUnitSkips (Join-Path $fixture 'missing.xml') @('visual')).Count -eq 0) 'missing result never implies a skip'
    $missing=$false
    try { Get-EasnapFileDigest (Join-Path $fixture 'missing.bin') @{} | Out-Null } catch { $missing=$true }
    Assert-Selection $missing 'missing build artifact fails closed'
    # Lab integration: host-only list and capture-path reading.
    $spec=[PSCustomObject]@{ tests=[PSCustomObject]@{
        hostOnly=@('clicks');
        captureTests=@('preview','real_*');
        capturePathRules=@(
            [PSCustomObject]@{ pattern='EASNAP_CAPTURE_PATH=(\w+)'; path='$1' },
            [PSCustomObject]@{ pattern='Desktop Duplication not available here'; path='GDI' },
            [PSCustomObject]@{ pattern='GDI screen blit \(fallback\)'; path='GDI'; test='real_*' }) } }
    Assert-Selection (((Get-EasnapHostOnlyTests $spec) -join ',') -eq 'clicks') 'host-only tests come from tests.hostOnly'
    Assert-Selection (@(Get-EasnapHostOnlyTests $null).Count -eq 0) 'no spec means no host-only tests'
    $rules=Get-EasnapCaptureRules $spec
    Assert-Selection ((Test-EasnapCaptureTest 'preview' $rules) -and (Test-EasnapCaptureTest 'real_desktop' $rules) -and -not (Test-EasnapCaptureTest 'pure' $rules)) 'capture tests match by name or wildcard'
    Assert-Selection ((Get-EasnapCapturePath $rules.Rules "x`nEASNAP_CAPTURE_PATH=Duplication (probe)`n" 'preview') -eq 'Duplication') 'group reference names the path'
    Assert-Selection ((Get-EasnapCapturePath $rules.Rules 'Desktop Duplication not available here; 3 pictures' 'preview') -eq 'GDI') 'literal rule names the fallback'
    Assert-Selection ((Get-EasnapCapturePath $rules.Rules 'provider=GDI screen blit (fallback)' 'preview') -eq '') 'a rule limited to other tests does not apply'
    Assert-Selection ((Get-EasnapCapturePath $rules.Rules 'provider=GDI screen blit (fallback)' 'real_desktop') -eq 'GDI') 'a rule limited to a test applies there'
    Assert-Selection ((Format-EasnapCaptureResult 'passed' 'GDI') -eq 'passed (GDI fallback - Duplication path not exercised)') 'a pass on the fallback says so'
    Assert-Selection ((Format-EasnapCaptureResult 'passed' 'Duplication') -eq 'passed (Duplication)' -and (Format-EasnapCaptureResult 'failed' 'GDI') -eq 'failed') 'other results keep their word'
    $capture=Join-Path $fixture 'capture.xml'
    Set-Content -LiteralPath $capture -Value '<testsuite><testcase name="preview" status="run"><properties><property name="cmake_labels" value="x"/></properties><system-out>EASNAP_CAPTURE_PATH=WGC (observer)</system-out></testcase><testcase name="pure" status="run"><system-out>nothing</system-out></testcase></testsuite>'
    $outputs=Get-EasnapJUnitOutputs $capture @('preview','pure')
    Assert-Selection ($outputs['preview'] -like '*WGC*' -and $outputs['pure'] -eq 'nothing') 'system-out is read per selected test'
    Add-EasnapJUnitCaptureProperties $capture @{ preview='WGC' }
    Add-EasnapJUnitCaptureProperties $capture @{ preview='WGC' }
    $stored=[xml](Get-Content -LiteralPath $capture -Raw)
    $properties=@($stored.SelectNodes("//testcase[@name='preview']/properties/property[@name='easnap_capture_path']"))
    Assert-Selection ($properties.Count -eq 1 -and $properties[0].GetAttribute('value') -eq 'WGC') 'the JUnit file carries the capture path once'
    Assert-Selection (@($stored.SelectNodes("//testcase[@name='preview']/properties/property[@name='cmake_labels']")).Count -eq 1) 'existing properties survive'
    Write-Host "Test selection: $checks checks passed."
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if (-not $resolved.StartsWith([IO.Path]::GetFullPath((Join-Path $repo '.local'))+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture path escaped workspace.' }
    if (Test-Path -LiteralPath $resolved) {
        $entries=@(Get-Item -LiteralPath $resolved -Force)+@(Get-ChildItem -LiteralPath $resolved -Recurse -Force)
        if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Refuse fixture cleanup through a reparse point.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
