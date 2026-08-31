<#
.SYNOPSIS
    Launches GTA SA with BorderlessMode under controlled conditions and reports
    whether the game survived, for bugs that only appear on a fresh install.

.DESCRIPTION
    Two things make BorderlessMode bugs hard to reproduce by hand.

    The first is that Windows records per-executable compatibility layers.
    A game folder that has been launched a few times picks up HIGHDPIAWARE,
    which hides the display-scaling class of bug entirely - so a report of
    "a new modpack does not start, an old one does" is really "the new path
    has no AppCompat history yet". -Fresh reproduces that: it points a
    directory junction at the modpack, giving a path Windows has never seen,
    and clears the layer entry again before every run. No files are copied.

    The second is that the plugin does many things at once. The [debug]
    section switches parts of it off; -Matrix walks a list of combinations so
    a failure can be attributed to one of them instead of guessed at.

.PARAMETER Root
    The modpack folder, i.e. the one holding gta_sa.exe.

.PARAMETER Switches
    Nine 0/1 flags, in order:
      windowHook, inputFilters, messagePump, cursorGuard,
      displayGuard, gamePatches, borderlessStyle, conversion, dpiAware
    1 disables that part. '0,0,0,0,0,0,0,0,0' is the plugin fully enabled;
    '1,1,1,1,1,1,1,1,1' leaves it loaded but inert, which is the control worth
    having before blaming the plugin at all.

.PARAMETER Matrix
    Run a built-in bisection sequence instead of a single combination.

.PARAMETER Fresh
    Run from a junction with no AppCompat history, cleared before each launch.

.PARAMETER CompatLayer
    Force a compatibility layer for the run, e.g. DWM8And16BitMitigation or
    DPIUNAWARE. Uses the __COMPAT_LAYER environment variable, so nothing is
    written to the registry.

.EXAMPLE
    .\tools\repro.ps1 -Root 'D:\modpacks\mypack' -Fresh
    Reproduce a first-launch failure.

.EXAMPLE
    .\tools\repro.ps1 -Root 'D:\modpacks\mypack' -Fresh -Matrix
    Reproduce it and narrow it down to one part of the plugin.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Root,

    [string]$Switches = '0,0,0,0,0,0,0,0,0',
    [switch]$Matrix,
    [switch]$Fresh,
    [string]$CompatLayer = '',
    [int]$WaitSeconds = 12,
    [int]$LogTail = 0
)

$ErrorActionPreference = 'Stop'

$switchNames = @(
    'disableWindowHook', 'disableInputFilters', 'disableMessagePump',
    'disableCursorGuard', 'disableDisplayGuard', 'disableGamePatches',
    'disableBorderlessStyle', 'disableConversion', 'disableDpiAware'
)
$layersKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'

function Set-DebugSection {
    param([string]$IniPath, [string]$Values)

    $vals = $Values -split ','
    if ($vals.Count -ne $switchNames.Count) {
        throw "-Switches needs $($switchNames.Count) comma-separated values, got $($vals.Count)."
    }

    $encoding = New-Object System.Text.UnicodeEncoding($false, $true)
    $text = [System.IO.File]::ReadAllText($IniPath, [System.Text.Encoding]::Unicode)
    $text = ($text -replace '(?s)\r?\n\[debug\].*$', '').TrimEnd()

    $block = "`r`n`r`n[debug]`r`nheartbeat=1`r`n"
    for ($i = 0; $i -lt $switchNames.Count; $i++) {
        $block += "$($switchNames[$i])=$($vals[$i])`r`n"
    }
    [System.IO.File]::WriteAllText($IniPath, $text + $block, $encoding)
}

function Remove-DebugSection {
    param([string]$IniPath)

    $encoding = New-Object System.Text.UnicodeEncoding($false, $true)
    $text = [System.IO.File]::ReadAllText($IniPath, [System.Text.Encoding]::Unicode)
    $text = ($text -replace '(?s)\r?\n\[debug\].*$', '').TrimEnd()
    [System.IO.File]::WriteAllText($IniPath, $text + "`r`n", $encoding)
}

function Invoke-Run {
    param([string]$GameRoot, [string]$Values)

    $ini = Join-Path $GameRoot 'scripts\BorderlessMode.ini'
    $log = Join-Path $GameRoot 'scripts\BorderlessMode.log'
    $exe = Join-Path $GameRoot 'gta_sa.exe'

    if (-not (Test-Path $exe)) { throw "No gta_sa.exe in $GameRoot." }
    if (-not (Test-Path $ini)) { throw "No BorderlessMode.ini in $GameRoot\scripts. Run the game once first." }

    Set-DebugSection -IniPath $ini -Values $Values
    Remove-ItemProperty -Path $layersKey -Name $exe -ErrorAction SilentlyContinue
    Remove-Item $log -ErrorAction SilentlyContinue
    Get-Process gta_sa -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400

    if ($CompatLayer) { $env:__COMPAT_LAYER = $CompatLayer }
    else { Remove-Item Env:__COMPAT_LAYER -ErrorAction SilentlyContinue }

    $proc = Start-Process -FilePath $exe -WorkingDirectory $GameRoot -PassThru
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    $survived = $true
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) { $survived = $false; break }
    }
    $seconds = [math]::Round(((Get-Date) - $proc.StartTime).TotalSeconds, 1)
    Get-Process gta_sa -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400

    [pscustomobject]@{
        Switches = $Values
        Result   = if ($survived) { 'ALIVE' } else { "DIED after ${seconds}s" }
        Survived = $survived
        Log      = $log
    }
}

# A junction gives a path Windows has no compatibility history for, which is
# what "a brand new modpack folder" actually means. It costs nothing: the
# files are not copied.
$gameRoot = (Resolve-Path $Root).Path
$junction = $null
if ($Fresh) {
    $junction = Join-Path (Split-Path $gameRoot -Parent) ("_repro_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Junction -Path $junction -Target $gameRoot | Out-Null
    $gameRoot = $junction
    Write-Host "Fresh path: $junction" -ForegroundColor Cyan
}

try {
    # Each step adds one layer, so the first one that fails names the culprit.
    #
    # Deliberately absent: conversion on with borderlessStyle off. That pairing
    # gives the device a windowed swap chain while leaving the window at its
    # fullscreen size, which does not work on its own and would read as a
    # failure of the conversion. Use it by hand with -Switches when isolating,
    # not as a step in a bisection.
    $combinations = if ($Matrix) {
        @(
            '1,1,1,1,1,1,1,1,1'   # inert: is the plugin involved at all?
            '1,1,1,1,1,1,0,0,0'   # core: conversion, restyle and DPI awareness
            '0,0,0,0,0,0,0,0,0'   # everything
        )
    } else {
        @($Switches)
    }

    $results = foreach ($combo in $combinations) {
        $run = Invoke-Run -GameRoot $gameRoot -Values $combo
        $colour = if ($run.Survived) { 'Green' } else { 'Red' }
        Write-Host ("{0}  {1}" -f $run.Switches, $run.Result) -ForegroundColor $colour
        if ($LogTail -gt 0 -and (Test-Path $run.Log)) {
            Get-Content $run.Log -Tail $LogTail | ForEach-Object { "    $_" }
        }
        $run
    }

    Write-Host ""
    Write-Host ("order: " + ($switchNames -replace '^disable', '' -join ','))
    $results | Format-Table Switches, Result -AutoSize
} finally {
    $ini = Join-Path $gameRoot 'scripts\BorderlessMode.ini'
    if (Test-Path $ini) { Remove-DebugSection -IniPath $ini }
    if ($junction) {
        Remove-ItemProperty -Path $layersKey -Name (Join-Path $junction 'gta_sa.exe') -ErrorAction SilentlyContinue
        if (Test-Path $junction) { (Get-Item $junction).Delete() }
        Write-Host "Fresh path removed." -ForegroundColor Cyan
    }
    Remove-Item Env:__COMPAT_LAYER -ErrorAction SilentlyContinue
}
