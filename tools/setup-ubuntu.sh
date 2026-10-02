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
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$ROOT"

say() { printf '\n\033[36m==> %s\033[0m\n' "$1"; }

# --- 1. Host packages -------------------------------------------------------
# Unlike a bare Windows box, Ubuntu has a host C++ compiler, so building
# picotool from source is possible. We still take the prebuilt binaries below
# because they are version-matched to the SDK.
# libusb + pkg-config are needed for picotool to talk to a board over USB.
# sigrok-firmware-fx2lafw: the cheap FX2LP analyzer has no firmware of its
# own, and sigrok uploads this on every plug-in. Without it the analyzer is
# simply "not found". picocom is the serial terminal for both boards.
say "apt packages"
# Ubuntu's automatic updates hold the package lock for minutes at a time,
# often right after boot. Wait for it instead of failing on the first line.
APT_WAIT=(-o DPkg::Lock::Timeout=600)
sudo apt-get "${APT_WAIT[@]}" update
sudo apt-get "${APT_WAIT[@]}" install -y \
    build-essential cmake ninja-build git python3 python3-venv \
    libusb-1.0-0-dev pkg-config \
    pulseview sigrok-cli sigrok-firmware-fx2lafw \
    picocom

# --- 2. brltty: the trap ----------------------------------------------------
# Ubuntu's braille daemon claims /dev/ttyACM* devices. Your Pico enumerates,
# then disappears a second later. This is the most common Ubuntu embedded-dev
# trap and it looks exactly like faulty hardware or a bad cable.
say 'removing brltty (it steals /dev/ttyACM*)'
# Only skip the removal when brltty is not installed. "|| true" would also
# swallow a failed removal, leaving the trap armed with no sign of it.
if dpkg -s brltty >/dev/null 2>&1; then
    sudo apt-get "${APT_WAIT[@]}" remove -y brltty
fi

# --- 3. Device permissions --------------------------------------------------
# USB CDC *is* this product's transport, so serial access without sudo is not
# optional. Group membership needs a full logout, not just a new shell.
say 'serial + udev permissions'
sudo usermod -aG dialout "$USER"

# picotool: lets BOOTSEL-mode boards be accessed without sudo. Taken from
# the picotool tag that matches the SDK, not master: master renamed this file
# once already (99- to 60-), and a moving branch is how that breaks a setup.
sudo curl -fsSL -o /etc/udev/rules.d/60-picotool.rules \
    "https://raw.githubusercontent.com/raspberrypi/picotool/$SDK_VER/udev/60-picotool.rules"

# sigrok: nothing to download. The libsigrok package pulled in by pulseview
# above installs its own rules into /lib/udev/rules.d, including the one that
# gives the logged-in user the FX2LP analyzer. A second copy from upstream
# master would only shadow the version the distro tested.

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
# Clone the release TAG, not master. Pinning the toolchain while tracking a
# moving branch would make "$SDK_VER" a claim rather than a fact — and the
# prebuilt pioasm and picotool fetched below are versioned WITH the SDK, so
# pairing them against a drifted master is how you get a mismatch that only
# surfaces at link time.
if [ ! -d "$ROOT/sdk" ]; then
    git clone -b "$SDK_VER" --depth 1 https://github.com/raspberrypi/pico-sdk.git "$ROOT/sdk"
fi
git -C "$ROOT/sdk" submodule update --init --depth 1 lib/tinyusb   # USB CDC transport

# Assert, don't assume. This also catches an SDK left behind by an earlier
# run of this script that cloned master.
sdk_have=$(sed -n 's/.*set(PICO_SDK_VERSION_\(MAJOR\|MINOR\|REVISION\) \([0-9]*\)).*/\2/p' \
           "$ROOT/sdk/pico_sdk_version.cmake" | paste -sd. -)
if [ "$sdk_have" != "$SDK_VER" ]; then
    echo "ERROR: $ROOT/sdk contains pico-sdk $sdk_have, but this script targets $SDK_VER." >&2
    echo "       Delete that directory and re-run, or change SDK_VER at the top." >&2
    exit 1
fi
echo "    pico-sdk $sdk_have verified"

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
BEGIN='# >>> EmuWire pico-sdk >>>'
END='# <<< EmuWire pico-sdk <<<'

# Rewrite the block on every run rather than skipping it when present, so a
# re-run repairs whatever an older version of this script wrote. That
# includes the first version's single-marker block, which baked a frozen
# copy of PATH into .bashrc.
sed -i "/^$BEGIN\$/,/^$END\$/d" "$PROFILE"
sed -i '/^# --- EmuWire pico-sdk ---$/,+3d' "$PROFILE"

# \$PATH is escaped so .bashrc gets the variable, not today's value of it.
# Unescaped, every later change to PATH made above this block would be
# silently overwritten each time a shell starts.
cat >> "$PROFILE" <<EOF
$BEGIN
export PICO_SDK_PATH="$ROOT/sdk"
export PICO_TOOLCHAIN_PATH="$GCC_DIR"
export PATH="\$PATH:$GCC_DIR/bin:$ROOT/tools/$SDK_VER/pioasm:$ROOT/picotool/$SDK_VER/picotool:$ROOT/openocd/$OPENOCD_VER"
$END
EOF

cat <<EOF

Done. Now: LOG OUT AND BACK IN. The dialout group only applies to a new
session; a new terminal is not enough.

Then check, no hardware needed:

  groups                           # includes dialout
  $GCC_DIR/bin/arm-none-eabi-gcc --version   # 14.2.1
                                   # (by full path: an older apt arm-none-eabi-gcc
                                   #  may come first on PATH; builds use this one
                                   #  anyway, via PICO_TOOLCHAIN_PATH)
  picotool version                 # picotool v2.3.0
  command -v pioasm                # a path under $ROOT

A real build, from this repo:

  cmake -S $REPO/tests/rig -B $REPO/tests/rig/build -G Ninja \\
    -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350 \\
    -Dpioasm_DIR=$ROOT/tools/$SDK_VER/pioasm \\
    -Dpicotool_DIR=$ROOT/picotool/$SDK_VER/picotool
  cmake --build $REPO/tests/rig/build

With hardware attached:

  picotool info                    # a Pico held in BOOTSEL, WITHOUT sudo
  sigrok-cli --scan                # the analyzer, listed as fx2lafw
  openocd -s $ROOT/openocd/$OPENOCD_VER/scripts \\
    -f interface/cmsis-dap.cfg -f target/rp2350.cfg      # via the Debug Probe

OpenOCD needs -s: the prebuilt keeps its scripts next to the binary, where
it does not look by default.
EOF
