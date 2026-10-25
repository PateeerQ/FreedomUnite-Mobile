Monster Hunter: Freedom Unite Recomp -- Windows
Maintained by PateeerQ -- https://github.com/PateeerQ/FreedomUniteRecomp

A native port of Monster Hunter Freedom Unite (PSP, Europe, ULES01213): the
game's own code, statically recompiled, on a reimplementation of the PSP's
firmware. No game files come with it: you need your own copy.

WHAT YOU NEED
  1. Your dump of the game: Monster Hunter Freedom Unite (Europe), ULES01213, as an .iso
  2. Its decrypted EBOOT: in PPSSPP, Settings > Tools > Developer tools >
     "Dump decrypted EBOOT.BIN" while the game runs (ULES01213_EBOOT.BIN)
  3. The PSP's fonts: the .pgf files from PPSSPP's assets\flash0\font,
     copied into work\root\flash0\font here

START
  Run mhfu_launcher.exe. Under Advanced choose the ISO and the EBOOT; the Play
  page checks everything, then DEPART.

  Keyboard: W/A/S/D move, arrows = D-pad (camera), K/Space = Cross, L = Circle,
  J = Square, I = Triangle, Q/E = L/R, Enter = Start, V = Select (change them on
  the Keyboard page). DualSense, DualShock and Xbox controllers work too.

  Multiplayer: in the game's window, Multiplayer > Create Server on one PC
  (Internet: players anywhere, or This network: same Wi-Fi), Join Server on
  the others and pick the room (or type its code, MH-XXXX), then go online in
  the game (Gathering Hall). PC and Android play together. No port forwarding.

  Chat: while in a room, press SELECT (keyboard V/Backspace, controller
  Create/Share), type, Enter. Lines show top-left; joins and leaves too.

  Saves are in work\root\ms\PSP\SAVEDATA (or work\profiles\<name>).

FILES
  mhfu_launcher.exe   the launcher
  mhfu_host.exe       the game
  *.dll               FFmpeg 7.1 (LGPL 2.1), for movies and music

NOTICE
  Not affiliated with or endorsed by Capcom. Monster Hunter is a trademark of
  Capcom. FFmpeg is licensed under the LGPL 2.1 (https://ffmpeg.org/legal.html);
  these DLLs are the unmodified BtbN LGPL shared build, n7.1.1.
