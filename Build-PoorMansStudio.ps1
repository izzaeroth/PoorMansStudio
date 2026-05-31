<#
Poor Man's Studio build helper

Save this file as:

    Build-PoorMansStudio.ps1

Place it in the project root beside:

    CMakeLists.txt

Default usage from PowerShell:

    .\Build-PoorMansStudio.ps1

Usage from x64 Native Tools Command Prompt:

    powershell -ExecutionPolicy Bypass -File .\Build-PoorMansStudio.ps1

Default parallel build jobs:

    8

Override parallel jobs:

    .\Build-PoorMansStudio.ps1 -Parallel 16

From x64 Native Tools Command Prompt with override:

    powershell -ExecutionPolicy Bypass -File .\Build-PoorMansStudio.ps1 -Parallel 16

Use a different generator if needed:

    .\Build-PoorMansStudio.ps1 -Generator "Visual Studio 17 2022"
#>

param(
    [string]$Configuration = "Release",
    [string]$Generator = "Visual Studio 18 2026",
    [string]$Platform = "x64",
    [string]$BuildDir = "builds",
    [int]$Parallel = 8,
    [switch]$SkipConfigure
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectRoot

$Started = Get-Date
$Timer = [System.Diagnostics.Stopwatch]::StartNew()
$Result = "FAILED"
$OutputExe = ""

function Write-Section {
    param([string]$Text)

    Write-Host ""
    Write-Host "============================================================"
    Write-Host $Text
    Write-Host "============================================================"
}

function Get-ProjectVersion {
    $versionFile = Join-Path $ProjectRoot "src\app\AppVersion.h"

    if (Test-Path $versionFile) {
        $match = Select-String -Path $versionFile -Pattern 'appVersion\s*=\s*"([^"]+)"' | Select-Object -First 1
        if ($match -and $match.Matches.Count -gt 0) {
            return $match.Matches[0].Groups[1].Value
        }
    }

    $cmakeFile = Join-Path $ProjectRoot "CMakeLists.txt"

    if (-not (Test-Path $cmakeFile)) {
        return "unknown"
    }

    $content = Get-Content $cmakeFile -Raw

    $patterns = @(
        'project\s*\([^)]*VERSION\s+([0-9]+(?:\.[0-9]+){1,3})',
        'set\s*\(\s*(?:PROJECT_VERSION|APP_VERSION|PMS_VERSION|POORMANSSTUDIO_VERSION)\s+["'']?([0-9]+(?:\.[0-9]+){1,3})["'']?\s*\)'
    )

    foreach ($pattern in $patterns) {
        $match = [regex]::Match(
            $content,
            $pattern,
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
            [System.Text.RegularExpressions.RegexOptions]::Singleline
        )

        if ($match.Success) {
            return $match.Groups[1].Value
        }
    }

    return "unknown"
}

function Format-Elapsed {
    param([TimeSpan]$Elapsed)

    if ($Elapsed.TotalHours -ge 1) {
        return "{0} hr {1} min {2} sec" -f [math]::Floor($Elapsed.TotalHours), $Elapsed.Minutes, $Elapsed.Seconds
    }

    if ($Elapsed.TotalMinutes -ge 1) {
        return "{0} min {1} sec" -f [math]::Floor($Elapsed.TotalMinutes), $Elapsed.Seconds
    }

    return "{0} sec" -f [math]::Max(1, [math]::Ceiling($Elapsed.TotalSeconds))
}

function Find-OutputExe {
    $preferredPaths = @(
        [System.IO.Path]::Combine($ProjectRoot, "workspace", "program", "Poor Man's Studio.exe"),
        [System.IO.Path]::Combine($ProjectRoot, $BuildDir, "PoorMansStudio_artefacts", $Configuration, "Poor Man's Studio.exe"),
        [System.IO.Path]::Combine($ProjectRoot, $BuildDir, $Configuration, "Poor Man's Studio.exe"),
        [System.IO.Path]::Combine($ProjectRoot, $BuildDir, $Configuration, "PoorMansStudio.exe")
    )

    foreach ($path in $preferredPaths) {
        if (Test-Path $path) {
            return $path
        }
    }

    $searchRoots = @(
        [System.IO.Path]::Combine($ProjectRoot, "workspace", "program"),
        [System.IO.Path]::Combine($ProjectRoot, $BuildDir)
    )

    foreach ($root in $searchRoots) {
        if (Test-Path $root) {
            $exe = Get-ChildItem -Path $root -Recurse -Filter "Poor Man's Studio.exe" -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1

            if ($null -ne $exe) {
                return $exe.FullName
            }

            $exe = Get-ChildItem -Path $root -Recurse -Filter "PoorMansStudio.exe" -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1

            if ($null -ne $exe) {
                return $exe.FullName
            }
        }
    }

    return ""
}

function Require-Command {
    param([string]$Name)

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue

    if ($null -eq $cmd) {
        throw "Required command not found on PATH: $Name"
    }
}

try {
    Write-Section "Poor Man's Studio Build"

    $Version = Get-ProjectVersion

    Write-Host "Project Root:    $ProjectRoot"
    Write-Host "Version:         $Version"
    Write-Host "Configuration:   $Configuration"
    Write-Host "Generator:       $Generator"
    Write-Host "Platform:        $Platform"
    Write-Host "Build Dir:       $BuildDir"
    Write-Host "Parallel Jobs:   $Parallel"

    Write-Section "Preflight Checks"

    Require-Command "cmake"

    if (-not (Test-Path (Join-Path $ProjectRoot "CMakeLists.txt"))) {
        throw "CMakeLists.txt was not found. Run this script from the project root."
    }

    if (-not (Test-Path (Join-Path $ProjectRoot "external\JUCE"))) {
        Write-Warning "external\JUCE was not found. Configure/build may fail until JUCE is placed there."
    }
    else {
        Write-Host "external\JUCE found."
    }

    Write-Host "CMake found."
    Write-Host "CMakeLists.txt found."

    if (-not $SkipConfigure) {
        Write-Section "Configuring"

        & cmake `
            -S . `
            -B $BuildDir `
            -G $Generator `
            -A $Platform

        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed with exit code $LASTEXITCODE."
        }
    }
    else {
        Write-Section "Skipping Configure"
        Write-Host "Using existing build directory: $BuildDir"
    }

    Write-Section "Building"

    & cmake `
        --build $BuildDir `
        --config $Configuration `
        --parallel $Parallel

    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    $OutputExe = Find-OutputExe

    if ([string]::IsNullOrWhiteSpace($OutputExe)) {
        Write-Warning "Build finished, but the Poor Man's Studio executable was not found under workspace\program or $BuildDir."
    }

    $Result = "SUCCESS"
}
catch {
    Write-Host ""
    Write-Host "Build error:"
    Write-Host $_.Exception.Message
    $Result = "FAILED"
}
finally {
    $Timer.Stop()
    $Finished = Get-Date
    $Version = Get-ProjectVersion

    if ([string]::IsNullOrWhiteSpace($OutputExe)) {
        $OutputExe = Find-OutputExe
    }

    Write-Host ""
    Write-Host "Poor Man's Studio Build Summary"
    Write-Host "--------------------------------"
    Write-Host ("Version:        {0}" -f $Version)
    Write-Host ("Configuration:  {0}" -f $Configuration)
    Write-Host ("Generator:      {0}" -f $Generator)
    Write-Host ("Platform:       {0}" -f $Platform)
    Write-Host ("Parallel Jobs:  {0}" -f $Parallel)
    Write-Host ("Started:        {0}" -f $Started.ToString("yyyy-MM-dd HH:mm:ss"))
    Write-Host ("Finished:       {0}" -f $Finished.ToString("yyyy-MM-dd HH:mm:ss"))
    Write-Host ("Elapsed:        {0}" -f (Format-Elapsed $Timer.Elapsed))
    Write-Host ("Result:         {0}" -f $Result)

    if (-not [string]::IsNullOrWhiteSpace($OutputExe)) {
        Write-Host ("Output:         {0}" -f $OutputExe)
    }
    else {
        Write-Host "Output:         not found"
    }

    Write-Host ""

    if ($Result -ne "SUCCESS") {
        exit 1
    }

    exit 0
}
