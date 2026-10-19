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

function Read-EasnapJUnitCases([string]$Report) {
    if (-not (Test-Path -LiteralPath $Report)) { return @() }
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create($Report, $settings)
    try { $xml = New-Object Xml.XmlDocument; $xml.XmlResolver = $null; $xml.Load($reader) }
    finally { $reader.Dispose() }
    return @($xml.SelectNodes('//testcase'))
}

function Get-EasnapJUnitPasses([string]$Report, [string[]]$Selected) {
    return @(Read-EasnapJUnitCases $Report | Where-Object {
        $_.GetAttribute('name') -in $Selected -and
        -not $_.SelectSingleNode('failure|error|skipped') -and
        $_.GetAttribute('status') -notin @('notrun', 'disabled')
    } | ForEach-Object { $_.GetAttribute('name') })
}

# Tests that said they cannot run in this environment (exit code 77 with
# SKIP_RETURN_CODE). They neither passed nor failed.
function Get-EasnapJUnitSkips([string]$Report, [string[]]$Selected) {
    return @(Read-EasnapJUnitCases $Report | Where-Object {
        $_.GetAttribute('name') -in $Selected -and
        -not $_.SelectSingleNode('failure|error') -and
        ($_.SelectSingleNode('skipped') -or $_.GetAttribute('status') -eq 'notrun')
    } | ForEach-Object { $_.GetAttribute('name') })
}

# --- Lab integration: testenv.json, host-only tests, capture paths ----------

# The lab spec (testenv.json at the repository root) is optional for the host
# scripts: without it there are no host-only tests and no capture-path rules.
function Get-EasnapTestEnvSpec([string]$Root) {
    $path = Join-Path $Root 'testenv.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try { return ([IO.File]::ReadAllText($path) | ConvertFrom-Json) }
    catch { Write-Warning "testenv.json could not be read ($($_.Exception.Message)); host-only and capture-path features are off."; return $null }
}

function Get-EasnapSpecValue($Object, [string]$Path, $Default = $null) {
    $current = $Object
    foreach ($segment in $Path.Split('.')) {
        if ($null -eq $current) { return $Default }
        $property = $current.PSObject.Properties[$segment]
        if (-not $property) { return $Default }
        $current = $property.Value
    }
    if ($null -eq $current) { return $Default }
    return $current
}

# Names of the tests the spec keeps on the host (tests.hostOnly), as strings.
function Get-EasnapHostOnlyTests($Spec) {
    return @(Get-EasnapSpecValue $Spec 'tests.hostOnly' @() | ForEach-Object { "$_" } | Where-Object { $_ })
}

# Capture-sensitive tests (tests.captureTests, wildcards allowed) and the
# ordered rules that read the capture path from their output.
function Get-EasnapCaptureRules($Spec) {
    return [PSCustomObject]@{
        Tests = @(Get-EasnapSpecValue $Spec 'tests.captureTests' @() | ForEach-Object { "$_" } | Where-Object { $_ })
        Rules = @(Get-EasnapSpecValue $Spec 'tests.capturePathRules' @())
    }
}

function Test-EasnapCaptureTest([string]$Name, $CaptureRules) {
    foreach ($pattern in $CaptureRules.Tests) { if ($Name -like $pattern) { return $true } }
    return $false
}

# The first rule whose pattern matches the output names the path (Duplication,
# GDI, WGC, ...); '$1' in the path inserts a regex group. '' when nothing matched.
function Get-EasnapCapturePath($Rules, [string]$Text, [string]$TestName = '') {
    foreach ($rule in @($Rules)) {
        $only = "$(Get-EasnapSpecValue $rule 'test' '')"
        if ($only -and $TestName -and $TestName -notlike $only) { continue }
        $pattern = "$(Get-EasnapSpecValue $rule 'pattern' '')"
        if (-not $pattern) { continue }
        $match = [regex]::Match($Text, $pattern, [Text.RegularExpressions.RegexOptions]::Multiline)
        if ($match.Success) {
            $path = "$(Get-EasnapSpecValue $rule 'path' '')"
            for ($index = $match.Groups.Count - 1; $index -ge 1; $index--) { $path = $path.Replace("`$$index", $match.Groups[$index].Value) }
            return $path
        }
    }
    return ''
}

# How a pass is worded: a pass on a fallback path did not exercise the path
# the development PC uses, and the line says so.
function Format-EasnapCaptureResult([string]$Status, [string]$CapturePath) {
    if ($Status -ne 'passed') { return $Status }
    if (-not $CapturePath) { return 'passed (capture path unknown)' }
    if ($CapturePath -eq 'GDI') { return 'passed (GDI fallback - Duplication path not exercised)' }
    return "passed ($CapturePath)"
}

# system-out text of the selected test cases in a JUnit file, by name.
function Get-EasnapJUnitOutputs([string]$Report, [string[]]$Selected) {
    $outputs = @{}
    foreach ($case in Read-EasnapJUnitCases $Report) {
        $name = $case.GetAttribute('name')
        if ($name -notin $Selected) { continue }
        $node = $case.SelectSingleNode('system-out')
        $outputs[$name] = $(if ($node) { "$($node.InnerText)" } else { '' })
    }
    return $outputs
}

# Records the capture path in the JUnit file itself, as a testcase property
# beside CTest's cmake_labels, so the lab and CI readers see it.
function Add-EasnapJUnitCaptureProperties([string]$Report, [hashtable]$CapturePaths) {
    if (-not $CapturePaths.Count -or -not (Test-Path -LiteralPath $Report)) { return }
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create($Report, $settings)
    try { $xml = New-Object Xml.XmlDocument; $xml.XmlResolver = $null; $xml.Load($reader) }
    finally { $reader.Dispose() }
    $changed = $false
    foreach ($case in @($xml.SelectNodes('//testcase'))) {
        $name = $case.GetAttribute('name')
        if (-not $CapturePaths.ContainsKey($name)) { continue }
        $properties = $case.SelectSingleNode('properties')
        if (-not $properties) { $properties = $case.PrependChild($xml.CreateElement('properties')) }
        foreach ($old in @($properties.SelectNodes("property[@name='easnap_capture_path']"))) { [void]$properties.RemoveChild($old) }
        $property = $xml.CreateElement('property')
        $property.SetAttribute('name', 'easnap_capture_path')
        $property.SetAttribute('value', "$($CapturePaths[$name])")
        [void]$properties.AppendChild($property)
        $changed = $true
    }
    if ($changed) { $xml.Save($Report) }
}
