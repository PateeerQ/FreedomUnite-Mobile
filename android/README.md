# Monster Hunter: Freedom Unite Recomp for Android

The app module builds the repository's own `CMakeLists.txt` (the recompiled
game, psprecomp, SDL3, rcheevos, FFmpeg) for `arm64-v8a` into `libmain.so`,
inside SDL's Android activity. It needs the recompiled code in `work/gen` and
`work/gen_ovl`, so run `scripts\setup.ps1` on the PC first, and FFmpeg for
Android once (`scripts/build-ffmpeg-android.sh`, Git Bash).

> The APK contains code recompiled from your own EBOOT. Think before you
> share or upload it.

## Build

1. Android Studio, SDK Manager, SDK Tools: **NDK (Side by side) 27.x** and
   **CMake 3.22.1+**. If your NDK is not 27.0.12077973, put its version in
   `android/gradle.properties` as `mhfu.ndkVersion=27.x.y`.
2. A JDK 17–21 for Gradle. Android Studio's own JBR is JDK 25 now, which this
   Gradle rejects ("Unsupported class file major version 69"); in Android
   Studio set Settings → Build Tools → Gradle → Gradle JDK to a 21.
3. File, Open, choose this `android` folder. Let Gradle sync.
4. Build, Build APK(s), or Run with the phone connected (USB debugging on).
   The first native build compiles the whole recompiled game and takes a while.

From a terminal instead:

```powershell
$env:JAVA_HOME = "$env:USERPROFILE\.jdks\jbr-21.0.11"
cd android; .\gradlew.bat assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

`scripts\release.ps1` builds the release APK (and the Windows zip) into
`release\v<version>\`.

## The game's files

Put the ISO, the decrypted EBOOT (`ULES01213_EBOOT.BIN`) and the PSP's `.pgf`
fonts (loose, or in a `font` folder) in one folder on the phone. When the app
starts without a valid disc it asks for that folder (Android's file manager,
"Use this folder"), keeps access to it and copies the EBOOT and the fonts into
its own folder, `/sdcard/Android/data/com.pateeerq.mhfu/files`. The disc
is read where it is.

They can be pushed there by hand too:

```powershell
$dst = "/sdcard/Android/data/com.pateeerq.mhfu/files"
adb push work\ULES01213_EBOOT.BIN $dst/
adb push "<your MHFU EU .iso>" $dst/
adb push work\root\flash0 $dst/root/flash0      # the PSP's fonts
adb push work\root\ms $dst/root/ms              # optional: your PC saves
```

Optional `args.txt` there holds more arguments, one per line. The log is
`run.log` in the same folder: `adb pull $dst/run.log`.

## In the app

Touch buttons (hidden while a controller is connected, unless set On), and a
drawer from the left edge (swipe, Back, or the menu button): Multiplayer,
RetroAchievements, Virtual gamepad, Edit button layout, Button opacity, Show
FPS, Check for updates, About, Exit.
