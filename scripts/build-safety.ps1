Set-StrictMode -Version Latest

function Get-EasnapBuildDirectory {
    <# Validate an existing, in-workspace CMake build before invoking its clean target. #>
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$BuildDirectory
    )

    $root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')
    $candidate = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
        [IO.Path]::GetFullPath($BuildDirectory)
    } else {
        [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
    }
    $candidate = $candidate.TrimEnd('\', '/')
    if (-not $candidate.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
                                   [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Clean requires a build directory strictly inside this repository.'
    }

    # Check each component before descending; Resolve-Path alone does not reject junctions.
    $relative = $candidate.Substring($root.Length + 1)
    $components = $relative -split '[\\/]'
    if ($components[0] -in @('.git', '.agents', '.github', '.serena', 'apps', 'docs', 'scripts')) {
        throw 'Clean cannot target source, documentation or repository configuration.'
    }
    $current = $root
    foreach ($component in @('') + $components) {
        if ($component) { $current = Join-Path $current $component }
        $entry = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        if (-not $entry.PSIsContainer -or
            ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Clean refuses non-directory or reparse-point path components.'
        }
    }

    # Refuse nested junctions too: generated clean rules may otherwise traverse them.
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($candidate)
    while ($pending.Count -gt 0) {
        foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop) {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Clean refuses a build tree containing reparse points.'
            }
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName) }
        }
    }

    $cache = Join-Path $candidate 'CMakeCache.txt'
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $cache -ErrorAction Stop) {
        if ($line -match '^(CMAKE_HOME_DIRECTORY|CMAKE_CACHEFILE_DIR|CMAKE_PROJECT_NAME):[^=]+=(.*)$') {
            $values[$matches[1]] = $matches[2]
        }
    }
    if ($values['CMAKE_PROJECT_NAME'] -ne 'Easnap' -or
        -not $values['CMAKE_HOME_DIRECTORY'] -or -not $values['CMAKE_CACHEFILE_DIR'] -or
        [IO.Path]::GetFullPath($values['CMAKE_HOME_DIRECTORY']).TrimEnd('\', '/') -ne $root -or
        [IO.Path]::GetFullPath($values['CMAKE_CACHEFILE_DIR']).TrimEnd('\', '/') -ne $candidate) {
        throw 'Clean requires an Easnap CMake cache matching this source and build directory.'
    }
    return $candidate
}
