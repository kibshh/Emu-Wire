# The toolchain, and why each piece exists

There are a lot of tools in a Pico project and it isn't obvious what each one does or why it's there. This page explains the role of each, so that when something breaks you know which tool to blame.

For installing them, see [setup.md](setup.md).

## The shape of it

```
   .c / .h ──┐
              ├─► arm-none-eabi-gcc ──► .elf ──► picotool ──► .uf2 ──► board
   .pio ──► pioasm ──► .pio.h ──┘                                        ▲
                                                                          │
   CMake + Ninja orchestrate all of the above          OpenOCD + Debug Probe
                                                        (flash & debug over SWD)
```

Two things surprise people: **`.pio` files are assembled by a separate tool** before the C compiler ever sees them, and **`picotool` is part of the build**, not just a flashing utility.

---

## Building

### `arm-none-eabi-gcc` — the cross compiler

Your laptop is x86-64 or ARM64; the target is a Cortex-M33. `arm-none-eabi-gcc` is GCC configured to emit code for that target. The name describes it: **arm** architecture, **none** = no operating system (bare metal), **eabi** = the embedded ABI.

It ships with a matching `binutils` (assembler, linker) and `newlib`, a small C library suited to microcontrollers.

Use the official Arm tarball rather than your distribution's package. Distro builds differ in configuration, and if you work across several machines you want binaries that compare byte for byte.

> Companion tools worth knowing: `arm-none-eabi-size` (how much flash and RAM did I just use?), `arm-none-eabi-objdump -d` (disassemble), `arm-none-eabi-readelf -A` (which architecture did I *actually* build for?).

### `pico-sdk` — the hardware layer

Raspberry Pi's C/C++ SDK. It provides the startup code, linker scripts, clock and PLL configuration, and typed access to every peripheral, plus higher-level libraries for multicore, DMA, PIO and USB.

The key point: it is mostly a **collection of CMake targets**, not a prebuilt library. You link `pico_stdlib`, `hardware_pio`, `hardware_dma` and so on, and only what you name gets compiled in.

It also bundles **TinyUSB**, which is why USB works at all (see below).

### `CMake` — the build description

CMake doesn't build anything. It reads `CMakeLists.txt` and *generates* build files for another tool. The pico-sdk is built entirely around it: `pico_sdk_init()`, `pico_add_extra_outputs()` and `pico_generate_pio_header()` are SDK-provided CMake functions that wire up the real work.

Two flags matter constantly:

```
-DPICO_BOARD=pico2       # pin map, LED pin, flash size
-DPICO_PLATFORM=rp2350   # which chip family to target
```

Get these wrong and you get a clean, successful build of a binary that will not run.

### `Ninja` — the build executor

CMake generates Ninja files; Ninja runs the compiler. It exists because it is much faster than `make` at figuring out what actually needs rebuilding — noticeable on a project with hundreds of SDK source files.

You rarely invoke it directly; `cmake --build` calls it for you.

---

## PIO

### `pioasm` — the PIO assembler

PIO programs are written in their own assembly language in `.pio` files. `pioasm` assembles them into a C header containing the encoded instructions plus a helper function to configure a state machine.

```bash
pioasm -o c-sdk foo.pio foo.pio.h
```

In a normal build CMake calls this for you via `pico_generate_pio_header()`. Running it by hand is still useful, because of this:

**`pioasm` runs on your development machine and needs no hardware.** PIO instruction memory is limited to 32 instructions per PIO block, so knowing your program's length is a constant concern — and you can check it anywhere:

```console
$ pioasm -o c-sdk i2c.pio /tmp/out.h && grep _wrap /tmp/out.h
#define i2c_wrap_target 12
#define i2c_wrap 17
```

So a PIO program can be written *and* budget-checked without a board. Only its electrical behaviour needs one.

---

## Getting code onto a board

### `picotool` — UF2 generation, flashing, inspection

Three distinct jobs, and the first is the one people miss:

**1. It builds the `.uf2`.** In SDK 2.x, `picotool` replaced the old `elf2uf2` utility. Without it your build produces an `.elf` and stops. **This makes picotool a build dependency even on a machine that never touches hardware.**

**2. It flashes.** `picotool load -f firmware.uf2` writes over USB. With `picotool reboot` you can also restart a running board into bootloader mode, so reflashing doesn't require reaching for the BOOTSEL button — provided the firmware is still healthy enough to respond.

**3. It inspects.** This is worth building into habit:

```console
$ picotool info -a build/blink.uf2
File blink.uf2 family ID 'rp2350-arm-s':
 target chip:  RP2350
 pico_board:   pico2
 sdk version:  2.3.0
```

An accidental RP2040 build compiles, links and produces a perfectly valid UF2 that simply won't run on a Pico 2. Checking the family ID catches that in a second rather than at the bench.

### UF2 — the drag-and-drop format

A UF2 file is flash contents wrapped in 512-byte blocks, each carrying its own target address and a family ID. Hold BOOTSEL while plugging in the board and it appears as a USB drive; copy a UF2 onto it and the bootloader writes it to flash and reboots.

The point of the format is that it needs **no drivers and no tools** — a plain file copy is a firmware update. The family ID is what stops you flashing an RP2040 image onto an RP2350.

### `OpenOCD` + Debug Probe — real debugging

Drag-and-drop is fine until something hangs. OpenOCD speaks SWD (Arm's two-wire debug protocol) through a hardware probe, giving you breakpoints, single-stepping, memory and register inspection, and flashing without touching BOOTSEL at all.

The Raspberry Pi Debug Probe is a small board presenting a standard **CMSIS-DAP** interface plus a **separate UART**. That second part matters more than it sounds on any project where USB itself is under test: if your firmware's USB link is the thing you're debugging, you cannot also use it to print debug messages. The probe's UART is an independent channel.

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg
```

> Your distribution's `openocd` package is very likely too old to know what an RP2350 is. Check for `scripts/target/rp2350.cfg` before assuming otherwise.

One honest limitation: breakpoints change timing. For code with hard real-time constraints, a debugger tells you about state, and a logic analyzer tells you about timing. You need both, for different questions.

### `TinyUSB` — the USB stack

Implementing USB from scratch is a large job. TinyUSB is an embedded USB stack bundled with the pico-sdk as a submodule — which is why `git submodule update --init lib/tinyusb` is not optional if you want USB.

It provides the device classes; **CDC** (Communications Device Class) is the one that makes a board appear as a serial port, with no driver installation on any modern OS.

---

## Host-side tooling

### `Python`, `ruff`, `mypy`, `pytest`

The host SDK is Python, because every embedded team already has it installed and nobody needs convincing to `pip install` something.

| Tool | Role |
|---|---|
| **ruff** | Linter *and* formatter, replacing flake8/isort/black. Fast enough to run on save. |
| **mypy** | Static type checking. Catches interface mistakes before they reach a device. |
| **pytest** | Tests. Unit tests must pass with no hardware present; hardware tests skip cleanly when no board is attached. |

Configuration lives at the repository root (`ruff.toml`, `mypy.ini`) specifically so that what you run locally and what CI runs are the same thing.

### `sigrok` / `PulseView` — logic analysis

Open-source logic-analyzer software. PulseView is the GUI, `sigrok-cli` the command line, and both support a wide range of hardware including the inexpensive FX2LP-based USB analyzers.

A logic analyzer answers the question no other tool here can: *what actually appeared on the wire?* A debugger tells you what your code believes; only a capture tells you what the pins did.

Useful sanity check before blaming your firmware:

```bash
sigrok-cli --scan          # is the analyzer even detected?
```

Note that sampling rate sets a floor on what you can observe. A 24 MHz analyzer samples every ~41.7 ns, so anything shorter than that can fall between two samples and be invisible. Match the instrument to the effect you're trying to see.

---

## Which tools does a given machine need?

| | Build only | Bench |
|---|---|---|
| arm-none-eabi-gcc, CMake, Ninja, pico-sdk | ✅ | ✅ |
| pioasm | ✅ | ✅ |
| picotool | ✅ *(builds the UF2)* | ✅ *(also flashes)* |
| Python + ruff/mypy/pytest | ✅ | ✅ |
| OpenOCD + Debug Probe | — | ✅ |
| sigrok / PulseView | — | ✅ |

A machine with no hardware attached can still compile firmware, assemble and budget PIO programs, and run the entire host test suite.
