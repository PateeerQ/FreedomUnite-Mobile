# MHFU-Android

Monster Hunter Freedom Unite (PSP, EU `ULES01213`) statically recompiled to
native code with [psprecomp](https://github.com/sp00nznet/psprecomp), on the
way to a native Android port with PSP-style touch buttons and ad hoc
multiplayer through a private server.

> **No game data here.** The ISO, the decrypted EBOOT, the extracted disc and
> the generated C code all stay in `work/` (gitignored). Bring your own dump.

## Status

Desktop bring-up. The EBOOT and the first overlays recompile, and the game
boots to its language selection screen.

## Layout

| Path | What |
|---|---|
| `psprecomp/` | toolkit, as a git submodule |
| `host/main.c` | desktop bring-up host: loads the EBOOT, sets up the boot thread, runs, reports |
| `host/hle_mhfu.c` | firmware MHFU needs that psprecomp lacks (UMD, Impose, system params, VTimers) |
| `host/libfont.c` | sceLibFont: the PSP's PGF system fonts, which MHFU draws all its text with |
| `host/utility.c` | sceUtility savedata (to `work/root/ms/PSP/SAVEDATA`) and system message dialogs |
| `seeds/eboot.txt` | EBOOT functions only reached through a register-built address |
| `host/input.c` | controller: scripted presses, the window's keys, frame snapshots |
| `host/window.c` | a Windows window that shows the framebuffer, reads the keyboard, and has the Multiplayer menu |
| `host/lobby.c` | "Create Server" / "Join Server": rooms by code, settings in `<root>/net.cfg` |
| `host/adhoc.c` | sceNet, sceNetAdhoc (PDP/PTP), sceNetAdhocctl and sceWlan over the room network |
| `host/net/` | the room network: protocol (`proto.h`), hub, client, sockets/threads for Windows and POSIX |
| `tools/mhfu_hub.c` | the standalone room hub, for a server reachable from the internet |
| `tools/mhfu_netcheck.c` | end-to-end check of the room network without the game |
| `scripts/harvest.ps1` | runs and re-runs setup until a scripted path loads no new overlay |
| `seeds/ov<hash>.txt` | per-overlay functions only reached through a pointer built at run time |
| `host/overlays.h` | table of recompiled overlays (`work/gen_ovl/overlays.c`, written by setup) |
| `tools/mhfu_ovl.c` | wraps an overlay the host saved as an ELF for `allegrexrecomp` |
| `patches/psprecomp/` | fixes to psprecomp not upstream yet; `setup.ps1` applies them |
| `scripts/setup.ps1` | builds the tool, extracts the disc, recompiles the EBOOT, builds the host |
| `scripts/run.ps1` | runs the host and saves `work/run.log`; reads `disc0:` from the ISO setup was given |
| `work/` | everything made from the game (never committed) |

## First-time setup (Windows)

Needs CMake and Visual Studio Build Tools ("Desktop development with C++").
7-Zip is used to extract the ISO if installed; otherwise Windows mounts it.

```powershell
git submodule add https://github.com/sp00nznet/psprecomp psprecomp   # once
git submodule update --init --recursive

.\scripts\setup.ps1 -Iso "<your MHFU EU .iso>" -Eboot "<decrypted EBOOT>"
.\scripts\run.ps1
```

The decrypted EBOOT can be made with PPSSPP: Settings, Tools, Developer tools,
"Dump decrypted EBOOT.BIN on game boot"; it lands in
`Documents\PPSSPP\PSP\SYSTEM\DUMP\ULES01213_EBOOT.BIN`.

## Overlays

MHFU loads part of its code from `DATA.BIN` while it runs. When the host meets
an overlay that has no recompiled version yet, it saves it to
`work/overlays/` and the run stops at the first jump into it. Run
`setup.ps1` again: it recompiles every saved overlay into `work/gen_ovl/`,
and the next run switches each one in when the game loads it. Repeat until a
run saves nothing new.

## Fonts

MHFU's text comes from the PSP's own system fonts (`flash0:/font/*.pgf`),
which are firmware, not part of the game. `setup.ps1` copies them from
PPSSPP's install (`assets\flash0\font`) to `work/root/flash0/font/`; give
another folder with `-Fonts <folder>`. Without them the text is blank.

## Playing in a window

```powershell
.\scripts\run.ps1 -Window            # close the window to stop
```

Keys: arrows = D-pad, X = Cross, S = Circle, Z = Square, A = Triangle,
Q = L, W = R, Space/Enter = Start, V/Backspace = Select, I/J/K/L = stick.
The game is held to 60 frames a second there.

There is no on-screen keyboard: when the game asks for text (the character
name) it gets `-Name`, `Hunter` by default: `.\scripts\run.ps1 -Window -Name "Pateeer"`.

Without a window, `-Press "300:CROSS,420:START"` presses buttons at given game
frames (one controller read each, held 6 frames; `500-560:UP` holds a range)
and `-Snap 60` saves every 60th frame to `work/root/frames/`.

## Reading a run

`work/run.log` ends with a report: how many recompiled calls ran, the last
function reached, firmware calls that are not implemented yet, GE work, and a
framebuffer capture (`work/root/frame.ppm`) once the game sets one. A
`dispatch miss` names an address that was not recompiled.
