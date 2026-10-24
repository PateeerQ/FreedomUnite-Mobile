<#
.SYNOPSIS
    Run the recompiled MHFU once and save the log.

.DESCRIPTION
    Runs mhfu_host against work\ULES01213_EBOOT.BIN with work\root as the
    device root. The run stops when the entry returns, when execution reaches
    code that was not recompiled, after -Budget dispatched calls, or after
    -Seconds of wall-clock time. The
    report at the end (and work\run.log) is what to look at.

    -Window opens a window that shows the game and takes the keyboard:
    arrows = D-pad, X = Cross, S = Circle, Z = Square, A = Triangle,
    Q = L, W = R, Space/Enter = Start, V/Backspace = Select, I/J/K/L = stick.
    The game then runs at 60 frames a second; close the window to stop.

    -Press feeds timed presses, counted in game frames (one controller read
    each): "300:CROSS,420:START,500-560:UP". -Snap N saves every Nth frame to
    work\root\frames\.

    -Name is the text the game's on-screen keyboard gets (the character name;
    default Hunter).

.EXAMPLE
    .\scripts\run.ps1
    .\scripts\run.ps1 -Seconds 20
    .\scripts\run.ps1 -Window
    .\scripts\run.ps1 -Press "300:CROSS" -Snap 60
    .\scripts\run.ps1 -Window -Name "Fathir"
#>
[CmdletBinding()]
param(
    [uint64]$Budget = 2000000000,
    # Wall-clock limit; the host reports and stops when it is reached.
    [int]$Seconds = 60,
    # Log every file the game opens (psprecomp's PSPRECOMP_TRACE_IO).
    [switch]$NoTraceIo,
    # The disc image; defaults to the one setup.ps1 was given (work\iso.txt).
    [string]$Iso,
    # Show the game in a window and play it with the keyboard.
    [switch]$Window,
    [int]$Scale = 2,
    # Timed button presses, in game frames, e.g. "300:CROSS,420:START".
    [string]$Press,
    # Save every Nth frame to work\root\frames.
    [int]$Snap = 0,
    # What the on-screen keyboard types: the character name.
    [string]$Name,
    # Diagnostics: switch the depth buffer off / restore the old VFPU behaviour
    # (all of it, or -VfpuOff "prefix,vrot,vdiv" for parts).
    [switch]$NoDepth,
    [switch]$VfpuCompat,
    [string]$VfpuOff,
    # Multiplayer (see host\lobby.h): open a room, or join one by its code.
    # -Root runs a second player on this PC with its own saves and settings
    # (e.g. work\root2; ms\ is copied from work\root the first time).
    [switch]$NetCreate,
    [string]$NetJoin,
    [string]$Nick,
    [string]$Mac,
    [string]$Relay,
    # (-Root for short. Not named $Root: PowerShell names ignore case, and
    # the script's own $root is the device root.)
    [Alias("Root")] [string]$DeviceRoot,
    [string]$Config = "Release"
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$work = Join-Path $repo "work"

$exe = @(
    "$repo\build\$Config\mhfu_host.exe",
    "$repo\build\mhfu_host.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { throw "mhfu_host.exe not found -- run .\scripts\setup.ps1 first" }

$log = Join-Path $work "run.log"
# Overlays the game loads at run time are saved here (see host\hle_mhfu.c).
New-Item -ItemType Directory -Force (Join-Path $work "overlays") | Out-Null
# The runtime logs to stderr. Windows PowerShell turns a native program's
# stderr into error records (fatal under 'Stop'), so let cmd merge the two
# streams before PowerShell sees them.
if (-not $NoTraceIo) { $env:PSPRECOMP_TRACE_IO = "1" }
$eboot = Join-Path $work "ULES01213_EBOOT.BIN"
$root  = Join-Path $work "root"
if ($DeviceRoot) {
    # A second device: its own memory stick and net.cfg, the same disc.
    $main = $root
    $root = if ([IO.Path]::IsPathRooted($DeviceRoot)) { $DeviceRoot } else { Join-Path $repo $DeviceRoot }
    New-Item -ItemType Directory -Force $root | Out-Null
    foreach ($d in "ms", "flash0") {
        if (-not (Test-Path (Join-Path $root $d)) -and (Test-Path (Join-Path $main $d))) {
            Copy-Item -Recurse (Join-Path $main $d) (Join-Path $root $d)
        }
    }
    $log = Join-Path $work ("run-" + (Split-Path -Leaf $root) + ".log")
}
$isoTxt = Join-Path $work "iso.txt"
if (-not $Iso -and (Test-Path $isoTxt)) { $Iso = (Get-Content $isoTxt -TotalCount 1).Trim() }
$isoArg = ""
if ($Iso) {
    if (-not (Test-Path $Iso)) { throw "disc image not found: $Iso" }
    $isoArg = "--iso `"$((Resolve-Path $Iso).Path)`""
} else {
    Write-Warning "No disc image (run setup.ps1 again, or pass -Iso); disc0: uses work\root\disc"
}
# With a window the run lasts until it is closed, unless -Seconds was given.
if ($Window -and -not $PSBoundParameters.ContainsKey('Seconds')) { $Seconds = 3600 }
$extra = ""
if ($Window) { $extra += " --window $Scale" }
if ($Press)  { $extra += " --press `"$Press`"" }
if ($Name)   { $extra += " --name `"$Name`"" }
if ($NetCreate) { $extra += " --net-create" }
if ($NetJoin)   { $extra += " --net-join `"$NetJoin`"" }
if ($Nick)      { $extra += " --nick `"$Nick`"" }
if ($Mac)       { $extra += " --mac $Mac" }
if ($Relay)     { $extra += " --relay $Relay" }
$env:PSPRECOMP_NO_DEPTH    = if ($NoDepth)    { "1" } else { "" }
$env:PSPRECOMP_VFPU_COMPAT = if ($VfpuCompat) { "1" } elseif ($VfpuOff) { $VfpuOff } else { "" }
if ($Snap -gt 0) {
    $frames = Join-Path $root "frames"
    if (Test-Path $frames) { Remove-Item -Recurse -Force $frames }
    New-Item -ItemType Directory -Force $frames | Out-Null
    $extra += " --snap $Snap"
}
cmd /c "`"$exe`" `"$eboot`" `"$root`" $isoArg --budget $Budget --seconds $Seconds$extra 2>&1" | Tee-Object -FilePath $log
# Keep every run's log too, so runs can be compared afterwards.
$logs = Join-Path $work "logs"
New-Item -ItemType Directory -Force $logs | Out-Null
$tag = (Get-Date -Format "yyyyMMdd-HHmmss")
if ($VfpuCompat) { $tag += "-vfpucompat" } elseif ($VfpuOff) { $tag += "-off-" + ($VfpuOff -replace '[^a-z0-9]', '_') }
if ($NoDepth) { $tag += "-nodepth" }
Copy-Item $log (Join-Path $logs "run-$tag.log")
$frame = Join-Path $root "frame.ppm"
if (Test-Path $frame) { Copy-Item $frame (Join-Path $logs "frame-$tag.ppm") }
Write-Host "`nLog saved to $log" -ForegroundColor Green
