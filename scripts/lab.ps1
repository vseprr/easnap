<#
.SYNOPSIS
    Runs the Easnap test lab (winlab) with this repository's testenv.json.

.DESCRIPTION
    Thin wrapper: locates winlab (WINLAB_HOME, else C:\Projects\winlab) and
    forwards the command and options to winlab.ps1 with -Spec pointing at the
    repository's testenv.json. The lab is a Hyper-V VM; see docs/testing.md
    ("Lab") and C:\Projects\winlab\README.md.

    Commands: status, create, provision, snapshot, restore, run, probe,
    parity, verify, connect, screenshot, stop, destroy.

.PARAMETER Command
    The winlab command.

.PARAMETER Tier
    run: Standard (default), Full or Soak.

.PARAMETER Tests
    probe: only these registered tests.

.PARAMETER NoRestore
    run/probe: keep the VM state instead of restoring the golden checkpoint.

.PARAMETER Variant
    Default: the lab of testenv.json. Scale150: a second VM built from the
    same testenv.json with the display scale at 150 %, its own name, storage
    folder and report folder (build\Testing\lab-scale150). The derived spec is
    written to build\Testing\lab-specs\ for every call.

.EXAMPLE
    .\scripts\lab.ps1 run -Tier Full
.EXAMPLE
    .\scripts\lab.ps1 run -Tier Full -Variant Scale150
.EXAMPLE
    .\scripts\lab.ps1 probe
.EXAMPLE
    .\scripts\lab.ps1 verify
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('status', 'create', 'provision', 'snapshot', 'restore', 'run', 'probe', 'parity', 'verify', 'connect', 'screenshot', 'stop', 'destroy')]
    [string]$Command,
    [ValidateSet('Standard', 'Full', 'Soak')][string]$Tier,
    [string[]]$Tests,
    [switch]$NoRestore,
    [switch]$Json,
    [switch]$Shell,
    [switch]$Yes,
    [string]$Label,
    [int]$MaxAgeHours = 0,
    [string]$Name,
    [string]$Verdict,
    [switch]$Force,
    [switch]$NoWait,
    [switch]$SkipReboot,
    [ValidateSet('Default', 'Scale150')][string]$Variant = 'Default'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$winlab = if ($env:WINLAB_HOME) { $env:WINLAB_HOME } else { 'C:\Projects\winlab' }
$entry = Join-Path $winlab 'winlab.ps1'
if (-not (Test-Path -LiteralPath $entry)) {
    throw "winlab was not found at $entry. Clone it to C:\Projects\winlab or set WINLAB_HOME."
}
$repoRoot = Split-Path -Parent $PSScriptRoot
$spec = Join-Path $repoRoot 'testenv.json'

# Further lab images come from the same testenv.json: only these fields
# differ, so tests, provisioning and run commands stay in one file.
$variants = @{
    Scale150 = [ordered]@{
        'vm.name'            = 'easnap-lab-150'
        'vm.storagePath'     = 'C:\VMs\easnap-lab-150'
        'vm.dpi'             = 150
        'image.computerName' = 'EASNAP-LAB-150'
        'report.outputDir'   = 'build/Testing/lab-scale150'
        'report.summaryFile' = 'build/Testing/lab-scale150-latest.md'
    }
}

function Set-SpecValue($Object, [string]$Path, $Value) {
    $parts = $Path.Split('.')
    $node = $Object
    for ($i = 0; $i -lt $parts.Count - 1; $i++) {
        $property = $node.PSObject.Properties[$parts[$i]]
        if ($null -eq $property -or $null -eq $property.Value) {
            throw "testenv.json has no '$($parts[0..$i] -join '.')' for the variant."
        }
        $node = $property.Value
    }
    $leaf = $parts[-1]
    if ($node.PSObject.Properties[$leaf]) { $node.$leaf = $Value }
    else { $node | Add-Member -NotePropertyName $leaf -NotePropertyValue $Value }
}

if ($Variant -ne 'Default') {
    $derived = [IO.File]::ReadAllText($spec, [Text.Encoding]::UTF8) | ConvertFrom-Json
    foreach ($override in $variants[$Variant].GetEnumerator()) { Set-SpecValue $derived $override.Key $override.Value }
    # winlab resolves relative paths against the spec's folder; the derived
    # spec lives elsewhere, so every path that points into the repository is
    # made absolute. A new relative path in testenv.json belongs here too.
    Set-SpecValue $derived 'sync.source' $repoRoot
    foreach ($key in 'report.outputDir', 'report.summaryFile') {
        $relative = $variants[$Variant][$key]
        Set-SpecValue $derived $key ([IO.Path]::GetFullPath((Join-Path $repoRoot $relative)))
    }
    $provision = $derived.PSObject.Properties['provision']
    if ($provision -and $provision.Value.PSObject.Properties['scripts']) {
        $derived.provision.scripts = @($derived.provision.scripts | ForEach-Object {
            if ([IO.Path]::IsPathRooted($_)) { $_ } else { [IO.Path]::GetFullPath((Join-Path $repoRoot $_)) }
        })
    }
    $specDir = Join-Path $repoRoot 'build\Testing\lab-specs'
    New-Item -ItemType Directory -Force -Path $specDir | Out-Null
    $spec = Join-Path $specDir "testenv.$($Variant.ToLowerInvariant()).json"
    # Written beside and moved into place, so a concurrent call never reads
    # half a file.
    $temporary = "$spec.$PID.tmp"
    [IO.File]::WriteAllText($temporary, ($derived | ConvertTo-Json -Depth 32), (New-Object Text.UTF8Encoding $false))
    Move-Item -LiteralPath $temporary -Destination $spec -Force
}
# winlab runs in a child Windows PowerShell so its exit code (0 trusted,
# 2 tree differs, ...) reaches the caller; an in-process `exit` would not.
$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $entry, $Command, '-Spec', $spec)
if ($Tier) { $arguments += @('-Tier', $Tier) }
if ($Tests) { $arguments += @('-Tests', ($Tests -join ',')) }
if ($NoRestore) { $arguments += '-NoRestore' }
if ($Json) { $arguments += '-Json' }
if ($Shell) { $arguments += '-Shell' }
if ($Yes) { $arguments += '-Yes' }
if ($Label) { $arguments += @('-Label', $Label) }
if ($MaxAgeHours) { $arguments += @('-MaxAgeHours', "$MaxAgeHours") }
if ($Name) { $arguments += @('-Name', $Name) }
if ($Verdict) { $arguments += @('-Verdict', $Verdict) }
if ($Force) { $arguments += '-Force' }
if ($NoWait) { $arguments += '-NoWait' }
if ($SkipReboot) { $arguments += '-SkipReboot' }
$preference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { & powershell.exe @arguments; $code = $LASTEXITCODE }
finally { $ErrorActionPreference = $preference }
exit $code
