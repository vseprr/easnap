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
    $missing=$false
    try { Get-EasnapFileDigest (Join-Path $fixture 'missing.bin') @{} | Out-Null } catch { $missing=$true }
    Assert-Selection $missing 'missing build artifact fails closed'
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
