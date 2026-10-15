# Continuous integration

Three workflows, six jobs. They run on every push to `main` and on every pull request.

| Workflow | Job | What it checks |
|---|---|---|
| `protocol` | `drift` | The generated files still match the spec |
| | `generated-code` | The generated header compiles and the module imports |
| `firmware` | `build` | The firmware and the bench harnesses build, and target the right chip |
| | `pio-budget` | PIO programs fit the 32-instruction limit |
| `python` | `lint` | ruff, ruff format, mypy |
| | `test` | The unit suite, on Python 3.9 and 3.13 |

## protocol

**`drift`** runs `python protocol/codegen.py --check`. It regenerates `messages.h` and `protocol.py` and fails if what is committed differs.

This is the one that matters most. The wire format is generated from `protocol/`, so a hand-edit to either output makes the firmware and the host SDK disagree about the bytes on the wire — and nothing else would notice, because both halves still build and both still pass their own tests.

On failure it prints the diff, not just "files differ". Codegen validates before emitting, so an invalid spec fails here too, naming the rule that broke.

**`generated-code`** compiles the header and imports the module. This exists because nothing else does either yet — there is no firmware build and no SDK package. Without it a codegen change could emit something that does not compile, and CI would stay green.

It uses host `gcc` rather than the Arm cross-compiler. The header is fixed-width types and packed structs, so its struct-size assertions hold identically on x86, and that takes seconds instead of a 150 MB toolchain download. The import runs on **3.9 specifically**, so codegen emitting 3.10-only syntax fails here rather than for a user.

## firmware

**`build`** compiles for `pico2` and then checks the result rather than trusting the flags:

```
picotool info -a build/*.uf2   →   family must be rp2350-arm-s
```

An RP2040 build compiles, links, and produces a perfectly valid UF2 that simply will not run on a Pico 2. Asserting the family id catches that in CI instead of at the bench. The UF2 is uploaded as a run artefact, which is useful when you build on one machine and flash on another.

The same job then builds the **bench harnesses** under `tests/`, with the same toolchain, and checks their UF2s the same way. They are not throwaway test code: they compile firmware sources from where those live, so `tests/slave` builds `firmware/src/bus/i2c_bus.c` and assembles `firmware/pio/i2c_slave.pio`. Until the firmware project exists, this is the only job that compiles any firmware C at all. Their UF2s are uploaded too, so the bench machine can flash a build it did not compile.

**`pio-budget`** assembles every `.pio` with host `pioasm` and reports program lengths. PIO instruction memory is 32 instructions per block, shared by that block's four state machines, and it is the tightest constraint in the firmware. `pioasm` runs on the host, so this is enforceable with no hardware attached.

## python

**`lint`** runs `ruff check`, `ruff format --check` and `mypy`, using the configs at the repository root so local and CI runs are the same invocation.

**`test`** runs the unit suite on **3.9 and 3.13**. 3.9 is the floor the SDK promises; testing it catches 3.10-only syntax that a modern interpreter would accept. The suite needs no hardware. Hardware tests live elsewhere and are not run here.

## Skipped jobs say so

Parts of the project do not exist yet. A job with nothing to do **skips loudly**, with a notice naming what is missing and when it will run for real:

```
::notice title=Firmware build skipped::firmware/CMakeLists.txt does not exist yet.
```

A job that passes because it found nothing to do is the same failure mode this project refuses in the firmware itself.

## Pinned versions

The Arm toolchain, pico-sdk, picotool and pioasm versions in `firmware.yml` match [`tools/setup-windows.ps1`](../tools/setup-windows.ps1) and [`tools/setup-ubuntu.sh`](../tools/setup-ubuntu.sh). One version across CI and every developer machine, so a CI failure is never "works on my machine".

## Running the same checks locally

```bash
python protocol/codegen.py --check    # drift
ruff check . && ruff format --check . # lint
mypy .                                # types
python -m pytest sdk/tests -q         # unit tests
```

That is everything `protocol` and `python` do. The firmware jobs need the toolchain from [setup.md](setup.md).
