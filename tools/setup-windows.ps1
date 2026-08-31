# EmuWire — Windows toolchain setup (no admin required)
#
# Reproduces the pico-sdk build environment on a fresh Windows machine.
# Everything lands under %USERPROFILE%\.pico-sdk, which is the same layout the
# official VS Code Pico extension uses, so the two coexist.
#
#   powershell -ExecutionPolicy Bypass -File setup-windows.ps1
#
# Verified working: SDK 2.3.0, Arm GNU Toolchain 14.2.Rel1, CMake 4.4.2,
# Ninja 1.13.2, Python 3.12.10, picotool 2.3.0, OpenOCD 0.12.0+dev.

$ErrorActionPreference = 'Stop'

$SDK_VER      = '2.3.0'
$TOOLS_TAG    = 'v2.3.0-1'      # raspberrypi/pico-sdk-tools release
$GCC_VER      = '14.2.rel1'
$OPENOCD_VER  = '0.12.0+dev'
$ROOT         = Join-Path $env:USERPROFILE '.pico-sdk'
$TMP          = Join-Path $env:TEMP 'emuwire-setup'

New-Item -ItemType Directory -Force -Path $ROOT, $TMP | Out-Null

function Expand-Zip($zip, $dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    # Windows' bsdtar handles zip and is much faster than Expand-Archive.
    & "$env:SystemRoot\System32\tar.exe" -xf $zip -C $dest
    if ($LASTEXITCODE -ne 0) { throw "extract failed: $zip" }
}

# --- 1. Host tools via winget (user scope, no admin) ------------------------
# The Arm toolchain has no user-scope installer, so it is fetched as a zip below.
Write-Host '==> host tools (cmake, ninja, python)' -ForegroundColor Cyan
foreach ($pkg in 'Kitware.CMake', 'Ninja-build.Ninja', 'Python.Python.3.12') {
    winget install $pkg --exact --source winget --scope user `
        --accept-package-agreements --accept-source-agreements --disable-interactivity
}

# --- 2. Arm GNU toolchain ---------------------------------------------------
Write-Host '==> arm-none-eabi gcc (305 MB)' -ForegroundColor Cyan
$gccDir = Join-Path $ROOT "toolchain\$GCC_VER"
if (-not (Test-Path (Join-Path $gccDir 'bin\arm-none-eabi-gcc.exe'))) {
    $zip = Join-Path $TMP 'armgcc.zip'
    $url = "https://developer.arm.com/-/media/Files/downloads/gnu/$GCC_VER/binrel/arm-gnu-toolchain-$GCC_VER-mingw-w64-i686-arm-none-eabi.zip"
    Invoke-WebRequest -Uri $url -OutFile $zip
    # This archive has no top-level folder; extract straight into the versioned dir.
    Expand-Zip $zip $gccDir
}

# --- 3. pico-sdk ------------------------------------------------------------
Write-Host '==> pico-sdk' -ForegroundColor Cyan
$sdkDir = Join-Path $ROOT 'sdk'
if (-not (Test-Path $sdkDir)) {
    git clone -b master --depth 1 https://github.com/raspberrypi/pico-sdk.git $sdkDir
}
Push-Location $sdkDir
git submodule update --init --depth 1 lib/tinyusb   # needed for USB CDC transport
Pop-Location

# --- 4. Prebuilt pioasm / picotool / OpenOCD --------------------------------
# Building these from source needs a host C++ compiler, which this machine has
# no admin rights to install. Raspberry Pi ship prebuilt binaries matching each
# SDK release, and that is the supported Windows path.
Write-Host '==> pioasm, picotool, openocd' -ForegroundColor Cyan
$base = "https://github.com/raspberrypi/pico-sdk-tools/releases/download/$TOOLS_TAG"
$assets = @{
    "pico-sdk-tools-$SDK_VER-x64-win.zip" = Join-Path $ROOT "tools\$SDK_VER"
    "picotool-$SDK_VER-x64-win.zip"       = Join-Path $ROOT "picotool\$SDK_VER"
    "openocd-$OPENOCD_VER-x64-win.zip"    = Join-Path $ROOT "openocd\$OPENOCD_VER"
}
foreach ($a in $assets.Keys) {
    $zip = Join-Path $TMP $a
    Invoke-WebRequest -Uri "$base/$a" -OutFile $zip
    Expand-Zip $zip $assets[$a]
}

# --- 5. Persistent environment ---------------------------------------------
Write-Host '==> environment' -ForegroundColor Cyan
[Environment]::SetEnvironmentVariable('PICO_SDK_PATH', $sdkDir, 'User')
[Environment]::SetEnvironmentVariable('PICO_TOOLCHAIN_PATH', $gccDir, 'User')

$path = [Environment]::GetEnvironmentVariable('Path', 'User')
foreach ($d in @(
    (Join-Path $gccDir 'bin'),
    (Join-Path $ROOT "tools\$SDK_VER\pioasm"),
    (Join-Path $ROOT "picotool\$SDK_VER\picotool"),
    (Join-Path $ROOT "openocd\$OPENOCD_VER")
)) {
    if ($path -notlike "*$d*") { $path = $path.TrimEnd(';') + ';' + $d }
}
[Environment]::SetEnvironmentVariable('Path', $path, 'User')

Write-Host ''
Write-Host 'Done. Open a NEW terminal, then configure a build with:' -ForegroundColor Green
Write-Host @"
  cmake -S . -B build -G Ninja ``
    -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350 ``
    -Dpioasm_DIR="$ROOT/tools/$SDK_VER/pioasm" ``
    -Dpicotool_DIR="$ROOT/picotool/$SDK_VER/picotool"
"@
