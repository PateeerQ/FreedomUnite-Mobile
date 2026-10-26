<p align="center">
  <img src="assets/icon/mhfu.png" width="128" alt="">
</p>

# Monster Hunter: Freedom Unite Recomp

Monster Hunter Freedom Unite (PSP, EU `ULES01213`) statically recompiled to
native code with [psprecomp](https://github.com/sp00nznet/psprecomp): a native
port for **Windows** and **Android**, with PSP-style touch buttons, controllers,
RetroAchievements and ad hoc multiplayer through rooms.

**Maintained by [PateeerQ](https://github.com/PateeerQ).**

> **No game data here.** The ISO, the decrypted EBOOT, the extracted disc and
> the generated C code all stay in `work/` (gitignored). Bring your own dump.
> Not affiliated with or endorsed by Capcom; Monster Hunter is a trademark of Capcom.

## Status

Version **0.3.1**. The game boots, plays its movies, creates and saves a
character, and runs the village (Pokke), the house, the Training School and
quests in 3D at its own 30 fps, or at **60 fps** in the new 60 FPS mode (beta),
with sound, music, keyboard, controllers and touch buttons. A village quest has
been played from start to finish without a crash, and players have hunted
together through a room, on the same network or over the internet, on PC and on
Android (Poco X7 Pro).

### Progress

Rough estimates, as of 2026-10-26. "Recompiled" means the code builds and is
switched in when the game loads it; "tested" means someone has played through it.

| Area | Progress | Notes |
|---|---|---|
| CPU recompilation (Allegrex + VFPU) | ![85%](https://img.shields.io/badge/85%25-brightgreen?style=flat-square) | EBOOT and every overlay recompile; VFPU ops checked against PPSSPP as they turn up |
| Game overlays, recompiled | ![100%](https://img.shields.io/badge/100%25-brightgreen?style=flat-square) | 298 of 298, extracted from DATA.BIN (`tools/re/databin.py`) |
| Game overlays, tested in game | ![83%](https://img.shields.io/badge/83%25-brightgreen?style=flat-square) | most of the game played: menus, village, house, Training School, Gathering Hall, quests on most maps (estimate) |
| System modules (firmware HLE) | ![70%](https://img.shields.io/badge/70%25-yellow?style=flat-square) | threads, I/O, UMD/ISO, display, ctrl, utility dialogs, savedata, libfont, sceMpeg, sceAtrac, sceNetAdhoc, Netconf; missing: encrypted saves, the sound PRX |
| Graphics (GE + GL renderer) | ![70%](https://img.shields.io/badge/70%25-yellow?style=flat-square) | textures, lighting, fog, skinning, clipping, render-to-texture, GL 3.3 and GLES 3.0; missing: stencil, colour test, logic op, mipmaps, culling, DXT, specular, points/lines |
| Audio | ![85%](https://img.shields.io/badge/85%25-brightgreen?style=flat-square) | sound effects, music (ATRAC3/3+ via FFmpeg), movie sound; small gaps while loading |
| Movies | ![90%](https://img.shields.io/badge/90%25-brightgreen?style=flat-square) | H.264 + ATRAC3+ via FFmpeg, own PSMF demuxer |
| Input | ![90%](https://img.shields.io/badge/90%25-brightgreen?style=flat-square) | keyboard, controllers (right stick = camera), touch buttons with a layout editor, swipe camera on Android, text box for the game's keyboard |
| Save data | ![80%](https://img.shields.io/badge/80%25-brightgreen?style=flat-square) | save and load work (unencrypted); on Android a save folder of your choice, kept in sync |
| Multiplayer (ad hoc over rooms) | ![85%](https://img.shields.io/badge/85%25-brightgreen?style=flat-square) | Create/Join Server with passcodes on PC and Android, on the LAN or over the internet relay; room chat, ping, 60 fps rooms; Gathering Hall and quests together, smooth in quests; movement in the hall lags |
| RetroAchievements | ![70%](https://img.shields.io/badge/70%25-yellow?style=flat-square) | sign-in, game identified, unlock toasts, on PC and Android (softcore); not yet checked against a real unlock |
| Gameplay coverage | ![40%](https://img.shields.io/badge/40%25-yellow?style=flat-square) | village life, a full quest, the Gathering Hall together; more maps and monsters untested |
| PC release (launcher) | ![85%](https://img.shields.io/badge/85%25-brightgreen?style=flat-square) | launcher (play, profiles, keys, controllers, achievements, 60 FPS, updates, log, crash report, about); v0.3.1 zip |
| Android port | ![75%](https://img.shields.io/badge/75%25-yellow?style=flat-square) | 30 fps, or 60 with 60 FPS mode; touch buttons, layout editor, drawer menu, multiplayer, achievements, save folder, crash log, updates; v0.3.1 APK |

## Playing

You need your own copy of the game and three files made from it:

1. the disc image: Monster Hunter Freedom Unite (Europe), `ULES01213`, as an `.iso`
2. its decrypted EBOOT: in PPSSPP, Settings → Tools → Developer tools →
   "Dump decrypted EBOOT.BIN on game boot" (`ULES01213_EBOOT.BIN`)
3. the PSP's fonts: the `.pgf` files from PPSSPP's `assets/flash0/font`

### Windows

Unzip `FreedomUniteRecomp-v<version>-windows-x64.zip`, put the fonts in
`work\root\flash0\font`, run `mhfu_launcher.exe`, choose the ISO and the EBOOT
under **Advanced**, then **DEPART** on the Play page.

The launcher checks the install, then starts the game with the chosen profile,
upscaler, output size and fullscreen. It also binds keys, tests and picks
controllers, signs in to RetroAchievements, checks
[the releases](https://github.com/PateeerQ/FreedomUniteRecomp/releases) for a newer
version and shows the game's log. Its settings live in `work/launcher/`.

Keys: W/A/S/D = analog stick, arrows = D-pad, K or Space = Cross, L = Circle,
J = Square, I = Triangle, Q = L, E = R, Enter = Start, V/Backspace = Select,
F3 = FPS, F4 = 60 FPS mode, F5/F6 = volume down/up (also a slider in the
launcher). **Mouse camera**: hold the right mouse button and move the mouse;
F7 turns mouse look on without holding it. The game window's **Options** menu
sets the volume, the mouse camera's sensitivity and mouse look (also sliders in
the launcher). Controllers (DualSense, DualShock, Xbox) work by button position; the
right stick moves the camera.

### Android

Install `FreedomUniteRecomp-v<version>-android-arm64.apk` (arm64, Android 8+).
Put the ISO and the EBOOT in one folder on the phone. The first time, the app
asks for it ("Use this folder"); after that it goes straight into the game.
The fonts are taken from PPSSPP when it is installed (PPSSPP for Android dumps
only the EBOOT); otherwise put the `.pgf` files in that folder too.

- **Touch buttons**: stick, D-pad, face buttons, L/R, Select/Start. Hidden while
  a controller is connected (Virtual gamepad: Automatic / On / Off).
- **Camera**: drag a finger anywhere away from the buttons: sideways turns the
  camera, a longer drag up or down looks up or down.
- **Drawer**: swipe in from the left edge, press Back, or tap the menu button.
  Multiplayer, RetroAchievements, Virtual gamepad, Edit button layout, Button
  opacity, Resolution, 60 FPS (beta), Show FPS, Save folder, Change game files,
  Copy crash log, Check for updates, About, Exit.
- **Edit button layout** moves and resizes every group of buttons.
- **Save folder**: like PPSSPP's memory stick folder. Choose a folder you can
  see (for example `Documents/MHFU`) and the saves are kept there too, so they
  survive Clear data and reinstalling; **Load from folder** brings saves copied
  there (for example from a PC) into the game.
- **Crash log**: after a crash the app offers its log at the next start (Copy /
  Share), for bug reports.

### 60 FPS mode (beta)

The game normally runs at the PSP's 30 fps. With **60 FPS (beta)** on (the
launcher's checkbox or F4 on Windows, the drawer on Android) it runs at 60 at
normal game speed: the community "60 FPS" cheat for `ULES01213` is built into
the recompiled code. Online, a room runs at its slowest player's rate: 60 when
everyone has it on, otherwise 30 for everyone (the chat says who).

### Multiplayer

One player chooses **Create Server** (optionally with a 4-digit passcode); the
others choose **Join Server** and pick the room (or type its code, `MH-4K7Q`).
The list shows each room's players, passcode lock, 30/60 FPS and ping, nearest
first, with a search box on Android.
Then everyone goes online in the game as usual (the Gathering Hall). Rooms work
on the same Wi-Fi or hotspot, between PCs and phones. On PC it is the game
window's **Multiplayer** menu; on Android, the drawer. Rooms can also be made
**over the internet** (Create Server → Internet) through the project's relay
server, so players anywhere can join; nobody forwards a port. Another relay can
be set with `relay=host:port` in `<root>/net.cfg`.

In a room, **Select** opens the room chat (MHFU has none of its own); joins,
leaves and the ping show top-left.

## Building

### Windows

Needs CMake and Visual Studio Build Tools ("Desktop development with C++").
7-Zip is used to extract the ISO if installed; otherwise Windows mounts it.

```powershell
git submodule add https://github.com/sp00nznet/psprecomp psprecomp   # once
git submodule update --init --recursive

.\scripts\setup.ps1 -Iso "<your MHFU EU .iso>" -Eboot "<decrypted EBOOT>"
.\scripts\run.ps1 -Window
```

`setup.ps1` copies the fonts from PPSSPP's install (`assets\flash0\font`) to
`work/root/flash0/font/`; give another folder with `-Fonts <folder>`.

MHFU loads part of its code from `DATA.BIN` while it runs (298 overlays:
menus, stages, monsters). Extract them all once, then let setup recompile them
into `work/gen_ovl/`:

```powershell
python tools\re\databin.py work\ULES01213_EBOOT.BIN work\root\disc\PSP_GAME\USRDIR\DATA.BIN overlays work\overlays
.\scripts\setup.ps1 -Iso "<iso>" -Eboot "<eboot>" -KeepDisc
```

If the host still meets an overlay with no recompiled version, it saves it to
`work/overlays/` and stops; running `setup.ps1` again picks it up.

### Android

Needs Android Studio's SDK, NDK `27.0.12077973` and a JDK 17–21 (Android
Studio's own JBR is 25 now, which this Gradle rejects). FFmpeg for Android is
built once with `scripts/build-ffmpeg-android.sh` (Git Bash). Then, after the
Windows setup has generated the C:

```powershell
$env:JAVA_HOME = "$env:USERPROFILE\.jdks\jbr-21.0.11"
cd android; .\gradlew.bat assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

See [android/README.md](android/README.md).

### Releases

```powershell
.\scripts\release.ps1
```

builds both and writes `release\v<version>\` (gitignored): the Windows zip
(launcher, game, FFmpeg, the Visual C++ runtime, a README) and the APK.
The version is `MHFU_VERSION` in `host/about.h` and `versionName` in
`android/app/build.gradle`; they must match. The icon is drawn by
`tools/make_icon.py`.

Both packages contain the game's code, recompiled from your EBOOT (though no game
files). Keep that in mind before publishing them.

## Layout

| Path | What |
|---|---|
| `psprecomp/` | toolkit, as a git submodule |
| `launcher/` | the PC launcher (`mhfu_launcher.exe`): ready check, profiles, display settings, keyboard and controller setup, RetroAchievements, updates, log, about |
| `android/` | the Android app: Gradle project, `SetupActivity` (game folder), `MhfuActivity` (the game, its dialogs), `RoomPicker` (Join Server), `SaveSync` (save folder), `CrashLog`, `Updater` |
| `host/main.c` | the host: loads the EBOOT, sets up the boot thread, runs, reports |
| `host/window.c` | the Win32 window: OpenGL context, keyboard, FPS overlay (F3), Multiplayer and Help menus |
| `host/window_sdl.c` | the SDL3 window (Android, Linux): touch buttons, layout editor, drawer menu, toasts, text box |
| `host/android.c` | what the Android dialogs call: the lobby and the RetroAchievements sign-in |
| `host/about.h` | name, version, maintainer, for every About page |
| `host/achievements.c` | RetroAchievements through rcheevos (`rc_client`) |
| `host/hle_mhfu.c` | firmware MHFU needs that psprecomp lacks (UMD, Impose, system params, VTimers) |
| `host/libfont.c` | sceLibFont: the PSP's PGF system fonts, which MHFU draws all its text with |
| `host/utility.c` | sceUtility savedata (to `work/root/ms/PSP/SAVEDATA`), message dialogs, the on-screen keyboard |
| `host/input.c` | controller: scripted presses, the window's keys, frame snapshots |
| `host/atrac.c`, `host/mpeg.c` | sceAtrac3plus and sceMpeg, decoding with FFmpeg: music, movies with sound |
| `host/audio.c`, `host/gamepad.c` | sound output and controllers, through SDL3 |
| `host/lobby.c` | "Create Server" / "Join Server": rooms by code, the room's frame rate, settings in `<root>/net.cfg` |
| `host/chat.c`, `host/stats.c` | the room chat (Select) and the FPS / ping line |
| `host/adhoc.c` | sceNet, sceNetAdhoc (PDP/PTP), sceNetAdhocctl, sceWlan and sceUtilityNetconf over the room network |
| `host/net/` | the room network: protocol (`proto.h`), hub, client, sockets/threads for Windows and POSIX |
| `host/overlays.h` | table of recompiled overlays (`work/gen_ovl/overlays.c`, written by setup) |
| `seeds/eboot.txt`, `seeds/ov<hash>.txt` | functions only reached through a pointer built at run time |
| `tools/re/databin.py` | decrypts DATA.BIN as the game does and extracts every overlay |
| `tools/mhfu_ovl.c` | wraps an overlay as an ELF for `allegrexrecomp` |
| `tools/mhfu_netcheck.c` | end-to-end check of the room network without the game |
| `tools/fps60_patch.py` | builds the 60 FPS cheat into the recompiled C (setup runs it) |
| `tools/make_icon.py` | draws the app icon (Android mipmaps, `assets/icon/mhfu.ico`) |
| `patches/psprecomp/` | fixes to psprecomp not upstream yet; `setup.ps1` applies them |
| `scripts/setup.ps1` | builds the tool, extracts the disc, recompiles the EBOOT and overlays, builds the host |
| `scripts/run.ps1` | runs the host and saves `work/run.log` |
| `scripts/harvest.ps1` | runs and re-runs setup until a scripted path loads no new overlay |
| `scripts/release.ps1` | packs the Windows zip and the Android APK |
| `work/` | everything made from the game (never committed) |

## Development runs

```powershell
.\scripts\run.ps1 -Window            # close the window to stop
```

The window draws with OpenGL at `-Scale` times the PSP's resolution (default
2). `-Renderer software` uses psprecomp's reference rasteriser instead: slow
(about 9 FPS in the village), but it is the one the GL renderer is checked
against.

When the game asks for text (the character name), a text box opens over the
window. `-Name "Pateeer"` answers it without asking; scripted runs (`-Press`)
answer `Hunter`. `-Press "300:CROSS,420:START"` presses buttons at given game
frames (`500-560:UP` holds a range) and `-Snap 60` saves every 60th frame to
`work/root/frames/`. `MHFU_NO_GAMEPAD=1` ignores controllers.

`work/run.log` ends with a report: how many recompiled calls ran, the last
function reached, firmware calls that are not implemented yet, GE work, and a
framebuffer capture (`work/root/frame.ppm`) once the game sets one. A
`dispatch miss` names an address that was not recompiled.

## Credits

Maintained by **PateeerQ**. Built on [psprecomp](https://github.com/sp00nznet/psprecomp)
(sp00nznet), [SDL3](https://libsdl.org), [FFmpeg](https://ffmpeg.org) (LGPL 2.1),
[rcheevos](https://github.com/RetroAchievements/rcheevos),
[Dear ImGui](https://github.com/ocornut/imgui), [nlohmann/json](https://github.com/nlohmann/json)
and [stb](https://github.com/nothings/stb). [PPSSPP](https://github.com/hrydgard/ppsspp)'s
source was the reference for the firmware's behaviour, and
[OceanTriRecomp](https://github.com/krisztsa/OceanTriRecomp) for the approach.
