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

    # A miss inside an overlay that IS recompiled is a function discovery did
    # not find (reached through a pointer): it becomes a seed for that overlay.
    if (-not $new.Count) {
        $m = [regex]::Match($out, 'dispatch miss: 0x([0-9A-F]{8}) is in overlay slot (\d+)')
        if ($m.Success) {
            $addr = $m.Groups[1].Value; $slot = $m.Groups[2].Value
            $loads = [regex]::Matches($out, "overlay slot ${slot}: (\S+) \(([0-9A-F]{8})\) loaded, recompiled")
            if ($loads.Count) {
                $last = $loads[$loads.Count - 1]
                $hash = $last.Groups[2].Value
                # Only code can be a seed: an address in the overlay's data or
                # bss is a bad pointer (or code made at run time), and a seed
                # there would recompile whatever bytes the ELF has at that spot.
                $bin = Get-ChildItem (Join-Path (Split-Path -Parent $scripts) "work\overlays") -Filter "*_$hash.bin" |
                    Select-Object -First 1
                if ($bin) {
                    $b = [IO.File]::ReadAllBytes($bin.FullName)
                    $base = [BitConverter]::ToUInt32($b, 8); $code = [BitConverter]::ToUInt32($b, 12)
                    $a = [Convert]::ToUInt32($addr, 16)
                    if ($a -lt $base + 0x40 -or $a -ge $base + 0x40 + $code) {
                        Write-Host "round ${r}: miss at $addr is outside $($last.Groups[1].Value)'s code; not a seed. Stopping."
                        $out -split "`n" | Select-String 'dispatch miss|words at the target' | ForEach-Object { "  " + $_.Line.Trim() }
                        return
                    }
                }
                $seedFile = Join-Path (Split-Path -Parent $scripts) "seeds\ov$hash.txt"
                if (-not (Test-Path $seedFile)) {
                    Set-Content -Path $seedFile -Encoding ASCII -Value @(
                        "# $($last.Groups[1].Value) ($hash), slot $slot.",
                        "# Functions only reached through a pointer built at run time (dispatch misses).")
                }
                if ((Get-Content $seedFile) -contains $addr) { Write-Host "round ${r}: $addr already seeded; stopping"; return }
                Add-Content -Path $seedFile -Value $addr -Encoding ASCII
                $new = @("seed $addr for ov$hash")
            }
        }
    }
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
