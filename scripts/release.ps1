# Packs a release into release\v<version>\ (gitignored):
#   FreedomUniteRecomp-v<version>-windows-x64.zip   launcher, game, FFmpeg, README
#   FreedomUniteRecomp-v<version>-android-arm64.apk the release APK
#
#   .\scripts\release.ps1                   # builds first (Release + assembleRelease)
#   .\scripts\release.ps1 -NoBuild
#
# Both contain the game's code recompiled from your EBOOT (mhfu_host.exe,
# libmain.so). No game files go in (no ISO, EBOOT, fonts, saves), but
# think before publishing them: see the README's notice.
param(
    [switch]$NoBuild
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$repo\host\about.h" -Pattern '#define MHFU_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
$gradleVersion = (Select-String -Path "$repo\android\app\build.gradle" -Pattern "versionName '([^']+)'").Matches[0].Groups[1].Value
if ($version -ne $gradleVersion) { throw "host/about.h says $version, android/app/build.gradle says ${gradleVersion}: make them the same" }

if (-not $NoBuild) {
    cmake --build "$repo\build" --config Release --target mhfu_host mhfu_launcher
    if ($LASTEXITCODE) { throw "the Windows build failed" }
    # AGP 8.7's Gradle runs on JDK 17-21; Android Studio's own JBR became 25. A JBR 21 under ~/.jdks first.
    $jdk = Get-ChildItem "$env:USERPROFILE\.jdks\jbr-21*", "$env:USERPROFILE\.jdks\*-21*" -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path "$($_.FullName)\bin\java.exe" } | Select-Object -First 1
    $env:JAVA_HOME = if ($jdk) { $jdk.FullName } else { "C:\Program Files\Android\Android Studio\jbr" }
    # Gradle's warnings go to stderr, which Windows PowerShell takes for an error
    # under ErrorActionPreference Stop: relaxed while it runs, the exit code decides.
    $ErrorActionPreference = "Continue"
    & "$repo\android\gradlew.bat" -p "$repo\android" assembleRelease --console=plain 2>&1 | ForEach-Object { "$_" }
    $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE) { throw "the Android build failed" }
}

$out = "$repo\release\v$version"
New-Item -ItemType Directory -Force $out | Out-Null
$name = "FreedomUniteRecomp-v$version"

# ---- Windows ----
$stage = "$out\$name-windows-x64"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\work\root\flash0\font" | Out-Null
$bin = "$repo\build\Release"
foreach ($f in "mhfu_launcher.exe", "mhfu_host.exe", "mhfu.png") { Copy-Item "$bin\$f" $stage }
Get-ChildItem "$bin\*.dll" | Copy-Item -Destination $stage
# The Visual C++ runtime, beside the exes (app-local), so no redistributable has to be installed.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "no Visual C++ runtime in $vs\VC\Redist" }
foreach ($f in "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") { Copy-Item "$($crt.FullName)\$f" $stage }
Copy-Item "$repo\scripts\release-readme.txt" "$stage\README.txt"
Set-Content -Encoding utf8 "$stage\work\root\flash0\font\PUT_FONTS_HERE.txt" `
    "Copy the PSP's .pgf fonts here (PPSSPP has them in assets\flash0\font). Without them all text is blank."
$zip = "$out\$name-windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item -Recurse -Force $stage

# ---- Android ----
$apk = "$repo\android\app\build\outputs\apk\release\app-release.apk"
if (-not (Test-Path $apk)) { throw "no release APK: build it (without -NoBuild)" }
Copy-Item $apk "$out\$name-android-arm64.apk" -Force

Get-ChildItem $out | Select-Object Name, @{ n = "MB"; e = { [math]::Round($_.Length / 1MB, 1) } }
