<#
.SYNOPSIS
    Formats the C++ sources with clang-format, or checks that they are formatted.
.DESCRIPTION
    Runs clang-format with the repository's .clang-format over every tracked
    C++ file under apps/ (*.cpp, *.h, *.hpp, *.inl). The style keeps the
    existing line breaks (ColumnLimit 0); see docs/development.md.

    clang-format comes from the Visual Studio installation ("C++ Clang tools
    for Windows"), found with vswhere, unless -ClangFormat names one. The tree
    is formatted with major version 19; another major version can format
    differently, so the script refuses it.
.PARAMETER Check
    Change nothing; exit 1 and name the files that are not formatted.
.PARAMETER ClangFormat
    Path to clang-format.exe instead of the one Visual Studio installs.
.EXAMPLE
    .\scripts\format.ps1
.EXAMPLE
    .\scripts\format.ps1 -Check
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [string]$ClangFormat
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$requiredMajor = 19

function Get-ClangFormatMajor([string]$Path) {
    $line = (& $Path --version | Out-String).Trim()
    if ($line -match 'clang-format version (\d+)\.') { return [int]$Matches[1] }
    return -1
}

function Find-ClangFormat {
    # Visual Studio 2022 ships clang-format with its C++ workload (the editor
    # uses it) under VC\Tools\Llvm, whether or not the Clang compiler is added.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere was not found at $vswhere. Install Visual Studio 2022 or pass -ClangFormat."
    }
    $found = New-Object System.Collections.Generic.List[string]
    foreach ($installation in @(& $vswhere -products * -property installationPath)) {
        if (-not $installation) { continue }
        foreach ($relative in 'VC\Tools\Llvm\x64\bin\clang-format.exe', 'VC\Tools\Llvm\bin\clang-format.exe') {
            $candidate = Join-Path $installation $relative
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                if ((Get-ClangFormatMajor $candidate) -eq $requiredMajor) { return $candidate }
                $found.Add($candidate)
            }
        }
    }
    if ($found.Count -gt 0) {
        throw "No clang-format $requiredMajor in Visual Studio; found only: $($found -join ', '). Pass -ClangFormat."
    }
    throw 'No Visual Studio installation contains clang-format. Install the C++ workload or pass -ClangFormat.'
}

$tool = if ($ClangFormat) { [IO.Path]::GetFullPath($ClangFormat) } else { Find-ClangFormat }
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
    throw "clang-format was not found at $tool."
}
$major = Get-ClangFormatMajor $tool
if ($major -ne $requiredMajor) {
    throw "clang-format major version $major at $tool; the sources are formatted with version $requiredMajor."
}
Write-Host "Using clang-format $major ($tool)"

$exitCode = 0
Push-Location -LiteralPath $repoRoot
try {
    $files = @(git -c core.quotepath=false ls-files -- 'apps/*.cpp' 'apps/*.h' 'apps/*.hpp' 'apps/*.inl' |
               Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed.' }
    if ($files.Count -eq 0) { throw 'No C++ files were found under apps/.' }

    if ($Check) {
        $unformatted = New-Object System.Collections.Generic.List[string]
        foreach ($file in $files) {
            # --dry-run --Werror exits non-zero and describes each change on
            # stderr when the file is not formatted. Any other failure (a bad
            # .clang-format, an unreadable file) stops the check instead of
            # being counted as unformatted.
            $output = & {
                $ErrorActionPreference = 'Continue'
                & $tool --style=file --dry-run --Werror -- $file 2>&1 | ForEach-Object { "$_" }
            }
            if ($LASTEXITCODE -eq 0) { continue }
            if (($output -join "`n") -notmatch 'clang-format-violations') {
                throw "clang-format failed on ${file}:`n$($output -join "`n")"
            }
            $unformatted.Add($file)
        }
        if ($unformatted.Count -gt 0) {
            Write-Host ''
            Write-Host "$($unformatted.Count) of $($files.Count) files are not formatted:"
            $unformatted | ForEach-Object { Write-Host "  $_" }
            Write-Host 'Run .\scripts\format.ps1 to format them.'
            $exitCode = 1
        } else {
            Write-Host "All $($files.Count) C++ files are formatted."
        }
    } else {
        $batch = 40
        for ($i = 0; $i -lt $files.Count; $i += $batch) {
            $slice = $files[$i..([Math]::Min($i + $batch, $files.Count) - 1)]
            & $tool --style=file -i -- @slice
            if ($LASTEXITCODE -ne 0) { throw "clang-format failed (exit $LASTEXITCODE)." }
        }
        Write-Host "Formatted $($files.Count) C++ files."
    }
} finally {
    Pop-Location
}
exit $exitCode
