<#
.SYNOPSIS
    Run the game until it stops finding overlays that are not recompiled yet.

.DESCRIPTION
    A run that loads an overlay with no recompiled version saves it to
    work\overlays and stops. This repeats "run, then setup.ps1" until a run
    loads nothing new (or -Rounds is used up), so a scripted path through the
    game collects every overlay on it in one go. The run options are passed
    to run.ps1 as they are.

    setup.ps1 is retried when it fails on a locked file: an editor indexing
    work\gen_ovl (VS Code's C/C++ extension does) can hold one open briefly.

.EXAMPLE
    .\scripts\harvest.ps1 -Iso $iso -Eboot $eb -Press "300:CROSS" -Seconds 120
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Iso,
    [Parameter(Mandatory = $true)] [string]$Eboot,
    [string]$Press,
    [int]$Seconds = 120,
    [string]$Name = "Hunter",
    [int]$Snap = 0,
    [int]$Rounds = 8
)

$ErrorActionPreference = 'Stop'
$scripts = $PSScriptRoot

for ($r = 1; $r -le $Rounds; $r++) {
    $runArgs = @{ Window = $true; Seconds = $Seconds; Name = $Name }
    if ($Press) { $runArgs.Press = $Press }
    if ($Snap)  { $runArgs.Snap = $Snap }
    $out = & "$scripts\run.ps1" @runArgs *>&1 | Out-String
    $new = @([regex]::Matches($out, 'overlay slot \d+: (\S+) \(([0-9A-F]{8})\) loaded, NOT recompiled') |
        ForEach-Object { "$($_.Groups[1].Value) ($($_.Groups[2].Value))" })
    if (-not $new.Count) {
        Write-Host "round ${r}: no new overlay"
        $out -split "`n" | Select-String 'dispatch misses|unimplemented fw|game frames' | ForEach-Object { "  " + $_.Line.Trim() }
        return
    }
    Write-Host "round ${r}: new $($new -join ', '); recompiling"
    $ok = $false
    for ($t = 1; $t -le 5 -and -not $ok; $t++) {
        try { $s = & "$scripts\setup.ps1" -Iso $Iso -Eboot $Eboot -KeepDisc *>&1 | Out-String }
        catch { $s = $_.Exception.Message }
        $ok = $s -match 'Done\. Run it'
        if (-not $ok) {
            if ($s -notmatch 'being used by another process') { Write-Host $s; throw "setup.ps1 failed" }
            Start-Sleep -Seconds 15
        }
    }
    if (-not $ok) { throw "setup.ps1 kept failing on a locked file" }
}
Write-Host "stopped after $Rounds rounds"
