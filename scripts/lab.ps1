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

.EXAMPLE
    .\scripts\lab.ps1 run -Tier Full
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
    [switch]$SkipReboot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$winlab = if ($env:WINLAB_HOME) { $env:WINLAB_HOME } else { 'C:\Projects\winlab' }
$entry = Join-Path $winlab 'winlab.ps1'
if (-not (Test-Path -LiteralPath $entry)) {
    throw "winlab was not found at $entry. Clone it to C:\Projects\winlab or set WINLAB_HOME."
}
$spec = Join-Path (Split-Path -Parent $PSScriptRoot) 'testenv.json'
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
