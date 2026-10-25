# FreedomUniteRecomp

Monster Hunter Freedom Unite (PSP, EU `ULES01213`) statically recompiled to
native code with [psprecomp](https://github.com/sp00nznet/psprecomp), on the
way to a native PC and Android port with PSP-style touch buttons and ad hoc
multiplayer through a private server.

> **No game data here.** The ISO, the decrypted EBOOT, the extracted disc and
> the generated C code all stay in `work/` (gitignored). Bring your own dump.

## Status

Desktop bring-up on Windows (PC first, Android after). The game boots, plays
its movies, creates and saves a character, loads the save and runs the village
(Pokke), the house and the Training School in 3D at its own 30 fps, with
sound, music, keyboard and DualSense/Xbox controllers. A village quest has been
played from start to finish without a crash, and two players have hunted
together through a room (ad hoc multiplayer).

### Progress

Rough estimates, as of 2026-10-25. "Recompiled" means the code builds and is
switched in when the game loads it; "tested" means someone has played through it.

| Area | Progress | Notes |
|---|---|---|
| CPU recompilation (Allegrex + VFPU) | ![85%](https://img.shields.io/badge/85%25-brightgreen?style=flat-square) | EBOOT and every overlay recompile; VFPU ops checked against PPSSPP as they turn up |
| Game overlays, recompiled | ![100%](https://img.shields.io/badge/100%25-brightgreen?style=flat-square) | 298 of 298, extracted from DATA.BIN (`tools/re/databin.py`) |
| Game overlays, tested in game | ![10%](https://img.shields.io/badge/10%25-orange?style=flat-square) | title, menus, character creation, village, house, Training School, game_task and a quest region |
| System modules (firmware HLE) | ![70%](https://img.shields.io/badge/70%25-yellow?style=flat-square) | threads, I/O, UMD/ISO, display, ctrl, utility dialogs, savedata, libfont, sceMpeg, sceAtrac, sceNetAdhoc; missing: encrypted saves, the sound PRX |
| Graphics (GE + GL renderer) | ![70%](https://img.shields.io/badge/70%25-yellow?style=flat-square) | textures, lighting, fog, skinning, clipping, render-to-texture; missing: stencil, colour test, logic op, mipmaps, culling, DXT, specular, points/lines |
| Audio | ![80%](https://img.shields.io/badge/80%25-brightgreen?style=flat-square) | sound effects, music (ATRAC3/3+ via FFmpeg), movie sound; small gaps while loading |
| Movies | ![90%](https://img.shields.io/badge/90%25-brightgreen?style=flat-square) | H.264 + ATRAC3+ via FFmpeg, own PSMF demuxer |
| Input | ![90%](https://img.shields.io/badge/90%25-brightgreen?style=flat-square) | keyboard, controllers (right stick = camera), text box for the game's keyboard |
| Save data | ![80%](https://img.shields.io/badge/80%25-brightgreen?style=flat-square) | save and load work (unencrypted) |
| Multiplayer (ad hoc over rooms) | ![75%](https://img.shields.io/badge/75%25-yellow?style=flat-square) | two players in one room: Online Gathering Hall and a quest together work, smooth in quests; movement in the hall lags; internet relay untested |
| Gameplay coverage | ![40%](https://img.shields.io/badge/40%25-yellow?style=flat-square) | village life and a full quest (out, region changes, back) work; the Gathering Hall, more maps and monsters untested |
| PC release (launcher) | ![0%](https://img.shields.io/badge/0%25-lightgrey?style=flat-square) | after the game works end to end |
| Android port | ![0%](https://img.shields.io/badge/0%25-lightgrey?style=flat-square) | NDK + SDL3, touch buttons, GLES renderer |

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
| `host/window.c` | a Windows window: the OpenGL context psprecomp's GL renderer draws with (or the software renderer's framebuffer), the keyboard, the FPS overlay (F3), the Multiplayer menu |
| `host/atrac.c`, `host/mpeg.c` | sceAtrac3plus and sceMpeg, decoding with FFmpeg (`deps/ffmpeg`, gitignored): music, movies with sound |
| `host/audio.c`, `host/gamepad.c` | sound output and controllers, through SDL3 |
| `tools/re/databin.py` | decrypts DATA.BIN as the game does and extracts every overlay |
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

MHFU loads part of its code from `DATA.BIN` while it runs (298 overlays:
menus, stages, monsters). Extract them all once, then let setup recompile them
into `work/gen_ovl/`:

```powershell
python tools\re\databin.py work\ULES01213_EBOOT.BIN work\root\disc\PSP_GAME\USRDIR\DATA.BIN overlays work\overlays
.\scripts\setup.ps1 -Iso "<iso>" -Eboot "<eboot>" -KeepDisc
```

If the host still meets an overlay with no recompiled version, it saves it to
`work/overlays/` and stops; running `setup.ps1` again picks it up.

## Fonts

MHFU's text comes from the PSP's own system fonts (`flash0:/font/*.pgf`),
which are firmware, not part of the game. `setup.ps1` copies them from
PPSSPP's install (`assets\flash0\font`) to `work/root/flash0/font/`; give
another folder with `-Fonts <folder>`. Without them the text is blank.

## Playing in a window

```powershell
.\scripts\run.ps1 -Window            # close the window to stop
```

Keys: W/A/S/D = analog stick, arrows = D-pad, K or Space = Cross, L = Circle,
J = Square, I = Triangle, Q = L, E = R, Enter = Start, V/Backspace = Select.
F3 shows or hides the FPS line. The game runs at its own 30 frames a second
(vblanks follow real time). Controllers (DualSense, DualShock, Xbox) work by
button position; the right stick moves the camera. `MHFU_NO_GAMEPAD=1` ignores them.

The window draws with OpenGL at `-Scale` times the PSP's resolution (default
2). `-Renderer software` uses psprecomp's reference rasteriser instead: slow
(about 9 FPS in the village), but it is the one the GL renderer is checked
against.

When the game asks for text (the character name), a text box opens over the
window. `-Name "Pateeer"` answers it without asking; scripted runs (`-Press`)
answer `Hunter`.

Without a window, `-Press "300:CROSS,420:START"` presses buttons at given game
frames (one controller read each, held 6 frames; `500-560:UP` holds a range)
and `-Snap 60` saves every 60th frame to `work/root/frames/`.

## Reading a run

`work/run.log` ends with a report: how many recompiled calls ran, the last
function reached, firmware calls that are not implemented yet, GE work, and a
framebuffer capture (`work/root/frame.ppm`) once the game sets one. A
`dispatch miss` names an address that was not recompiled.
