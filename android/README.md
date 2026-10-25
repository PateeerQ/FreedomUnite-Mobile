# FreedomUniteRecomp for Android

The app module builds the repository's own `CMakeLists.txt` (the recompiled
game, psprecomp, SDL3) for `arm64-v8a` into `libmain.so`, inside SDL's Android
activity. It needs the recompiled code in `work/gen` and `work/gen_ovl`, so run
`scripts\setup.ps1` on the PC first.

> The APK contains code recompiled from your own EBOOT. Keep it on your own
> devices; never share or upload it.

## Build

1. Android Studio, SDK Manager, SDK Tools: **NDK (Side by side) 27.x** and
   **CMake 3.22.1+**. If your NDK is not 27.0.12077973, put its version in
   `android/gradle.properties` as `mhfu.ndkVersion=27.x.y`.
2. File, Open, choose this `android` folder. Let Gradle sync.
3. Build, Build APK(s), or Run with the phone connected (USB debugging on).
   The first native build compiles the whole recompiled game and takes a while.

From a terminal instead: `android\gradlew.bat -p android assembleDebug`, then
`adb install -r android\app\build\outputs\apk\debug\app-debug.apk`.

## The game's files

Install and start the app once, then copy your own files into its folder
(`/sdcard/Android/data/com.pateeerq.mhfu/files`):

```powershell
$dst = "/sdcard/Android/data/com.pateeerq.mhfu/files"
adb push work\ULES01213_EBOOT.BIN $dst/
adb push "<your MHFU EU .iso>" $dst/
adb push work\root\flash0 $dst/root/flash0      # the PSP's fonts
adb push work\root\ms $dst/root/ms              # optional: your PC saves
```

Optional `args.txt` there holds more arguments, one per line (for example
`--net-join` and a room code on the next line). The log is `run.log` in the
same folder: `adb pull $dst/run.log`.

For now the game is played with a controller (Bluetooth or USB); the touch
buttons come next.
