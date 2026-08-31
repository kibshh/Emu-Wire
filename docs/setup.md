# Setting up a development machine

Everything you need to build EmuWire's firmware, and — if you have hardware — to flash and debug it.

## Two kinds of machine

You don't need a Pico to work on this project. Most of the code is testable without one.

| | **Build machine** | **Bench machine** |
|---|---|---|
| Compile firmware, produce a `.uf2` | ✅ | ✅ |
| Assemble `.pio` files, check the instruction budget | ✅ | ✅ |
| Run the SDK and its unit tests | ✅ | ✅ |
| Flash a board | — | ✅ |
| Debug over SWD | — | ✅ |
| Capture a bus with a logic analyzer | — | ✅ |

A build machine is enough for the host SDK, the wire protocol, manifests, and writing PIO programs. You only need a bench machine to observe what the hardware actually does.

**Fast path:** run [`tools/setup-windows.ps1`](../tools/setup-windows.ps1) or [`tools/setup-ubuntu.sh`](../tools/setup-ubuntu.sh) and skip to [Verify](#verify). The rest of this page is what those scripts do, and why.

## Versions

Pin these. If you work across two machines, use the **same versions on both** — matching compilers mean identical binaries and remove a whole category of "builds on my machine" confusion.

| | Version |
|---|---|
| pico-sdk | 2.3.0 |
| Arm GNU Toolchain | 14.2.Rel1 |
| picotool / pioasm | 2.3.0 |
| OpenOCD | 0.12.0+dev (from `pico-sdk-tools`) |
| CMake | ≥ 3.13 |
| Python | ≥ 3.9 |

Everything installs under `~/.pico-sdk` (`%USERPROFILE%\.pico-sdk` on Windows) — the same layout the official VS Code Pico extension uses, so the two coexist.

---

## Windows

No administrator rights needed.

### 1. Host tools

```powershell
winget install Kitware.CMake      --exact --source winget --scope user
winget install Ninja-build.Ninja  --exact --source winget --scope user
winget install Python.Python.3.12 --exact --source winget --scope user
```

### 2. Arm toolchain

The Arm toolchain has no user-scope installer, so take the portable zip. Download [`arm-gnu-toolchain-14.2.rel1-mingw-w64-i686-arm-none-eabi.zip`](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (~305 MB) and extract it to `%USERPROFILE%\.pico-sdk\toolchain\14.2.rel1`.

> This archive has **no top-level folder** — extract straight into the versioned directory, or you'll end up with `bin\` scattered a level too high.

### 3. The SDK

```powershell
git clone -b master --depth 1 https://github.com/raspberrypi/pico-sdk.git $env:USERPROFILE\.pico-sdk\sdk
cd $env:USERPROFILE\.pico-sdk\sdk
git submodule update --init --depth 1 lib/tinyusb
```

`tinyusb` is not optional — USB CDC is how the host SDK talks to the board.

### 4. pioasm and picotool

Building these from source needs a host C++ compiler (MSVC or MinGW). If you don't have one, use Raspberry Pi's prebuilt binaries — this is the supported Windows path.

From [`pico-sdk-tools` v2.3.0-1](https://github.com/raspberrypi/pico-sdk-tools/releases/tag/v2.3.0-1), download and extract:

| Asset | Extract to |
|---|---|
| `pico-sdk-tools-2.3.0-x64-win.zip` | `.pico-sdk\tools\2.3.0` |
| `picotool-2.3.0-x64-win.zip` | `.pico-sdk\picotool\2.3.0` |
| `openocd-0.12.0+dev-x64-win.zip` | `.pico-sdk\openocd\0.12.0+dev` |

**`picotool` is required even if you never flash anything.** SDK 2.x generates the `.uf2` with it — without picotool the build stops at an `.elf`.

### 5. Environment

```powershell
[Environment]::SetEnvironmentVariable('PICO_SDK_PATH',       "$env:USERPROFILE\.pico-sdk\sdk", 'User')
[Environment]::SetEnvironmentVariable('PICO_TOOLCHAIN_PATH', "$env:USERPROFILE\.pico-sdk\toolchain\14.2.rel1", 'User')
```

Add to your user `Path`:

```
%USERPROFILE%\.pico-sdk\toolchain\14.2.rel1\bin
%USERPROFILE%\.pico-sdk\tools\2.3.0\pioasm
%USERPROFILE%\.pico-sdk\picotool\2.3.0\picotool
%USERPROFILE%\.pico-sdk\openocd\0.12.0+dev
```

> The OpenOCD entry has **no `bin\` subdirectory** — that build unpacks its
> binary and `scripts/` folder side by side. OpenOCD locates its `.cfg` files
> relative to its own executable, so `-f target/rp2350.cfg` resolves from any
> working directory once it's on `PATH`.

Open a **new** terminal afterwards.

### 6. Flashing from Windows (bench machines only)

If `picotool` reports *"No accessible RP2040/RP2350 devices in BOOTSEL mode were found"*, Windows hasn't bound the boot interface to WinUSB. Fix it with [Zadig](https://zadig.akeo.ie/): select the **RP2 Boot** device, install **WinUSB**. Drag-and-drop `.uf2` flashing works regardless.

---

## Linux (Ubuntu)

### 1. Packages

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git python3 python3-venv \
                    libusb-1.0-0-dev pkg-config
```

Bench machines also want the logic-analyzer software:

```bash
sudo apt install -y pulseview sigrok-cli
```

### 2. Remove brltty

```bash
sudo apt remove -y brltty
```

Ubuntu's braille daemon claims `/dev/ttyACM*` devices. Your board enumerates, then disappears a second later. **This is the most common Linux trap in embedded work** and it looks exactly like a faulty cable — which is especially confusing when a faulty cable is also a real possibility.

### 3. Permissions (bench machines)

USB CDC is this project's transport, so serial access without `sudo` isn't optional.

```bash
sudo usermod -aG dialout $USER
```

Then install udev rules so `picotool`, OpenOCD and PulseView can reach devices as a normal user:

```bash
sudo curl -fsSL -o /etc/udev/rules.d/99-picotool.rules \
  https://raw.githubusercontent.com/raspberrypi/picotool/master/udev/99-picotool.rules
sudo curl -fsSL -o /etc/udev/rules.d/60-libsigrok.rules \
  https://raw.githubusercontent.com/sigrokproject/libsigrok/master/contrib/60-libsigrok.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

> **Log out and back in.** Group membership doesn't apply to a new terminal — it needs a new session. Skipping this is why "I added myself to dialout and it still says permission denied" happens.

Without these rules, tools report *"no devices found"* rather than a permission error, which sends you looking for hardware faults that aren't there.

### 4. Arm toolchain

Use the official tarball, **not** `apt install gcc-arm-none-eabi`. The distro package is a different build; matching your other machines is worth more than the convenience.

```bash
mkdir -p ~/.pico-sdk/toolchain/14.2.rel1
curl -fL -o /tmp/armgcc.tar.xz \
  "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-$(uname -m)-arm-none-eabi.tar.xz"
tar -xf /tmp/armgcc.tar.xz -C ~/.pico-sdk/toolchain/14.2.rel1 --strip-components=1
```

### 5. SDK and tools

```bash
git clone -b master --depth 1 https://github.com/raspberrypi/pico-sdk.git ~/.pico-sdk/sdk
git -C ~/.pico-sdk/sdk submodule update --init --depth 1 lib/tinyusb

BASE=https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.0-1
A=$(uname -m)
mkdir -p ~/.pico-sdk/{tools/2.3.0,picotool/2.3.0,openocd/0.12.0+dev}
curl -fL "$BASE/pico-sdk-tools-2.3.0-$A-lin.tar.gz" | tar -xz -C ~/.pico-sdk/tools/2.3.0
curl -fL "$BASE/picotool-2.3.0-$A-lin.tar.gz"       | tar -xz -C ~/.pico-sdk/picotool/2.3.0
curl -fL "$BASE/openocd-0.12.0+dev-$A-lin.tar.gz"   | tar -xz -C ~/.pico-sdk/openocd/0.12.0+dev
```

**Don't use `apt install openocd`** — the packaged build has no `rp2350.cfg` and cannot talk to an RP2350. Check with `ls ~/.pico-sdk/openocd/*/scripts/target/rp2350.cfg`.

### 6. Environment

Append to `~/.bashrc`:

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14.2.rel1"
export PATH="$PATH:$PICO_TOOLCHAIN_PATH/bin:$HOME/.pico-sdk/tools/2.3.0/pioasm:$HOME/.pico-sdk/picotool/2.3.0/picotool:$HOME/.pico-sdk/openocd/0.12.0+dev"
```

---

## Verify

### Every machine

```bash
arm-none-eabi-gcc --version    # 14.2.Rel1
cmake --version                # >= 3.13
ninja --version
picotool version               # 2.3.0
```

Then build something. Any project configures the same way — note the two `_DIR` flags, which point CMake at the prebuilt tools:

```bash
cmake -S . -B build -G Ninja \
  -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350 \
  -Dpioasm_DIR="$HOME/.pico-sdk/tools/2.3.0/pioasm" \
  -Dpicotool_DIR="$HOME/.pico-sdk/picotool/2.3.0/picotool"
cmake --build build
```

Confirm you built for the right chip — it's easy to produce a silently-valid RP2040 binary:

```console
$ picotool info -a build/blink/blink.uf2
File blink.uf2 family ID 'rp2350-arm-s':
 target chip:  RP2350
 pico_board:   pico2
```

If that says `rp2040`, your `-DPICO_BOARD` / `-DPICO_PLATFORM` didn't take.

### Bench machines only

```bash
picotool info                                              # reads a board, no sudo
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg    # Debug Probe connects
sigrok-cli --scan                                          # analyzer found
```

---

## Working without hardware

Two things are worth knowing if you're on a build machine.

**`pioasm` runs on the host.** PIO programs are limited to **32 instructions per PIO block**, shared by that block's four state machines — the tightest constraint in the firmware. You can check it without a board:

```bash
pioasm -o c-sdk firmware/pio/i2c_slave.pio /tmp/out.h
grep _wrap /tmp/out.h
```

So a PIO program can be written *and* budget-checked anywhere. Only its electrical behaviour needs a bench.

**The host SDK is fully testable.** `pytest sdk/tests` needs no board; the integration suite under `tests/` skips automatically when no hardware is attached.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| `No CMAKE_CXX_COMPILER could be found` | No **host** compiler for building pioasm/picotool. Pass `-Dpioasm_DIR=` and `-Dpicotool_DIR=` to use prebuilt binaries instead. |
| Build stops at `.elf`, no `.uf2` | picotool missing. SDK 2.x generates the UF2 with it. |
| Board enumerates then vanishes (Linux) | `brltty`. Remove it. |
| `Permission denied` on `/dev/ttyACM0` | Not in `dialout`, or you didn't log out after joining. |
| picotool/PulseView: "no devices found" | Missing udev rules (Linux) or WinUSB binding (Windows) — a permissions problem wearing a hardware disguise. |
| OpenOCD: `target/rp2350.cfg not found` | Distro OpenOCD. Use the `pico-sdk-tools` build. |
| Board powers up, LED works, never enumerates | Charge-only USB cable. Very common — keep a known-good data cable for exactly this test. |
| `.uf2` says family `rp2040` | `-DPICO_BOARD` / `-DPICO_PLATFORM` not applied. Reconfigure with `--fresh`. |
