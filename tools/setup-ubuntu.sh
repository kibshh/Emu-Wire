#!/usr/bin/env bash
# EmuWire — Ubuntu bench machine setup
#
# The bench machine: flashes, debugs and captures. It needs everything
# a build machine has, plus the parts that touch hardware.
#
#   chmod +x setup-ubuntu.sh && ./setup-ubuntu.sh
#
# Versions are pinned to match the build machine exactly. Same compiler
# version means identical binaries and no "works on my machine" bugs.
#
# NOT YET TESTED on a clean Ubuntu install. Read it before running.

set -euo pipefail

SDK_VER='2.3.0'
TOOLS_TAG='v2.3.0-1'
GCC_VER='14.2.rel1'
OPENOCD_VER='0.12.0+dev'
ROOT="$HOME/.pico-sdk"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$ROOT"

say() { printf '\n\033[36m==> %s\033[0m\n' "$1"; }

# --- 1. Host packages -------------------------------------------------------
# Unlike a bare Windows box, Ubuntu has a host C++ compiler, so building
# picotool from source is possible. We still take the prebuilt binaries below
# because they are version-matched to the SDK.
# libusb + pkg-config are needed for picotool to talk to a board over USB.
say "apt packages"
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build git python3 python3-venv \
    libusb-1.0-0-dev pkg-config \
    pulseview sigrok-cli

# --- 2. brltty: the trap ----------------------------------------------------
# Ubuntu's braille daemon claims /dev/ttyACM* devices. Your Pico enumerates,
# then disappears a second later. This is the most common Ubuntu embedded-dev
# trap and it looks exactly like faulty hardware or a bad cable.
say 'removing brltty (it steals /dev/ttyACM*)'
sudo apt-get remove -y brltty || true

# --- 3. Device permissions --------------------------------------------------
# USB CDC *is* this product's transport, so serial access without sudo is not
# optional. Group membership needs a full logout, not just a new shell.
say 'serial + udev permissions'
sudo usermod -aG dialout "$USER"

# picotool: lets BOOTSEL-mode boards be accessed without sudo.
sudo curl -fsSL -o /etc/udev/rules.d/99-picotool.rules \
    https://raw.githubusercontent.com/raspberrypi/picotool/master/udev/99-picotool.rules

# sigrok: same for the FX2LP logic analyzer clone. Without this PulseView
# reports "no devices found" rather than a permission error.
sudo curl -fsSL -o /etc/udev/rules.d/60-libsigrok.rules \
    https://raw.githubusercontent.com/sigrokproject/libsigrok/master/contrib/60-libsigrok.rules

sudo udevadm control --reload-rules && sudo udevadm trigger

# --- 4. Arm GNU toolchain ---------------------------------------------------
# Deliberately NOT apt's gcc-arm-none-eabi: that is a different build from the
# your other machines'. Matching versions keeps the binaries byte-comparable.
say "arm-none-eabi gcc $GCC_VER"
GCC_DIR="$ROOT/toolchain/$GCC_VER"
if [ ! -x "$GCC_DIR/bin/arm-none-eabi-gcc" ]; then
    ARCH="$(uname -m)"   # x86_64 or aarch64
    URL="https://developer.arm.com/-/media/Files/downloads/gnu/$GCC_VER/binrel/arm-gnu-toolchain-$GCC_VER-${ARCH}-arm-none-eabi.tar.xz"
    curl -fL --progress-bar -o "$TMP/armgcc.tar.xz" "$URL"
    mkdir -p "$GCC_DIR"
    tar -xf "$TMP/armgcc.tar.xz" -C "$GCC_DIR" --strip-components=1
fi

# --- 5. pico-sdk ------------------------------------------------------------
say "pico-sdk $SDK_VER"
if [ ! -d "$ROOT/sdk" ]; then
    git clone -b master --depth 1 https://github.com/raspberrypi/pico-sdk.git "$ROOT/sdk"
fi
git -C "$ROOT/sdk" submodule update --init --depth 1 lib/tinyusb   # USB CDC transport

# --- 6. pioasm, picotool, OpenOCD ------------------------------------------
# Ubuntu's packaged openocd has no rp2350.cfg. This build does.
say 'pioasm, picotool, openocd (prebuilt, version-matched)'
BASE="https://github.com/raspberrypi/pico-sdk-tools/releases/download/$TOOLS_TAG"
LARCH="$(uname -m)"   # pico-sdk-tools uses x86_64 / aarch64
fetch() {  # $1 = asset, $2 = dest dir
    mkdir -p "$2"
    curl -fL --progress-bar -o "$TMP/$1" "$BASE/$1"
    tar -xf "$TMP/$1" -C "$2"
}
fetch "pico-sdk-tools-$SDK_VER-$LARCH-lin.tar.gz" "$ROOT/tools/$SDK_VER"
fetch "picotool-$SDK_VER-$LARCH-lin.tar.gz"       "$ROOT/picotool/$SDK_VER"
fetch "openocd-$OPENOCD_VER-$LARCH-lin.tar.gz"    "$ROOT/openocd/$OPENOCD_VER"

# --- 7. Environment ---------------------------------------------------------
say 'environment'
PROFILE="$HOME/.bashrc"
MARK='# --- EmuWire pico-sdk ---'
if ! grep -qF "$MARK" "$PROFILE"; then
cat >> "$PROFILE" <<EOF

$MARK
export PICO_SDK_PATH="$ROOT/sdk"
export PICO_TOOLCHAIN_PATH="$GCC_DIR"
export PATH="$PATH:$GCC_DIR/bin:$ROOT/tools/$SDK_VER/pioasm:$ROOT/picotool/$SDK_VER/picotool:$ROOT/openocd/$OPENOCD_VER"
EOF
fi

cat <<EOF

Done. Now: LOG OUT AND BACK IN — the dialout group needs a fresh session,
a new terminal is not enough.

Then verify all four:

  1. cmake -S . -B build -G Ninja -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350 \\
       -Dpioasm_DIR="$ROOT/tools/$SDK_VER/pioasm" \\
       -Dpicotool_DIR="$ROOT/picotool/$SDK_VER/picotool"
  2. picotool info                       # a Pico, WITHOUT sudo
  3. openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg
  4. sigrok-cli --scan                   # finds the analyzer
EOF
