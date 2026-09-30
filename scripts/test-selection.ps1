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
        $Hashes[$full] = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
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
    foreach ($entry in $RuntimeInputs) {
        if ([IO.Path]::GetFullPath($entry.executable) -eq $executable) { $inputs += $entry.runtime }
    }
    foreach ($argument in $Test.command | Select-Object -Skip 1) {
        if ([IO.Path]::IsPathRooted($argument) -and (Test-Path -LiteralPath $argument -PathType Leaf)) { $inputs += $argument }
    }
    $rows = @($inputs | Sort-Object -Unique | ForEach-Object { $_ + ':' + (Get-EasnapFileDigest $_ $Hashes) })
    return Get-EasnapDigest (@($Configuration, $ContextDigest,
        ($Test.command | ConvertTo-Json -Compress -Depth 8),
        ($Test.properties | ConvertTo-Json -Compress -Depth 16), ($rows -join "`n")) -join "`n")
}

function Select-EasnapTestWork([object[]]$Tests, [hashtable]$Fingerprints, [hashtable]$Passed,
                              [string]$Mode, [string]$DesktopTests, [datetime]$Now = [datetime]::UtcNow) {
    $run = @(); $cached = @(); $deferred = @()
    foreach ($test in $Tests) {
        $prior = $Passed[$test.name]
        $fresh = $false
        try {
            if ($prior -and $prior.fingerprint -eq $Fingerprints[$test.name]) {
                $age = $Now - [datetime]::Parse($prior.utc).ToUniversalTime()
                $fresh = $age.TotalSeconds -ge 0 -and $age.TotalHours -lt 24
            }
        } catch { $fresh = $false } # Malformed local cache entries only force a rerun.
        if ($Mode -eq 'Affected' -and $fresh) { $cached += $test.name; continue }
        $labels = @($test.properties | Where-Object name -eq 'LABELS' | ForEach-Object { $_.value })
        if ($DesktopTests -eq 'Defer' -and 'interactive-desktop' -in $labels) {
            $deferred += $test.name
        } else { $run += $test.name }
    }
    return [PSCustomObject]@{ Run=$run; Cached=$cached; Deferred=$deferred }
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
