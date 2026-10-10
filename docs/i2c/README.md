# I2C from zero

A course on the I2C bus, for someone who has never used it. It covers the protocol bit by bit, the electrical side, how real devices use it, and what goes wrong on a real bench.

By the end you should be able to read an I2C capture byte by byte, pick pull-up resistors, talk to a register-based sensor, and find out why a bus isn't working.

The numbers come from the I2C specification itself, NXP's **UM10204** (rev. 7). Everything about the RP2350's I2C controller was checked against the pico-sdk 2.3.0 source. Several lessons come from this project's own bench sessions, and are marked as such.

**You need:** knowing what a GPIO, a voltage and a clock are. No I2C knowledge at all.

> **A note on words.** The 2021 revision of the spec renamed *master* → **controller** and *slave* → **target**. This course uses the new terms. Older datasheets, the pico-sdk and this project's code still say master and slave. They mean exactly the same thing.

---

## Contents

1. [What I2C is](#1-what-i2c-is)
2. [Two wires, open drain, and pull-ups](#2-two-wires-open-drain-and-pull-ups)
3. [Bits: when SDA may change](#3-bits-when-sda-may-change)
4. [START, STOP and repeated START](#4-start-stop-and-repeated-start)
5. [Bytes and the ACK bit](#5-bytes-and-the-ack-bit)
6. [Addresses](#6-addresses)
7. [Transactions: write, read, and combined](#7-transactions-write-read-and-combined)
8. [Talking to real devices: registers](#8-talking-to-real-devices-registers)
9. [Clock stretching](#9-clock-stretching)
10. [Speeds and timing](#10-speeds-and-timing)
11. [The electrical side: pull-ups, capacitance, rise time](#11-the-electrical-side-pull-ups-capacitance-rise-time)
12. [Several controllers on one bus](#12-several-controllers-on-one-bus)
13. [Stuck buses and recovery](#13-stuck-buses-and-recovery)
14. [Relatives and extensions](#14-relatives-and-extensions)
15. [The RP2350's I2C controller](#15-the-rp2350s-i2c-controller)
16. [I2C in EmuWire](#16-i2c-in-emuwire)
17. [Pitfalls](#17-pitfalls)
18. [Debugging an I2C bus](#18-debugging-an-i2c-bus)
19. [Cheat sheet](#19-cheat-sheet)
20. [Further reading](#20-further-reading)

---

## 1. What I2C is

I2C ("I-squared-C", **Inter-Integrated Circuit**) is a way for chips on the same board to talk using **two wires**. Philips invented it in the early 1980s; NXP maintains the specification today.

It is everywhere because it is cheap:

- **Two wires for any number of devices.** A temperature sensor, an EEPROM, a display and a real-time clock can all share the same two wires.
- **Every device has an address.** The controller says who it's talking to, and only that device answers.
- **No chip-select lines.** SPI needs an extra wire per device; I2C doesn't.

The price is speed. I2C typically runs at 100 kHz or 400 kHz, sometimes 1 MHz. That's plenty for sensors and configuration, and far too slow for a camera.

### Who does what

- The **controller** starts every transaction and drives the clock. Usually a microcontroller.
- A **target** answers when its address is called. Sensors, memories, and so on.

A bus normally has one controller and several targets. Several controllers on one bus are allowed too (lesson 12), but rare.

---

## 2. Two wires, open drain, and pull-ups

### The wires

| Wire | Name | Who drives it |
|---|---|---|
| **SCL** | serial clock | the controller (a target may hold it low — lesson 9) |
| **SDA** | serial data | whoever is sending at that moment, controller or target |

Plus a shared **ground**. Without a common ground there is no common idea of "low".

### Nobody ever drives a line high

This is the most important idea in I2C. Every device connects to SDA and SCL with an **open-drain** output:

```
          VDD (e.g. 3.3 V)
           │
          ┌┴┐
          │ │  pull-up resistor, one per line, shared by the whole bus
          └┬┘
           │
  ─────────┴──────────┬───────────────┬────────── the line (SDA or SCL)
                      │               │
                   ┌──┴──┐         ┌──┴──┐
                   │ ON? │         │ ON? │        each device: a switch to ground
                   └──┬──┘         └──┬──┘
                     GND             GND
                   device A        device B
```

Each device can do exactly two things to a line:

- **pull it low**, by closing its switch to ground
- **let go**, by opening its switch. The resistor then pulls the line up to VDD.

No device ever connects a line to VDD. That has three consequences, and the rest of I2C is built on them.

### Consequence 1: no device can short another

If device A lets go while device B pulls low, the line is simply low. Nothing fights. With ordinary push-pull outputs, A driving high while B drives low would short VDD to ground through both chips.

### Consequence 2: the line is a wired-AND

The line is high only if **every** device has let go. If **any** device pulls low, it's low.

```
  A lets go,  B lets go   →  high
  A lets go,  B pulls low →  low
  A pulls low, B lets go  →  low
  A pulls low, B pulls low → low
```

That's an AND of everyone's "let go" signals. Clock stretching (lesson 9) and arbitration (lesson 12) are both built on it.

### Consequence 3: the pull-ups matter a lot

Pulling low is fast: a transistor switches on hard. Going high is slow: only the resistor pulls the line up, charging the wire's capacitance. The resistor's value decides how fast the rising edges are, which decides how fast the bus can run. Lesson 11 covers how to choose it.

**Without pull-ups, nothing works at all.** Both lines float near 0 V and every device thinks the bus is permanently busy. Most microcontrollers have internal pull-ups, but at 30–80 kΩ they're far too weak for real I2C edges. Use real resistors.

---

## 3. Bits: when SDA may change

The rule that defines a data bit:

> **SDA may only change while SCL is low. While SCL is high, SDA must stay still.**

```
              change           change           change
                ↓                ↓                ↓
SDA   ══════════╳════════════════╳════════════════╳═════════
SCL   ‾‾‾‾\__________/‾‾‾‾‾‾\__________/‾‾‾‾‾‾\_____________
                        ↑                ↑
                    sampled          sampled
```

The sender puts the bit on SDA while SCL is low. The controller then releases SCL. While SCL is high, the receiver reads SDA. Then SCL goes low again, and the sender may put the next bit on.

A "1" is the line let go (high). A "0" is the line pulled low.

There's exactly one bit per SCL pulse, whoever is sending. SCL always comes from the controller, even when the target is sending data back.

---

## 4. START, STOP and repeated START

If SDA may only change while SCL is low, then **SDA changing while SCL is high** can mean something else. I2C uses exactly that for its two framing signals:

| Signal | What happens | Meaning |
|---|---|---|
| **START** (S) | SDA falls while SCL is high | a transaction begins, everyone listen |
| **STOP** (P) | SDA rises while SCL is high | the transaction is over, the bus is free |
| **repeated START** (Sr) | a START without a STOP before it | a new transaction begins, but the controller keeps the bus |

```
SDA  ‾‾‾‾‾\_____________ ... ____________________/‾‾‾‾‾‾
SCL  ‾‾‾‾‾‾‾‾‾‾\___/‾‾‾\ ... /‾‾‾\______/‾‾‾‾‾‾‾‾‾‾‾‾‾‾
          ↑                                      ↑
        START:                                 STOP:
   SDA falls, SCL high                    SDA rises, SCL high
```

Only the controller generates these. Between a START and a STOP the bus is **busy**. After a STOP, it's free.

Data bits never look like START or STOP, because data bits only change while SCL is low. That's why every receiver can always tell framing from data.

### Why repeated START exists

A common need: "write a register number, then read from that register". If the controller sent a STOP between the two halves, the bus would be free for a moment, and on a bus with several controllers another one could jump in. A **repeated START** goes straight from one transaction to the next without letting go of the bus. Lesson 7 shows it in use.

---

## 5. Bytes and the ACK bit

### Eight bits, most significant first

Data travels in **bytes**, sent **MSB first**: bit 7, bit 6, … bit 0.

### Then a ninth bit: the answer

After every byte, the **receiver** answers on a ninth clock pulse:

- **ACK** (acknowledge): the receiver **pulls SDA low** during the ninth pulse. "Got it."
- **NACK** (not acknowledge): the receiver **leaves SDA high**. "No."

```
         bit 7   bit 6   bit 5   bit 4   bit 3   bit 2   bit 1   bit 0    ACK
SCL  __/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__
        └──────────────── sender drives SDA ────────────┘  └ receiver ┘
```

So the sender releases SDA after bit 0, and the receiver takes over for one bit. When the controller writes, the target ACKs. When the controller reads, the controller ACKs.

Since a NACK is just "nobody pulled SDA low", a NACK is also exactly what happens when **nobody is there**. Calling an address with no device on it gets a NACK. That's how I2C scanners find devices.

### Why a receiver says NACK

The spec lists five situations:

1. No device has that address.
2. The target is busy and can't receive right now (an EEPROM in the middle of writing, for example).
3. The target got data or a command it doesn't understand.
4. The target can't take any more data.
5. **A controller reading data NACKs the last byte**, to tell the target "that's enough". This is normal, not an error.

Number 5 matters: every read ends with a NACK. Seeing NACK in a capture doesn't mean something failed. Check *which* byte got it.

---

## 6. Addresses

### Seven bits, plus a direction bit

Every target has a **7-bit address**. The first byte after a START carries it, together with one more bit that says the direction:

```
 bit:   7    6    5    4    3    2    1  │  0
       [ ───────── address (7 bits) ──── ] [R/W]
                                            0 = write: controller → target
                                            1 = read:  target → controller
```

So address `0x76`, shifted up by one, gives two possible first bytes:

| | Byte on the wire |
|---|---|
| `0x76` + write | `0xEC` (`0x76 << 1`) |
| `0x76` + read | `0xED` (`0x76 << 1 \| 1`) |

> **The most common confusion in I2C.** Some datasheets give the 7-bit address (`0x76`). Others give the two 8-bit bytes (`0xEC`/`0xED`) and call those "the address". Software libraries almost always want the 7-bit form. If a device seems to be at an address twice too high, this is why.

### Reserved addresses

Not all 128 addresses are for normal devices. The spec reserves 16 of them:

| 7-bit address | Purpose |
|---|---|
| `0x00` | general call (with W) or START byte (with R) |
| `0x01` | CBUS addresses |
| `0x02` | reserved for other bus formats |
| `0x03` | reserved for future use |
| `0x04`–`0x07` | High-speed mode controller codes |
| `0x78`–`0x7B` | first byte of a **10-bit address** |
| `0x7C`–`0x7F` | device ID, and reserved |

That leaves **`0x08`–`0x77`, 112 addresses**, for ordinary devices.

### Choosing a device's address

A device's address is mostly fixed in its silicon. Many devices also have one or two **address pins**, so a few copies can share a bus. The BMP280 pressure sensor, for example:

| SDO pin | Address |
|---|---|
| tied to GND | `0x76` |
| tied to VDD | `0x77` |

Leave such a pin unconnected and the address is undefined. Some breakout boards tie it one way with a resistor; many tie it to the *other* address from the one you expect.

### Two devices, one address

If two devices with the same address share a bus, both answer at once. Writes reach both. On a read, both drive SDA together, and because the line is a wired-AND, you read the **bitwise AND** of their two answers. Nothing reports an error. It's always a wiring mistake, and a hard one to spot.

---

## 7. Transactions: write, read, and combined

Notation below: **S** START, **Sr** repeated START, **P** STOP, **A** ACK, **N** NACK. The second line says who sends each part: **C** controller, **T** target.

### Write

```
 S   addr+W   A   data   A   data   A   ...   P
 C   C        T   C      T   C      T         C
```

The controller sends as many bytes as it wants. The target ACKs each one. If the target NACKs a byte, the controller should stop: the target refused it.

### Read

```
 S   addr+R   A   data   A   data   A   ...   data   N   P
 C   C        T   T      C   T      C         T      C   C
```

After the address, the **target** sends the data and the **controller** ACKs each byte. On the last byte the controller sends **NACK**, which tells the target to stop and let go of SDA. Then STOP.

### Combined: write, then read

The most common pattern of all, for reading a register:

```
 S   addr+W   A   register   A   Sr   addr+R   A   data   N   P
 C   C        T   C          T   C    C        T   T      C   C
```

1. Write the register number.
2. **Repeated START**, without a STOP.
3. Read the data.

The repeated START keeps the bus. Many devices also accept a STOP and a new START in the middle. Some forget the register pointer if they see a STOP, and on multi-controller buses another controller could jump in, so the repeated START is the safe default.

---

## 8. Talking to real devices: registers

I2C itself only moves bytes. What the bytes *mean* is up to each device. But nearly all sensors and peripherals follow the same convention.

### The register map

The device behaves like a small block of numbered **registers**, each usually one byte. The datasheet lists them. For example, from the BMP280:

| Register | Name | Meaning |
|---|---|---|
| `0xD0` | `id` | chip ID, always `0x58` |
| `0xF4` | `ctrl_meas` | measurement settings — writable |
| `0xF7`–`0xF9` | `press` | pressure reading, 3 bytes |
| `0xFA`–`0xFC` | `temp` | temperature reading, 3 bytes |

### The register pointer

The device keeps an internal **pointer**: which register the next read or write uses.

- **The first byte the controller writes after the address sets the pointer.**
- Any further bytes in the same write go **into** registers, starting at the pointer.
- A read returns data **from** the pointer.

So:

```
Write 0x27 to register 0xF4:   S  0x76+W  A  0xF4  A  0x27  A  P
                                              ↑ pointer  ↑ value

Read register 0xD0:            S  0x76+W  A  0xD0  A  Sr  0x76+R  A  [0x58]  N  P
```

### Auto-increment and burst reads

Most devices **move the pointer forward** after each byte. Reading several bytes in a row then returns several registers in a row, in one transaction. That's a **burst read**:

```
S  0x76+W  A  0xF7  A  Sr  0x76+R  A  [press_msb] A [press_lsb] A ... [temp_xlsb] N  P
```

Six registers, one transaction, and all six belong to the same measurement. Reading them one at a time could mix bytes from two different measurements.

Devices differ in the details, and the datasheet is the only authority:

- Some don't auto-increment at all. Some only do on reads.
- At the end of the map, some wrap around to 0, some stop, some jump to a fixed place.
- A read **without** writing a pointer first returns whatever the pointer was left at, often register 0 after power-up.

### Larger devices: 16-bit register addresses

Memories bigger than 256 bytes need more address bits. Small EEPROMs borrow them from the device address itself: a 2 KB part answers on eight consecutive addresses, one per 256-byte block. Larger ones, such as a 32 KB EEPROM, take **two** address bytes after the device address, high byte first. Everything else works the same way.

### EEPROMs are busy after a write

After a write, an EEPROM spends a few milliseconds actually storing the data. During that time it **NACKs its own address**. To find out when it's done, the controller keeps sending the address until it gets an ACK. That's called **ACK polling**, and it's NACK reason 2 from lesson 5 in action.

---

## 9. Clock stretching

### What it is

The controller drives SCL. But because SCL is open drain too, a **target can hold it low**. The controller releases SCL, sees it's still low, and must wait until the target lets go. That pause is **clock stretching**.

```
                 controller lets go of SCL here
                          ↓
SCL  ‾‾\____________________________________/‾‾‾‾‾‾‾\___
              └── the target holds SCL low ─┘
                  "wait, I'm not ready"     ↑
                                    the target lets go:
                                    the clock pulse finally happens
```

It's the wired-AND from lesson 2 again: SCL is high only when **both** the controller and the target have let go.

### Why a target would do it

A target that's a microcontroller may need time to prepare the next byte, look something up, or finish an interrupt. Stretching lets it slow the bus down exactly when it needs to, without the controller knowing anything in advance.

### What it costs

- **Controllers must notice.** A controller that just counts time, without checking whether SCL actually went high, can't support stretching. Most do check. Some don't, or have bugs around it — the I2C controller in older Raspberry Pi boards is well known for mishandling stretched clocks. Lesson 16 describes one found in the pico-sdk.
- **The bus is held while it happens.** Nothing else can use the bus during a stretch.
- **The spec doesn't limit how long it lasts.** A target that holds SCL low forever stops the whole bus. SMBus, a stricter cousin of I2C, adds a timeout (lesson 14).

The spec makes stretching **optional** for targets. Plenty of devices never stretch; the BMP280 is one of them.

### The controller's own low time hides short stretches

The stretch starts when SCL falls, and the controller keeps SCL low for a while by itself anyway: half a period or more. If the target is ready before the controller would have released SCL, the stretch costs **no time at all** and is invisible on a capture. Only the part that runs past the controller's own low time is visible.

> **Found on this project's bench:** an emulated target that stretches on every byte showed **0.00 µs** of extra time at 400 kHz. The decision finished inside the controller's 1.5 µs low time. To see the stretch at all, you need a faster bus, where the controller's low time is shorter.

---

## 10. Speeds and timing

### The speed modes

| Mode | Max clock | Notes |
|---|---|---|
| **Standard-mode** (Sm) | 100 kHz | the original |
| **Fast-mode** (Fm) | 400 kHz | the most common today |
| **Fast-mode Plus** (Fm+) | 1 MHz | needs stronger drivers and stronger pull-ups |
| High-speed mode (Hs) | 3.4 MHz | special start sequence, rarely used (lesson 14) |
| Ultra Fast-mode (UFm) | 5 MHz | one direction only, push-pull — effectively a different bus |

The maximum is a ceiling, not a requirement: any lower clock is fine, down to near zero. A target rated for Fast-mode works at 100 kHz too.

### The timing rules

The spec defines minimum and maximum times for every part of a transaction. These are the ones that matter most:

| Parameter | Meaning | Sm | Fm | Fm+ |
|---|---|---|---|---|
| f<sub>SCL</sub> | clock frequency, max | 100 kHz | 400 kHz | 1 MHz |
| t<sub>LOW</sub> | SCL low, min | 4.7 µs | 1.3 µs | 0.5 µs |
| t<sub>HIGH</sub> | SCL high, min | 4.0 µs | 0.6 µs | 0.26 µs |
| t<sub>SU;DAT</sub> | data set up before SCL rises, min | 250 ns | 100 ns | 50 ns |
| t<sub>HD;DAT</sub> | data held after SCL falls, min | 0 | 0 | 0 |
| t<sub>HD;STA</sub> | after a START, before the first clock, min | 4.0 µs | 0.6 µs | 0.26 µs |
| t<sub>SU;STA</sub> | before a repeated START, min | 4.7 µs | 0.6 µs | 0.26 µs |
| t<sub>SU;STO</sub> | before a STOP, min | 4.0 µs | 0.6 µs | 0.26 µs |
| t<sub>BUF</sub> | bus free between STOP and next START, min | 4.7 µs | 1.3 µs | 0.5 µs |
| t<sub>r</sub> | **rise time**, max (30% → 70% of VDD) | 1000 ns | 300 ns | 120 ns |
| t<sub>f</sub> | fall time, max | 300 ns | 300 ns | 120 ns |
| C<sub>b</sub> | total bus capacitance, max | 400 pF | 400 pF | 550 pF |
| t<sub>SP</sub> | spikes inputs must ignore | — | 50 ns | 50 ns |

Notes:

- **The data hold time minimum is 0**, but a device must hold SDA long enough internally to get past SCL's own falling edge. The pico-sdk's comment puts it well: devices "must internally provide a hold time of at least 300 ns for the SDA signal to bridge the undefined region of the falling edge of SCL".
- **The rise time** is the one most likely to break a real bus. Lesson 11 is about it.
- Fast-mode Plus devices must also sink much more current (20 mA, against 3 mA for the others). That's what allows the stronger pull-ups it needs.

### Logic levels

| | Level |
|---|---|
| a valid **low** (V<sub>IL</sub>) | below **30 %** of VDD |
| a valid **high** (V<sub>IH</sub>) | above **70 %** of VDD |
| what a device must pull down to (V<sub>OL</sub>) | 0.4 V or less, at its rated current |

So at 3.3 V a high must be above 2.31 V. Anything between 30 % and 70 % is undefined: a slow edge spends real time in that zone, which is one more reason rise time matters.

---

## 11. The electrical side: pull-ups, capacitance, rise time

### Why edges are slow

Every wire, every pin and every connector has some **capacitance** to ground. When all devices let go of a line, the pull-up resistor has to charge that capacitance. The time it takes is set by the resistance times the capacitance, **R × C**:

```
  rise time (30 % → 70 % of VDD)  =  0.8473 × Rp × Cb
```

Bigger resistor or more capacitance → slower rising edges.

Falling edges are fast, because a transistor discharges the line directly. That's why I2C waveforms look like a shark fin: a sharp drop, then a curved climb.

### Choosing the pull-up: the two limits

The resistor has a **maximum**, set by speed:

```
  Rp(max) = tr(max) / (0.8473 × Cb)
```

and a **minimum**, set by how much current a device can sink while still reaching a valid low:

```
  Rp(min) = (VDD − VOL(max)) / IOL        VOL(max) = 0.4 V
                                          IOL = 3 mA (Sm, Fm) or 20 mA (Fm+)
```

Pick anything between the two. Closer to the minimum gives faster edges and more power; closer to the maximum gives slower edges and less power.

### Worked examples, at 3.3 V with 100 pF of bus

| Mode | Rp(max) | Rp(min) | A good choice |
|---|---|---|---|
| Standard (tr ≤ 1000 ns) | 1000 / (0.8473 × 100 p) = **11.8 kΩ** | 2.9 V / 3 mA = **967 Ω** | 4.7–10 kΩ |
| Fast (tr ≤ 300 ns) | 300 / (0.8473 × 100 p) = **3.5 kΩ** | **967 Ω** | 2.2 kΩ |
| Fast Plus (tr ≤ 120 ns) | 120 / (0.8473 × 100 p) = **1.4 kΩ** | 2.9 V / 20 mA = **145 Ω** | 1 kΩ |

The classic "4.7 kΩ pull-ups" are fine for 100 kHz, marginal for 400 kHz, and too weak for 1 MHz on a 100 pF bus: 4.7 kΩ × 100 pF gives about 400 ns of rise time.

### Where the capacitance comes from

- each device pin: up to 10 pF
- the microcontroller pin: similar
- wires and traces: typically tens of picofarads per metre
- connectors, breadboards, and any logic analyzer probe

A short bus with two devices may be 20–30 pF. A metre of cable with several boards on it can reach the 400 pF limit.

### Pull-ups add up

Many breakout boards come with their own pull-ups, often 4.7 kΩ or 10 kΩ. Connect three such boards and you have three resistors **in parallel**: three 4.7 kΩ resistors make about 1.6 kΩ. That's still fine. Ten of them make 470 Ω, which may be too strong for a Standard-mode device to pull low. Count what's on the bus.

### Mixed voltages

Two devices at different supply voltages, say 3.3 V and 5 V, can't simply share a bus: the 5 V side's pull-ups would put 5 V on the 3.3 V chip's pins. The usual fix is a **level shifter**, typically one small MOSFET per line, with pull-ups on both sides (NXP application note AN10441). Check a chip's datasheet before connecting it to a higher voltage.

### Unpowered devices pull the bus down

A device with no power often clamps SDA and SCL to ground through its internal protection diodes. The whole bus then sits near 0 V and nothing works. If a bus is dead, check that every device on it is powered.

---

## 12. Several controllers on one bus

Most buses have one controller. The spec allows several, and the wired-AND makes it work without any extra wires.

### Clock synchronisation

If two controllers drive SCL at once, the line is low while **either** holds it low. So the combined clock's low phase lasts as long as the **longest** low, and its high phase as long as the **shortest** high. Both controllers watch SCL and follow the combined clock. This is the same mechanism that lets a target stretch the clock.

### Arbitration

If two controllers start a transaction at the same moment, both send their bits on SDA and both **read SDA back** after every bit. As long as they send the same bits, nothing is lost. At the first bit where one sends a 1 (lets go) and the other a 0 (pulls low), the line is low:

- the one that sent 0 sees what it sent, and carries on, unaware anything happened
- the one that sent 1 sees 0 instead, knows it **lost arbitration**, and stops driving SDA

No data is corrupted. The winner's transaction goes through untouched, and the loser tries again once the bus is free. Because 0 wins, the controller calling the **lower address** wins.

---

## 13. Stuck buses and recovery

### The stuck target

The most common way an I2C bus dies: **a target holds SDA low, forever**.

It happens when a transaction is cut off in the middle. Say a target is sending a byte and has just put a `0` on SDA. Then the controller resets, or loses power for a moment, or a wire gets plugged in. The target is still waiting for the clock pulses that would let it finish the byte. Until they come, it holds SDA low.

The controller now sees SDA low and thinks the bus is busy. Every transaction fails, typically with a timeout. From outside it looks like dead hardware.

> **Found on this project's bench:** wiring a breadboard while it was powered was enough to make every transaction fail. What fixed it was **powering off, waiting, and powering on**. The lesson: change wiring with the power off.

### Recovering: nine clock pulses

The spec's procedure, called **bus clear**:

1. The controller sends **up to nine clock pulses** on SCL, watching SDA.
2. Somewhere in those pulses, the stuck target finishes its byte and lets go of SDA.
3. The controller then sends a **STOP**, and everything is back to idle.

Nine is enough because a target can be at most eight bits plus an ACK into a byte.

If that doesn't work, the last resorts are a hardware reset of the target, or cycling its power.

### Software reset

The spec also defines a **software reset**: a general call (address `0x00`, write) followed by the byte `0x06`. Devices that support it reset themselves. It needs a working bus to send, so it can't fix a stuck one.

---

## 14. Relatives and extensions

### 10-bit addresses

For buses that need more than 112 addresses. The first byte is `11110` followed by the top two address bits and R/W; a second byte carries the other eight. Those `11110xx` patterns are why `0x78`–`0x7B` are reserved. Rare in practice.

### General call

Address `0x00` with write: a message to **every** device at once. Devices that care answer; others ignore it. Its main use is the software reset above.

### High-speed mode (3.4 MHz)

A Hs transaction starts in Fast-mode with a special **controller code** (`00001xxx`, which no device may ACK), then switches to Hs speed until the next STOP. The controller uses an active current source to pull SCL up fast. Few devices support it.

### Ultra Fast-mode (5 MHz)

Push-pull outputs, one direction only (controller to targets), no ACK at all. It shares I2C's framing but is really a separate bus, for things like LED drivers.

### SMBus and PMBus

**SMBus**, the System Management Bus, is I2C with stricter rules, used on PC motherboards and batteries. The differences that bite:

- a **timeout**: a device holding the clock low for more than about 25–35 ms must reset. A long clock stretch that's legal in I2C isn't in SMBus.
- a minimum clock frequency, so a controller can't simply stop the clock
- optional **PEC**, a CRC-8 byte at the end of a transaction for error checking
- fixed voltage thresholds, instead of I2C's percentages of VDD

**PMBus** (power supplies) is built on SMBus.

### I3C

The newer MIPI I3C bus is a faster successor, using push-pull signalling for speed. It can share a bus with many ordinary I2C targets.

---

## 15. The RP2350's I2C controller

The RP2350 has **two I2C controllers** (`i2c0`, `i2c1`), each able to act as controller or target. Each is available on a fixed set of pins, chosen with the GPIO function select.

### Using it from the pico-sdk

```c
i2c_init(i2c0, 400 * 1000);                 // 400 kHz
gpio_set_function(4, GPIO_FUNC_I2C);        // SDA on GP4
gpio_set_function(5, GPIO_FUNC_I2C);        // SCL on GP5

uint8_t reg = 0xD0, id;
i2c_write_blocking(i2c0, 0x76, &reg, 1, true);   // true = no STOP: repeated START next
i2c_read_blocking(i2c0, 0x76, &id, 1, false);    // false = STOP at the end
```

The `nostop` argument is how you get a repeated START: `true` on the write leaves the bus held, and the next call starts with Sr.

The `_timeout_us` versions return after a deadline instead of waiting forever. Use them: a stuck bus otherwise hangs your program.

### What the return value means

Read from the SDK source:

| Return | Meaning |
|---|---|
| number of bytes | everything was ACKed |
| `PICO_ERROR_GENERIC` | the **address** was NACKed — usually nobody there |
| a smaller number of bytes | a **data** byte was NACKed; the number says how many got through |
| `PICO_ERROR_TIMEOUT` | the deadline passed |

So a refused register write returns `1` (the register number got through, the value didn't), not an error code.

### How it makes the clock

From `i2c_set_baudrate()` in the SDK:

- each SCL period is split **60 % low, 40 % high** — at 400 kHz, 1.5 µs low and 1.0 µs high
- SDA is held **300 ns** after each falling edge of SCL (120 ns at 1 MHz)
- input spikes shorter than about a sixteenth of the low time are filtered out

### The reported speed is arithmetic, not a measurement

`i2c_set_baudrate()` returns the frequency it *aimed* for. The real clock is slower: the controller counts its high time from the moment it **sees** SCL go high. Rise time and the spike filter come first, and add a little to every period. That's also how it supports clock stretching.

> **Measured on this project's bench:** set to 400 kHz, the bus actually ran at **375 kHz** — 2.67 µs per bit instead of 2.5 µs.

### Internal pull-ups

The RP2350's internal pull-ups are tens of kilohms: fine for a button, far too weak for I2C. Use external resistors.

---

## 16. I2C in EmuWire

EmuWire **pretends to be I2C targets**: you plug it into a board where sensors would go, and the board's firmware talks to it as if real sensors were there. That puts EmuWire on the target side of everything above.

### One state machine, many addresses

A real sensor answers one address. EmuWire answers up to eight, with one PIO state machine, deciding per transaction whether an address belongs to it. That decision needs a table lookup, and the ACK deadline (lesson 5) is shorter than a CPU can reliably meet. So EmuWire uses **clock stretching** (lesson 9): after every byte, it holds SCL low, the CPU decides, and the bus continues.

The PIO side of that is lesson 13 of [PIO from zero](../pio/README.md#13-case-study-emuwires-i2c-slave).

### What that means for controllers

- **A controller must support clock stretching** to work with EmuWire. Every I2C controller that checks SCL before counting its high time does. One that just counts time without looking at SCL doesn't.
- **At normal speeds the stretch is usually invisible.** The decision is quick enough to finish inside the controller's own low time (lesson 9).
- **A real BMP280 never stretches.** When EmuWire stretches and the real part wouldn't, that's a difference from real silicon, and EmuWire records it rather than hiding it.

### A real controller bug, found by stretching

On the bench, EmuWire refused a register write the correct way: it took the register number and NACKed the value. The capture showed a clean NACK and STOP.

The pico-sdk's `i2c_write` still reported success. It checks for a NACK as soon as the byte has been shifted out — which, with a stretching target, is **before** the ninth clock, while the target is still deciding. The missed NACK stays latched in the controller, and makes the **next** call fail with a timeout. The SDK even has a `TODO` at that exact spot asking whether an abort could arrive there.

So, two practical points:

- **If you drive EmuWire from a pico-sdk board**, a NACKed write may be reported as successful, followed by a mysterious timeout. That's the controller, not EmuWire. The workaround: after each `i2c_write`, check the controller's own abort flag (`raw_intr_stat` & `TX_ABRT`). If it is set, the write was refused; read `tx_abrt_source` for why, and read `clr_tx_abrt` to clear it so the next call starts clean. The project's test rig does exactly this, in `rig_write()` in [`tests/rig/main.c`](../../tests/rig/main.c).
- It's a good example of what an emulator is for: real firmware on a real bus behaving differently from what the driver promised.

### What the test rig checks

The project's I2C master rig ([`tests/rig/main.c`](../../tests/rig/main.c)) runs six transactions, each testing one part of this course:

| Rig test | What it exercises | Lesson |
|---|---|---|
| single-byte read | address, read, controller NACK at the end | 5, 7 |
| pointer + repeated START read | register pointer, repeated START, combined format | 4, 7, 8 |
| burst read (6 bytes) | auto-increment | 8 |
| register write, read back | writing a register, proven by reading it again | 8 |
| NACK from an absent address | nobody there means NACK | 5 |
| tolerates a stretched clock | the controller waiting while SCL is held | 9 |

One caution learned the hard way: the burst read only fails if all six bytes are **identical**. A target that sends one real byte and then stops reads back as `51 FF FF FF FF FF` — five bytes of nothing but pull-up — and still "passes". A test that doesn't check values can't tell you values are wrong.

---

## 17. Pitfalls

**1. The device "isn't there" — it's at twice the address you used.**
7-bit vs 8-bit address confusion. `0x76` and `0xEC` are the same device.
*Fix:* libraries want the 7-bit form.

**2. The device answers at the wrong address.**
An address pin is floating, or tied the other way on the breakout.
*Fix:* tie it explicitly. Check the board's schematic, not just the chip's datasheet.

**3. A sensor with both I2C and SPI ignores I2C.**
Its chip-select pin is low, which switches it to SPI.
*Fix:* tie CS high.

**4. Every transaction times out, even to empty addresses.**
A line is held low: no pull-ups, a stuck target, an unpowered device, or a line that simply isn't connected.
*Fix:* measure both lines with a multimeter at idle (lesson 18).

**5. It works at 100 kHz but fails at 400 kHz.**
Rise time. The pull-ups are too weak for the bus capacitance.
*Fix:* smaller pull-ups (lesson 11), a shorter bus, or a slower clock.

**6. A line reads 0 V and the wiring looks right.**
A pin pushed into a breadboard without being soldered, or a broken jumper.
*Fix:* solder headers. A line with no contact floats, and a multimeter shows it as about 0 V.

> Found on this project's bench: a breakout with its header pins only pushed through. SCL made contact and SDA didn't, and every transaction timed out.

**7. The bus dies after a reset or a wiring change.**
A target stuck mid-byte, holding SDA low.
*Fix:* bus clear (nine clocks, then STOP), or power-cycle. Wire with the power off.

**8. A write "succeeds" but the value never arrives.**
Possibly a controller driver missing a NACK — see lesson 16.
*Fix:* read the register back to check.

**9. A device returns the wrong register.**
A STOP between the pointer write and the read, on a device that resets its pointer on STOP.
*Fix:* repeated START (`nostop = true` on the write).

**10. An EEPROM stops answering right after a write.**
It's busy storing the data, and NACKs its address meanwhile.
*Fix:* ACK polling, or wait the write time from its datasheet.

**11. The bus goes slow, or stops, with one particular device present.**
That device stretches, and your controller handles stretching badly, or not at all.
*Fix:* a controller that supports stretching.

**12. Too many breakouts, and lows are no longer low.**
Their pull-ups in parallel became too strong.
*Fix:* remove pull-ups from some boards (often a solder jumper).

**13. A 3.3 V chip on a 5 V bus.**
Its pins see 5 V through the pull-ups.
*Fix:* a level shifter, unless the chip's datasheet says its pins tolerate it.

**14. A test passes when it shouldn't.**
It checked that bytes arrived, not what they were.
*Fix:* compare against known values whenever you have them.

---

## 18. Debugging an I2C bus

In order. The first step finds most problems.

**1. Measure the idle voltages.** With nothing happening, both SDA and SCL should sit at the pull-up voltage. A multimeter is enough.

| Idle reading | Meaning |
|---|---|
| both at VDD | electrically fine — look at addresses and software |
| one or both near 0 V, steady | held low: a stuck target, a short, or an unpowered device |
| near 0 V and drifting | not connected: no pull-up, or no contact |
| a bit below VDD | the pull-ups go to a lower supply, e.g. a breakout's own regulator. Usually fine. |

**2. Scan the bus.** Call every address from `0x08` to `0x77` and note which ones ACK. That shows what's actually there, at what address.

**3. Talk to one known register.** A chip ID register, with a known fixed value, is ideal: it proves addressing, the register pointer, repeated START and reading, in one transaction.

**4. Slow down.** If something works at 100 kHz and not faster, the problem is electrical (rise time), not logical.

**5. Capture it.** A logic analyzer with an I2C decoder shows every byte, every ACK and NACK, and every START and STOP. Check which byte got NACKed, and whether framing looks right. `sigrok-cli` can print the decoded bus as text with sample numbers, so timings can be measured exactly.

**6. Look at the edges.** A logic analyzer only sees 0 and 1. It can't show rise time or voltage levels. For those you need an **oscilloscope**.

**7. Power-cycle everything.** Cheap, and it clears stuck targets.

---

## 19. Cheat sheet

### Framing

```
START  SDA falls while SCL high        STOP   SDA rises while SCL high
data   SDA changes only while SCL low  ACK    receiver pulls SDA low on the 9th clock
```

### Transactions

```
write      S  addr+W A  data A  data A  P
read       S  addr+R A  data A  data N  P             (controller NACKs the last byte)
register   S  addr+W A  reg  A  Sr  addr+R A  data N  P
```

### Address byte

```
first byte = (address << 1) | R/W        R/W: 0 = write, 1 = read
0x76  →  0xEC write, 0xED read
usable addresses: 0x08 – 0x77
```

### Timing (minimums unless stated)

| | Sm 100 kHz | Fm 400 kHz | Fm+ 1 MHz |
|---|---|---|---|
| SCL low | 4.7 µs | 1.3 µs | 0.5 µs |
| SCL high | 4.0 µs | 0.6 µs | 0.26 µs |
| data setup | 250 ns | 100 ns | 50 ns |
| rise time, max | 1000 ns | 300 ns | 120 ns |
| bus capacitance, max | 400 pF | 400 pF | 550 pF |
| bus free STOP → START | 4.7 µs | 1.3 µs | 0.5 µs |
| sink current at 0.4 V | 3 mA | 3 mA | 20 mA |

### Pull-ups

```
rise time   tr     = 0.8473 × Rp × Cb
upper limit Rp(max) = tr(max) / (0.8473 × Cb)
lower limit Rp(min) = (VDD − 0.4 V) / IOL
valid low < 0.3 × VDD        valid high > 0.7 × VDD
```

### NACK means

nobody's there · busy · didn't understand · can't take more · **controller ending a read (normal)**

### pico-sdk return values

`n` = all bytes ACKed · `PICO_ERROR_GENERIC` = address NACKed · fewer than `n` = data byte NACKed · `PICO_ERROR_TIMEOUT` = deadline

---

## 20. Further reading

- **[UM10204, I2C-bus specification and user manual](https://www.nxp.com/docs/en/user-guide/UM10204.pdf)** (NXP). The authoritative source for everything in this course.
- **[AN10441, level shifting techniques in I2C-bus design](https://www.nxp.com/docs/en/application-note/AN10441.pdf)** (NXP). The one-MOSFET level shifter.
- **[SLVA689, I2C bus pull-up resistor calculation](https://www.ti.com/lit/an/slva689/slva689.pdf)** (Texas Instruments). Pull-up sizing in more depth.
- **[RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)**, the I2C chapter, for the controller's registers.
- **In this repo:** [PIO from zero](../pio/README.md) for how EmuWire's target is built, [`firmware/pio/i2c_slave.pio`](../../firmware/pio/i2c_slave.pio) for the program itself, and [`tests/rig/main.c`](../../tests/rig/main.c) for the controller-side tests.
