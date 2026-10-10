Set-StrictMode -Version Latest

function Get-EasnapDigest([string]$Text) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Get-EasnapFileDigest([string]$Path, [hashtable]$Hashes) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $Hashes.ContainsKey($full)) {
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw "Missing test input: $full. Build this configuration first." }
        # Computed with .NET directly: Get-FileHash lives in a module that Windows
        # PowerShell cannot load when it is started from PowerShell 7, as CTest
        # does on hosted runners.
        $algorithm = [Security.Cryptography.SHA256]::Create()
        $stream = [IO.File]::OpenRead($full)
        try { $Hashes[$full] = ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '') }
        finally { $stream.Dispose(); $algorithm.Dispose() }
    }
    return $Hashes[$full]
}

function Get-EasnapGlobalTestDigest([string]$Root, [object[]]$RuntimeInputs, [hashtable]$Hashes) {
    $covered = @{}
    foreach ($entry in $RuntimeInputs) {
        foreach ($path in $entry.sources) { $covered[[IO.Path]::GetFullPath($path)] = $true }
    }
    # Binaries reflect all compiled dependencies. Unmapped production files,
    # headers, resources and build/tooling changes conservatively invalidate all
    # tests, so an untested/new caller cannot silently bypass regression checks.
    $inputs = @(Get-Item -LiteralPath (Join-Path $Root 'CMakeLists.txt'))
    foreach ($directory in @('apps/easnap-desktop', 'scripts')) {
        $inputs += Get-ChildItem -LiteralPath (Join-Path $Root $directory) -Recurse -File |
            Where-Object {
                $_.Name -eq 'CMakeLists.txt' -or $_.Extension -in @('.cmake', '.ps1', '.py', '.h', '.hpp', '.hh', '.hxx', '.rc', '.ico', '.png', '.manifest', '.in') -or
                ($_.Extension -in @('.cpp', '.cc', '.cxx', '.c') -and -not $covered.ContainsKey($_.FullName))
            }
    }
    $rows = @($inputs | Sort-Object FullName -Unique | ForEach-Object {
        $_.FullName + ':' + (Get-EasnapFileDigest $_.FullName $Hashes)
    })
    return Get-EasnapDigest ($rows -join "`n")
}

function Get-EasnapTestFingerprint($Test, [object[]]$RuntimeInputs,
                                  [string]$Configuration, [string]$ContextDigest, [hashtable]$Hashes) {
    if (-not $Test.PSObject.Properties['command'] -or -not $Test.command.Count) {
        throw "CTest cannot resolve $($Test.name). Build this configuration first."
    }
    $executable = [IO.Path]::GetFullPath($Test.command[0])
    $inputs = @($executable)
    foreach ($argument in $Test.command | Select-Object -Skip 1) {
        if ([IO.Path]::IsPathRooted($argument) -and (Test-Path -LiteralPath $argument -PathType Leaf)) { $inputs += [IO.Path]::GetFullPath($argument) }
    }
    # A test started through the isolated-desktop runner names its real
    # executable as an argument; that executable's helpers are inputs too.
    foreach ($entry in $RuntimeInputs) {
        if ([IO.Path]::GetFullPath($entry.executable) -in $inputs) { $inputs += $entry.runtime }
    }
    $rows = @($inputs | Sort-Object -Unique | ForEach-Object { $_ + ':' + (Get-EasnapFileDigest $_ $Hashes) })
    return Get-EasnapDigest (@($Configuration, $ContextDigest,
        ($Test.command | ConvertTo-Json -Compress -Depth 8),
        ($Test.properties | ConvertTo-Json -Compress -Depth 16), ($rows -join "`n")) -join "`n")
}

# Only variables that can change a test's behaviour belong in the fingerprint.
# Hashing the whole environment made every new terminal rerun the full suite.
function Get-EasnapTestEnvironmentDigest {
    $rows = @(Get-ChildItem Env: | Where-Object { $_.Name -like 'EASNAP_*' } | Sort-Object Name |
        ForEach-Object { "$($_.Name)=$($_.Value)" })
    return Get-EasnapDigest ($rows -join "`n")
}

function Get-EasnapTestLabels($Test) {
    return @($Test.properties | Where-Object name -eq 'LABELS' | ForEach-Object { $_.value })
}

# fast: seconds, no devices. desktop: needs the visible desktop, the real
# pointer or presented pixels. soak: long repeats of an already covered path.
function Get-EasnapTestTier($Test) {
    $labels = Get-EasnapTestLabels $Test
    if ('soak' -in $labels) { return 'soak' }
    if ('interactive-desktop' -in $labels -or 'desktop-visual' -in $labels) { return 'desktop' }
    if ('unit' -in $labels) { return 'fast' }
    return 'integration'
}

function Test-EasnapNeedsDesktop($Test) {
    $labels = Get-EasnapTestLabels $Test
    return 'interactive-desktop' -in $labels -or 'desktop-visual' -in $labels
}

function Select-EasnapTestWork([object[]]$Tests, [hashtable]$Fingerprints, [hashtable]$Passed,
                              [string]$Mode, [string]$DesktopTests, [datetime]$Now = [datetime]::UtcNow,
                              [string]$Tier = 'Full') {
    $run = @(); $cached = @(); $deferred = @(); $excluded = @()
    foreach ($test in $Tests) {
        # Tier limits come first: a test outside the requested tier is neither
        # run nor reported as a pass, whatever the cache holds.
        $kind = Get-EasnapTestTier $test
        if (($kind -eq 'soak' -and $Tier -ne 'Soak') -or ($Tier -eq 'Fast' -and $kind -ne 'fast')) {
            $excluded += $test.name; continue
        }
        $prior = $Passed[$test.name]
        $fresh = $false
        try {
            if ($prior -and $prior.fingerprint -eq $Fingerprints[$test.name]) {
                $age = $Now - [datetime]::Parse($prior.utc).ToUniversalTime()
                $fresh = $age.TotalSeconds -ge 0 -and $age.TotalHours -lt 24
            }
        } catch { $fresh = $false } # Malformed local cache entries only force a rerun.
        if ($Mode -eq 'Affected' -and $fresh) { $cached += $test.name; continue }
        if ($DesktopTests -eq 'Defer' -and (Test-EasnapNeedsDesktop $test)) {
            $deferred += $test.name
        } else { $run += $test.name }
    }
    return [PSCustomObject]@{ Run=$run; Cached=$cached; Deferred=$deferred; Excluded=$excluded }
}

function Get-EasnapJUnitPasses([string]$Report, [string[]]$Selected) {
    if (-not (Test-Path -LiteralPath $Report)) { return @() }
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create($Report, $settings)
    try { $xml = New-Object Xml.XmlDocument; $xml.XmlResolver = $null; $xml.Load($reader) }
    finally { $reader.Dispose() }
    return @($xml.SelectNodes('//testcase') | Where-Object {
        $_.GetAttribute('name') -in $Selected -and
        -not $_.SelectSingleNode('failure|error|skipped') -and
        $_.GetAttribute('status') -notin @('notrun', 'disabled')
    } | ForEach-Object { $_.GetAttribute('name') })
}
