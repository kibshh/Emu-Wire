# EmuWire

**Emulate your whole sensor bus from one connector — then break it on purpose and watch what your firmware does.**

Plug a €5 Raspberry Pi Pico 2 in where your I2C/SPI sensors would be. Your firmware talks to what it thinks are four separate chips. Except you can NACK on demand, stretch the clock, violate setup timing, and capture every transaction it made.

> **Status: pre-alpha. Nothing works yet.**
> This repository currently contains the plan and the project scaffolding. There is no firmware, no SDK, and no released package. Watch or star if you want to know when there is.

---

## What it will do

```python
import emuwire as ew

dev = ew.connect()                        # USB CDC, auto-detect

# One bus, four devices, one connector
bus = dev.i2c_bus(scl="GP4", sda="GP5", speed=400_000)
bmp = bus.attach("bmp280", address=0x76)
sht = bus.attach("sht4x",  address=0x44)
lis = bus.attach("lis3dh", address=0x19)
ina = bus.attach("ina219", address=0x40)

bmp.set(press_msb=0x8A, temp_msb=0x7F)

# --- what real hardware cannot do ---
bmp.fault("nack_on_read", count=3)
bmp.fault("clock_stretch", us=5000)
sht.fault("setup_violation", ns=40)       # PIO-only
lis.fault("bus_hang")

# --- and what no emulator does ---
with bus.trace() as t:
    run_firmware_test()
print(t.transactions)                     # every access, timestamped
```

*This API is the design target, not a description of working software.*

## Why

1. **Fault injection, including timing violations no other tool can produce.** ISO 26262 (Parts 4 and 6) and IEC 61508 require testing safety mechanisms that are never invoked during normal operation.
2. **Whole-bus emulation.** Every device on the bus, from one connector — not one device at a time.
3. **The part hasn't arrived.** Sensor and MCU lead times run 30–55 weeks. Write the driver now.
4. **Trace and inject together.** See exactly how your firmware reacted to the failure you caused.

## How it works

RP2350's PIO is a programmable bus engine rather than a fixed peripheral, so address matching becomes a table lookup instead of a register field. One state machine watches SDA/SCL, stretches the clock while the second core decides whether to ACK, and answers for every device on the bus.

That same mechanism gives timing-violation faults and a passive bus sniffer for free — and the sniffer that traces your bus is the same code that verifies emulated devices against real silicon.

```
Python SDK (laptop) ←USB CDC→ RP2350 ←SCL/SDA, SPI, PWM, GPIO→ your unmodified DUT
```

**v1 is digital only** — I2C, SPI, PWM, digital I/O. Analog output will be available in the future.

## Hardware

A Raspberry Pi Pico 2 (RP2350A, ~€5), pull-up resistors, and jumper wires. No custom board required.

## Documentation

| | |
|---|---|
| [Setting up a development machine](docs/setup.md) | Windows and Linux, with or without hardware |
| [RP2350 / Pico 2 architecture](docs/rp2350.md) | The chip, and the PIO constraints that shape the design |
| [The toolchain explained](docs/toolchain.md) | What each tool does and why it's in the chain |

You don't need a Pico to contribute. Most of the code — the host SDK, the wire protocol, manifests, even PIO programs and their instruction budgets — is testable without one.

## Licence

Apache 2.0 — see [LICENSE](LICENSE).

The firmware, PIO programs, SDK and core sensor manifests are open. A verified manifest library — device models trace-diffed byte-for-byte and timing-for-timing against real silicon — will be offered commercially.

<!-- TODO(task 93): demo GIF — attach a sensor, inject a fault, show the trace -->
<!-- TODO(task 92): link the docs site once MkDocs is up -->
<!-- TODO(task 94): pip install instructions once published to PyPI -->
