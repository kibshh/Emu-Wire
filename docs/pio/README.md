# PIO from zero

A course on the RP2350's Programmable I/O, for someone who has never used it. It covers the programming model, the hardware underneath, and what actually happens on the pins.

By the end you should be able to read any `.pio` file, write your own, wire it up from C, and predict its timing to the clock cycle. You should also know the traps, several of which this project fell into on real hardware.

Every instruction, directive and SDK call in this course was checked against `pioasm` and pico-sdk **2.3.0**. Every complete example program assembles; the shorter snippets are excerpts from real programs. Where the RP2350 differs from the RP2040, the course says so.

**You need:** basic C, and knowing what a GPIO, a clock and an interrupt are. No PIO knowledge at all.

---

## Contents

1. [Why PIO exists](#1-why-pio-exists)
2. [The hardware, piece by piece](#2-the-hardware-piece-by-piece)
3. [Time: cycles, the clock divider, and stalls](#3-time-cycles-the-clock-divider-and-stalls)
4. [Your first program: a square wave](#4-your-first-program-a-square-wave)
5. [The instruction set](#5-the-instruction-set)
6. [Delay and side-set: two things in one cycle](#6-delay-and-side-set-two-things-in-one-cycle)
7. [Pins](#7-pins)
8. [Data in and out: shift registers and FIFOs](#8-data-in-and-out-shift-registers-and-fifos)
9. [Flow control, IRQ flags, and talking to the CPU](#9-flow-control-irq-flags-and-talking-to-the-cpu)
10. [A complete example: a UART transmitter](#10-a-complete-example-a-uart-transmitter)
11. [The C side in full](#11-the-c-side-in-full)
12. [Budgeting: cycles and the 32 instructions](#12-budgeting-cycles-and-the-32-instructions)
13. [Case study: EmuWire's I2C slave](#13-case-study-emuwires-i2c-slave)
14. [Pitfalls](#14-pitfalls)
15. [Debugging PIO](#15-debugging-pio)
16. [Cheat sheet](#16-cheat-sheet)
17. [Further reading](#17-further-reading)

---

## 1. Why PIO exists

### The problem

Say you need a protocol the chip has no peripheral for. Or one the chip *has*, but only as a master, when you need a slave. The classic answer is **bit-banging**: the CPU toggles pins in a loop.

Bit-banging works until timing matters. A CPU loop is never exact:

- an interrupt can arrive mid-loop and steal microseconds
- the code may run from flash, and a cache miss stalls the CPU
- the other core competing for the bus slows memory access

The result is **jitter**: edges that land a little early or late, differently every time. For many protocols that is fatal.

Fixed peripherals (UART, SPI, I2C blocks) have exact timing, but they do one protocol, one way. You get what the silicon designer chose.

### The answer

PIO puts small, very simple processors **right next to the GPIO pins**. Each one:

- executes **exactly one instruction per clock cycle**, every cycle, with no caches and no interrupts to disturb it
- can read and write pins directly
- talks to the CPU through small queues (FIFOs)
- runs completely independently of the CPU once started

You write a short program for it. It then produces pin activity with timing as exact as dedicated hardware, but with any shape you like.

### What PIO is not

PIO is deliberately primitive. It has:

- **no arithmetic** apart from "decrement and test for zero"
- **no memory** apart from two 32-bit scratch registers and two shift registers
- **no comparisons** apart from "is it zero" and "are these two registers equal"
- **room for 32 instructions** per block, shared by four state machines

So PIO is not a coprocessor for logic. It is a precise **timekeeper**.

> **The split nearly every serious PIO design ends up with:** PIO handles the timing, the CPU makes the decisions. When PIO needs a decision, it stops and waits for the CPU to answer. Lesson 9 shows how; lesson 13 shows a real design built entirely around it.

---

## 2. The hardware, piece by piece

### The big picture

```
RP2350
├── PIO0 ── 32-word instruction memory ── SM0  SM1  SM2  SM3 ── 8 IRQ flags
├── PIO1 ── 32-word instruction memory ── SM0  SM1  SM2  SM3 ── 8 IRQ flags
└── PIO2 ── 32-word instruction memory ── SM0  SM1  SM2  SM3 ── 8 IRQ flags
```

| | RP2350 | RP2040, for comparison |
|---|---|---|
| PIO blocks | **3** | 2 |
| State machines per block | 4 | 4 |
| State machines in total | **12** | 8 |
| Instruction memory | **32 instructions per block**, shared by its 4 SMs | same |
| IRQ flags per block | 8 | 8 |
| FIFO depth | 4 words each way (8 if joined) | same |
| PIO version | **1** | 0 |

A **block** is one PIO instance (`pio0`, `pio1`, `pio2`). A **state machine** (SM) is one of the little processors inside it.

### Inside a state machine

```
                      ┌───────────────── state machine ──────────────────┐
  CPU writes ──► TX FIFO (4) ──► OSR ──OUT──► pins / pindirs / X / Y ... │
                      │                                                   │
                      │  PC (program counter, 5 bits)                     │
                      │  X, Y (32-bit scratch registers)                  │
                      │  clock divider                                    │
                      │  config: pin groups, shift directions, wrap ...   │
                      │                                                   │
  CPU reads  ◄── RX FIFO (4) ◄── ISR ◄──IN─── pins / X / Y / NULL ...     │
                      └───────────────────────────────────────────────────┘
```

| Part | What it is |
|---|---|
| **PC** | Which instruction runs next. 5 bits, because memory is 32 words. |
| **X, Y** | Two 32-bit scratch registers. Used as loop counters, for comparisons, or to hold a value. |
| **OSR** (output shift register) | 32 bits waiting to be shifted **out**, a few bits at a time. Refilled from the TX FIFO. |
| **ISR** (input shift register) | Collects bits shifted **in**. Pushed to the RX FIFO when full. |
| **TX FIFO** | Queue from the CPU to the SM. 4 words. |
| **RX FIFO** | Queue from the SM to the CPU. 4 words. |
| **Clock divider** | Slows this SM down relative to the system clock. |
| **Configuration** | Which pins each instruction touches, shift directions, wrap points, and more. Set from C before starting. |

The OSR and ISR each have a **shift counter** that remembers how many bits have been shifted. That counter is what drives autopull and autopush (lesson 8).

### What the four state machines in a block share

- **The 32-word instruction memory.** Four SMs can run the same program (each with its own PC) or different programs, but all programs in a block must fit in 32 instructions *together*. This is the tightest constraint in most designs.
- **The 8 IRQ flags.** Any SM can set, clear or wait on any of them. That's how SMs synchronise with each other and with the CPU.

Everything else — registers, FIFOs, clock divider, pin configuration — is per state machine.

### How PIO connects to the rest of the chip

```
           ┌─ bus registers ─┐
  CPU ─────┤ TXF / RXF       │  FIFO access
           │ CTRL            │  enable, restart
           │ SMx_ADDR        │  read the current PC
           │ SMx_INSTR       │  execute one instruction now ("exec")
           │ FDEBUG          │  sticky stall/overflow flags
           │ IRQ, INTE/INTS  │  flags and interrupt routing
           └─────────────────┘
  DMA ──── one DREQ per FIFO: DMA can feed or drain a SM with no CPU at all
  NVIC ─── two interrupt lines per block (PIOx_IRQ_0, PIOx_IRQ_1)
  GPIO ─── through each pin's function select, and its pad
```

**GPIO function select.** Every GPIO has a multiplexer choosing who drives it: SIO (plain software GPIO), UART, SPI, PWM, PIO0, PIO1, PIO2, and others. A PIO can only **drive** a pin whose function select is set to that PIO. `pio_gpio_init()` does that.

**Reading is different:** any PIO can read **any** GPIO's input at any time, whatever its function select.

**The pad** is the electrical part: pull-up/pull-down, drive strength, slew rate, input enable. PIO doesn't control those. You set them from C with the normal `gpio_*` functions.

---

## 3. Time: cycles, the clock divider, and stalls

### One instruction, one cycle

Every PIO instruction takes **exactly one cycle**, jumps included. Nothing is pipelined in a way you can observe, and nothing is cached. If a program is eight instructions in a loop, the loop takes eight cycles. Always.

That is the whole point. You can calculate the timing of a PIO program on paper and the hardware will match it to the cycle.

### Delay: `[n]`

Any instruction can carry a delay, written `[n]`. After the instruction completes, the SM waits **n extra cycles** before the next one.

```
set pins, 1 [9]     ; 1 cycle for the SET, then 9 idle cycles = 10 cycles total
```

Delays are free: they cost no instruction memory. The maximum depends on side-set, which shares the same bits (lesson 6). Without side-set, the maximum is 31.

### The clock divider

By default a SM runs at the full system clock — 150 MHz on the RP2350, so 6.67 ns per cycle. The **clock divider** slows one SM down:

```
SM cycle rate = clk_sys / divider        divider: 1.0 to 65535.996, in steps of 1/256
```

So with `divider = 15.0`, the SM runs at 10 MHz and each cycle is 100 ns.

Each SM has its own divider. Four SMs in a block can run at four different speeds.

> **Fractional dividers jitter.** With a divider of 2.5, the SM alternates between 2 and 3 system cycles per PIO cycle, and averages 2.5. The *average* rate is exact, but individual edges wobble by one system cycle. Use whole-number dividers when edge timing matters.

### Stalls

Some instructions can't complete straight away. They **stall**: they stay on the same instruction, cycle after cycle, until their condition is true.

| Instruction | Stalls until |
|---|---|
| `WAIT` | the pin, IRQ flag or JMP pin reaches the given level |
| `PULL block` | the TX FIFO has data |
| `PUSH block` | the RX FIFO has room |
| `OUT` with autopull, OSR empty | the TX FIFO has data |
| `IN` with autopush, ISR full | the RX FIFO has room |
| `IRQ wait` | the flag it raised is cleared by someone else |

Three facts about stalls that you will rely on:

1. **A stalled SM leaves its pins exactly as they are.** If it was holding a line low, it keeps holding it. That is a feature: lesson 13 is built on it.
2. **The delay starts after the stall ends.** `wait 1 pin 0 [3]` waits for the pin, *then* waits 3 more cycles.
3. **Side-set happens at the start, even if the instruction then stalls** (lesson 6).

A stall is the only source of variable timing in PIO. Everything else is fixed.

### Wrap: a free jump

At the end of a program you usually want to loop back. A `JMP` would cost an instruction. Instead, mark the loop with `.wrap_target` and `.wrap`. When the PC reaches the instruction marked `.wrap`, it continues at `.wrap_target` **in the same cycle**, at no cost.

```
.wrap_target
    set pins, 1
    set pins, 0
.wrap               ; back to "set pins, 1" — no instruction, no extra cycle
```

---

## 4. Your first program: a square wave

### The program

```
.program square
.wrap_target
    set pins, 1 [9]      ; pin high, then wait 9 more cycles  (10 cycles)
    set pins, 0 [9]      ; pin low,  then wait 9 more cycles  (10 cycles)
.wrap
```

One period is 20 cycles. At the full 150 MHz, that's a 7.5 MHz square wave. With a divider of 150, each cycle is 1 µs and the wave is 50 kHz.

### What the assembler makes of it

`pioasm` turns each instruction into a 16-bit word. You can see them in the generated header:

```
0xe901, //  0: set    pins, 1                [9]
0xe900, //  1: set    pins, 0                [9]
```

Two words of the block's 32.

### The C side, minimal

```c
#include "hardware/pio.h"
#include "square.pio.h"          // generated by pioasm from square.pio

int main(void) {
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &square_program);   // load into memory
    uint sm = pio_claim_unused_sm(pio, true);              // pick a free SM

    pio_sm_config c = square_program_get_default_config(offset);
    sm_config_set_set_pins(&c, 15, 1);         // SET writes 1 pin, starting at GP15
    sm_config_set_clkdiv(&c, 150.0f);          // 1 µs per cycle

    pio_gpio_init(pio, 15);                              // GP15 is now driven by pio0
    pio_sm_set_consecutive_pindirs(pio, sm, 15, 1, true); // and it is an output

    pio_sm_init(pio, sm, offset, &c);   // apply config, PC = start of program
    pio_sm_set_enabled(pio, sm, true);  // go

    while (true) { }                    // the CPU is free; the pin keeps toggling
}
```

And in `CMakeLists.txt`, so the build assembles the `.pio` and gives you `square.pio.h`:

```cmake
pico_generate_pio_header(my_app ${CMAKE_CURRENT_LIST_DIR}/square.pio)
target_link_libraries(my_app pico_stdlib hardware_pio)
```

Notice the CPU's `while` loop does nothing. The state machine keeps running on its own, forever, with exact timing.

---

## 5. The instruction set

### How an instruction is encoded

Every instruction is 16 bits:

```
 15 14 13 │ 12 11 10  9  8 │  7  6  5  4  3  2  1  0
 opcode   │ delay/side-set │ arguments (depend on the opcode)
```

- **3 bits of opcode**, so 8 opcodes (and `MOV` doubles as `NOP`, so 9 mnemonics).
- **5 bits for delay and side-set together.** How they're split is lesson 6.
- **8 bits of arguments.**

### The nine instructions

#### `JMP` — jump, maybe

```
jmp <condition>, <label>
```

| Condition | Jumps when |
|---|---|
| *(none)* | always |
| `!x` / `!y` | X / Y is zero |
| `x--` / `y--` | X / Y is **not** zero — and then decrements it, whether or not it jumped |
| `x!=y` | X and Y differ |
| `pin` | the **JMP pin** is high (one pin, chosen in config) |
| `!osre` | the OSR is **not** empty (still has bits to shift out) |

`x--` is the loop instruction. With `set x, 7`, a `jmp x-- loop` at the end of a loop body runs the body **8 times**: it jumps while X is 7, 6, … 1, and falls through once it sees 0.

```
    set x, 7
loop:
    ; ... body, runs 8 times ...
    jmp x-- loop
```

There is no `jmp !pin`. To branch on "pin low", jump on `pin` to skip over a jump.

#### `WAIT` — stall until something happens

```
wait <0|1> gpio <n>        ; absolute GPIO number
wait <0|1> pin <n>         ; n-th pin of the IN group
wait <0|1> irq <n> [rel]   ; IRQ flag
wait <0|1> jmppin [+ n]    ; the JMP pin (+0..3)        — RP2350 only
```

- `wait 1 ...` stalls until the source is **1**. `wait 0 ...` until it is **0**.
- **If the condition is already true, it does not wait at all.** `wait 1 pin 0` on a pin that's already high returns immediately. To catch a **rising edge**, wait for 0 first, then for 1. This is the most common PIO bug.
- `wait 1 irq n` also **clears** the flag when it fires, so one SM can hand a token to another.
- `pin` goes through the IN pin group, including the RP2350's IN pin count mask (lesson 7). `gpio` and `jmppin` do not.

#### `IN` — shift bits into the ISR

```
in <source>, <count>        ; count: 1 to 32
```

Sources: `pins` (the IN group), `x`, `y`, `null` (zeros), `isr`, `osr`.

`in pins, 1` samples one pin and shifts it into the ISR. Which end it enters from depends on the shift direction (lesson 8 — read that section; everyone gets this wrong once).

#### `OUT` — shift bits out of the OSR

```
out <destination>, <count>  ; count: 1 to 32
```

Destinations: `pins`, `pindirs`, `x`, `y`, `null` (discard), `isr`, `pc`, `exec`.

- `out pins, 1` puts the next OSR bit on one pin.
- `out pc, 5` **jumps to an address taken from the data** — a computed jump. The CPU can choose where the program goes next.
- `out exec, 16` **executes** the next 16 bits as an instruction.

#### `PUSH` — move the ISR to the RX FIFO

```
push [iffull] [block|noblock]
```

- Default is `block`: stall if the RX FIFO is full.
- `noblock` on a full FIFO drops the data rather than stalling.
- `iffull` does nothing unless the ISR has reached its threshold.
- After a push the ISR is cleared to 0 and its counter reset.

#### `PULL` — refill the OSR from the TX FIFO

```
pull [ifempty] [block|noblock]
```

- Default is `block`: stall until the CPU sends something.
- `noblock` on an empty FIFO **copies X into the OSR** instead. A handy trick: put a default value in X, and the SM keeps outputting it whenever the CPU has nothing new.
- `ifempty` does nothing unless the OSR has reached its threshold.
- A `PULL` overwrites the whole OSR, discarding any bits not yet shifted out.

#### `MOV` — copy between registers

```
mov <destination>, [op]<source>
```

| Destinations | Sources | Ops |
|---|---|---|
| `pins`, `x`, `y`, `exec`, `pc`, `isr`, `osr`, and `pindirs` (RP2350) | `pins`, `x`, `y`, `null`, `status`, `isr`, `osr` | `!` or `~` invert, `::` reverse the bit order |

- `mov x, pins` reads the whole IN pin group in one go.
- `mov y, ::x` reverses X's 32 bits into Y. Free endianness conversion.
- `status` is all-ones or all-zeros depending on a FIFO-level test you choose in config.
- **`nop` is really `mov y, y`.** It does nothing for one cycle, and you'll mostly use it to carry a side-set.

#### `IRQ` — set, clear, or wait on a flag

```
irq [set|nowait] <n> [rel]     ; raise flag n, carry on
irq wait <n> [rel]             ; raise flag n, stall until someone clears it
irq clear <n> [rel]            ; clear flag n
irq next|prev ...              ; same, in the neighbouring PIO block — RP2350 only
```

`set` and `nowait` mean the same thing. `rel` makes the flag number relative to the SM (lesson 9).

#### `SET` — write a small constant

```
set <destination>, <value>     ; value: 0 to 31
```

Destinations: `pins`, `pindirs`, `x`, `y`.

- Only **5 bits** of data, so only up to **5 pins**, and X/Y only up to 31.
- `set x, 7` is the usual way to start a loop counter.
- `set pins` drives a pin to a constant without the OSR, so it doesn't disturb data you're shifting out.

### RP2350-only instructions need a line at the top

`wait ... jmppin`, `mov pindirs, ...`, `irq next/prev`, and the RX-FIFO-as-registers forms of `mov` need:

```
.pio_version 1
```

Without it, `pioasm` refuses them:

```
error: PIO version 1 is required for 'mov pindirs'
```

---

## 6. Delay and side-set: two things in one cycle

### The idea

**Side-set** changes up to five pins *at the same time* as whatever the instruction is doing. One instruction can shift a data bit out **and** move a clock edge, in a single cycle, with no extra instruction.

```
.program spi_tx
.side_set 1                   ; one side-set pin: the clock
.wrap_target
    out pins, 1   side 0 [1]  ; data bit out, clock low,  2 cycles
    nop           side 1 [1]  ; nothing else, clock high, 2 cycles
.wrap
```

That's a working SPI transmitter in two instructions: data changes while the clock is low, and the receiver samples it while the clock is high. 4 cycles per bit.

### The 5 shared bits

Side-set and delay share the same 5 bits of the instruction. The more side-set pins you declare, the less room is left for delay:

| Declaration | Side-set bits | Max delay | Every instruction must have `side`? |
|---|---|---|---|
| *(none)* | 0 | **31** | — |
| `.side_set 1` | 1 | **15** | **yes** |
| `.side_set 1 opt` | 1 + 1 enable bit | **7** | no |
| `.side_set 2` | 2 | 7 | yes |
| `.side_set 2 opt` | 2 + 1 | **3** | no |

All checked against `pioasm`. With a non-`opt` side-set, leaving out `side` is an error:

```
error: instruction requires 'side' to specify side set value
```

`opt` costs a bit of delay but lets instructions leave the side-set pins alone, which is usually what you want.

### Pins or pin directions

```
.side_set 1                    ; side-set writes pin VALUES
.side_set 1 opt pindirs        ; side-set writes pin DIRECTIONS
```

The `pindirs` form is how you drive an open-drain line, like an I2C clock, with side-set (lesson 7).

### When side-set happens — this matters

**Side-set takes effect at the very start of the instruction, even if that instruction then stalls.** The delay comes at the end.

```
wait 1 jmppin   side 0    ; side-set happens NOW, then the WAIT may stall for ages
```

That lets you write "release the clock, and wait until it has actually gone high" as **one** instruction. EmuWire's I2C slave does exactly that (lesson 13).

The flip side: if you only wanted the side-set to happen *after* a wait, you need two instructions.

---

## 7. Pins

### Every way an instruction touches a pin

| Mechanism | Reads or writes | Pins | Set in C with |
|---|---|---|---|
| `OUT pins` / `OUT pindirs` | writes | the **OUT group**: base + count (1–32) | `sm_config_set_out_pins` |
| `SET pins` / `SET pindirs` | writes | the **SET group**: base + count (1–5) | `sm_config_set_set_pins` |
| side-set | writes | the **side-set group**: base + count (0–5) | `sm_config_set_sideset_pins` |
| `MOV pins` | writes | the OUT group | — |
| `IN pins`, `MOV x, pins` | reads | the **IN group**: base (+ count on RP2350) | `sm_config_set_in_pins` |
| `WAIT pin n` | reads | the n-th pin of the IN group | — |
| `JMP pin`, `WAIT jmppin` | reads | the **JMP pin**: one pin | `sm_config_set_jmp_pin` |
| `WAIT gpio n` | reads | GPIO n, absolute | — |

Pins in a group are **numbered from its base**. With the IN base at GP4, `pin 0` is GP4 and `pin 1` is GP5. Groups wrap around after 32.

The groups are independent. They can overlap, as in EmuWire's I2C slave, where IN, SET and OUT all start at SDA.

### Pin values and pin directions

Each pin has two separate settings that PIO can write:

- **value** — 1 or 0, what the pin would drive
- **direction** — 1 = output (drives its value), 0 = input (drives nothing, floats)

`set pins, 1` changes the value. `set pindirs, 1` makes the pin an output. A pin only actually drives when its direction is output.

### Before PIO can drive a pin

Three things, in C, before starting the SM:

1. `pio_gpio_init(pio, pin)` — hand the pin's function select to this PIO.
2. Set its direction: `pio_sm_set_consecutive_pindirs(pio, sm, pin, count, is_out)`.
3. Point the right group at it in the config.

Forget step 1 and the program runs perfectly while the pin does nothing.

### Open drain: never drive high

On shared buses like I2C, devices must **never drive a line high**. A pull-up resistor makes the line high; each device can only pull it low. Two devices driving opposite levels would short each other.

PIO pins are push-pull, but you can get open-drain behaviour with **pin directions**:

```
Set the pin's VALUE to 0, once, at startup. Never change it.

  set pindirs, 1   →  output, driving the stored 0   →  line LOW
  set pindirs, 0   →  input, driving nothing          →  pull-up makes it HIGH
```

Now the program can only ever pull low or let go. In C, at startup:

```c
pio_sm_set_pins_with_mask(pio, sm, 0, pin_mask);      // values: 0, forever
pio_sm_set_pindirs_with_mask(pio, sm, 0, pin_mask);   // start released
```

### The input synchroniser

Every GPIO input passes through two flip-flops before PIO sees it. That protects against glitches when a pin changes right at a clock edge, but it adds **2 system clock cycles of delay**: PIO sees a pin change about 13 ns late at 150 MHz.

It can be bypassed per pin (the `INPUT_SYNC_BYPASS` register) for the fastest response. Do so only for signals already synchronised to `clk_sys`. Otherwise you trade delay for occasional garbage.

### When two state machines drive one pin

If two SMs in the same block write the same pin in the same cycle, the **higher-numbered SM wins**. Across blocks there's no contest: the pin's function select picks one block.

### RP2350: the IN pin count — and how it bit us

On the RP2350, the IN group has a **count**, and pins beyond it read as **0**. That lets `mov x, pins` read just the pins you care about, instead of all 32 with the rest as noise.

The register description says what it applies to:

> Set the number of pins which are not masked to 0 when read by an IN PINS, **WAIT PIN** or MOV x, PINS instruction.

So the mask also hides pins from **`WAIT PIN`**. EmuWire's I2C slave set the count to 1 so `mov` would read SDA alone, then waited on SCL with `wait 1 pin 1`. SCL was masked to 0, so the SM waited forever. The bus looked dead. The fix was `wait 1 jmppin`, which doesn't go through the IN group at all. Lesson 14 has the details.

### RP2350B: the GPIO base

The RP2350B package has 48 GPIOs, but a PIO block addresses 32 pins at a time. `pio_set_gpio_base(pio, 16)` moves a block's window to GP16–47. The RP2350A (Pico 2) has 30 GPIOs, so it never needs this.

---

## 8. Data in and out: shift registers and FIFOs

### The two paths

```
 OUT path:   CPU ── pio_sm_put() ──► TX FIFO ──► OSR ── out ──► pins, X, Y, PC ...
 IN path:    pins, X, Y ... ── in ──► ISR ──► RX FIFO ── pio_sm_get() ──► CPU
```

### Shift direction — read this twice

Both shift registers are 32 bits, and each has a **direction**: left or right. The direction decides which end bits enter from or leave by.

**The two registers do opposite things for the same word "left".**

```
IN, shift LEFT  — new bits enter at bit 0 and push older bits up.
                  After 8 bits, the byte sits in bits 7..0.

    before:  ........ ........ ........ ........
    in 8:    00000000 00000000 00000000 [byte  ]   ← read with (uint8_t)word

IN, shift RIGHT — new bits enter at bit 31 and push older bits down.
                  After 8 bits, the byte sits in bits 31..24.

    in 8:    [byte  ] 00000000 00000000 00000000   ← read with word >> 24

OUT, shift LEFT  — bits leave from bit 31, the top.   MSB first.
OUT, shift RIGHT — bits leave from bit 0, the bottom. LSB first.
```

So "shift left" means **MSB first** for both, but the received byte ends up at the **bottom** of the word while a byte you send must be placed at the **top**.

> **Found on the bench:** EmuWire's I2C slave received with IN shifting left, and read the byte with `word >> 24`. The byte was in bits 7..0. Every address read as `0x00`, and the slave ignored its own address. Raspberry Pi's own SPI example uses the same setting and reads the low byte.

### Thresholds, autopush and autopull

You *can* move data with explicit `push` and `pull` instructions. Usually you let the hardware do it:

- **Autopush:** after an `IN`, if the ISR's shift count has reached the **push threshold**, the ISR is pushed to the RX FIFO automatically.
- **Autopull:** when the OSR's shift count has reached the **pull threshold**, the next `OUT` refills the OSR from the TX FIFO first. If the FIFO is empty, that `OUT` stalls.

Both are free: no instruction needed. You configure them in C:

```c
sm_config_set_in_shift(&c,  /*shift_right=*/false, /*autopush=*/true, /*threshold=*/8);
sm_config_set_out_shift(&c, /*shift_right=*/false, /*autopull=*/true, /*threshold=*/8);
```

or with the RP2350 directives at the top of the `.pio` file:

```
.pio_version 1
.in 8 left auto 8          ; IN: 8 pins, shift left, autopush at 8
.out 1 right               ; OUT: 1 pin, shift right, no autopull
```

`pioasm` turns those into the same `sm_config_set_*` calls inside the generated default config.

### Autopull counts bits — all of them

Autopull refills when the **total** bits shifted out reach the threshold. If you take 4 bits of a word for one purpose and then shift out a byte, the first 4 bits of that byte come from the old word's leftovers. The refill only happens once 8 bits *in total* have gone.

When one pulled word carries several fields, turn autopull **off** and use explicit `pull` at the start of each phase. A `PULL` overwrites the whole OSR, so whatever the previous phase left behind is gone.

### Asking "is there anything left?"

`jmp !osre, label` jumps while the OSR still has bits to shift before reaching the threshold. Use it to loop over however many bits the CPU actually sent.

### FIFOs: what happens at the edges

Each FIFO holds **4 words**.

| Situation | State machine side | CPU side |
|---|---|---|
| TX FIFO empty | `PULL block` / autopull `OUT` stalls | — |
| TX FIFO full | — | `pio_sm_put()` **drops the word** and sets a debug flag |
| RX FIFO full | `PUSH block` / autopush `IN` stalls | — |
| RX FIFO empty | — | `pio_sm_get()` returns garbage and sets a debug flag |

The non-blocking CPU calls don't wait, and they don't warn. Use the `_blocking` versions, or check first with `pio_sm_is_tx_fifo_full()` / `pio_sm_is_rx_fifo_empty()`.

### Joining FIFOs

If data only flows one way, join the two FIFOs into one 8-deep queue:

```c
sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);   // 8-deep TX, no RX
sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);   // 8-deep RX, no TX
```

### RP2350: the RX FIFO as four registers

The RP2350 can turn the RX FIFO into **four registers** that the SM reads or writes by index, with `.fifo` and new `mov` forms:

```
.pio_version 1
.fifo putget
    mov rxfifo[y], isr      ; store the ISR in register number Y
    mov osr, rxfifo[0]      ; load register 0 into the OSR
```

The join mode decides who can access them: `txput` (SM writes, CPU reads), `txget` (CPU writes, SM reads), `putget` (SM only, as scratch storage). Useful for status words or lookup values the SM needs without consuming a FIFO entry.

### DMA

Each FIFO has a DMA request line. A DMA channel can feed the TX FIFO from memory, or drain the RX FIFO into memory, paced automatically by the SM. Long streams then need no CPU at all.

---

## 9. Flow control, IRQ flags, and talking to the CPU

### Labels and `public`

Labels mark jump targets. Mark one `public` and the generated header exports its position:

```
public rx_path:
    wait 1 jmppin
```

```c
#define i2c_slave_offset_rx_path 14u    // generated
```

The number is the instruction's position **inside the program**. The program can be loaded anywhere in the block, so add the load offset to get a real address:

```c
uint addr = offset + i2c_slave_offset_rx_path;
```

`.define public NAME value` exports constants the same way, as `program_NAME`.

### Letting the CPU choose where to go: `out pc`

```
pull block      ; wait for a word from the CPU
out pc, 5       ; jump to the address in its top 5 bits
```

The CPU sends the address of the next piece of program. That's a full multi-way branch in **one instruction**, with the decision made in C. It also means one word can carry several things at once: a flag, a destination, a data byte.

### Running an instruction from outside: `exec`

- From the CPU: `pio_sm_exec(pio, sm, pio_encode_jmp(offset + label));` runs one instruction immediately. Useful to force a SM back to a known point.
- From the program: `out exec, 16` or `mov exec, x` runs an instruction taken from data.

### IRQ flags

Each block has **8 flags**, shared by its four SMs.

- `irq set n` raises flag n.
- `irq wait n` raises it, then stalls until someone clears it. A handshake.
- `irq clear n` clears it.
- `wait 1 irq n` stalls until flag n is set, then clears it.

**Flags 0–3** can interrupt the CPU. **Flags 4–7** cannot, but the CPU can still read and clear them. That makes 4–7 useful as a label or status that the CPU checks inside an interrupt raised for some other reason.

### `rel`: one program, four state machines

Four SMs running the same program shouldn't all use flag 0. With `rel`, the flag number becomes relative to the SM:

```
irq set 0 rel    →  SM0 sets flag 0, SM1 flag 1, SM2 flag 2, SM3 flag 3
irq set 4 rel    →  SM0 sets flag 4, SM1 flag 5, SM2 flag 6, SM3 flag 7
```

The rule: bit 2 of the number stays put, and the SM number is added to the low two bits, wrapping inside groups of four.

### RP2350: flags in the next block

`irq next set 0` and `irq prev set 0` reach the flags of the neighbouring PIO block. SMs in different blocks can then synchronise directly.

### Interrupting the CPU

A block has two interrupt lines to the CPU. Each can be enabled for any mix of:

- flags 0–3
- each SM's "RX FIFO not empty"
- each SM's "TX FIFO not full"

```c
pio_set_irq0_source_enabled(pio, pis_interrupt0 + sm, true);  // flag (0 + sm)
irq_set_exclusive_handler(pio_get_irq_num(pio, 0), my_handler);
irq_set_enabled(pio_get_irq_num(pio, 0), true);
```

The handler must clear the flag (`pio_interrupt_clear(pio, n)`), or it fires again immediately.

**Interrupt enables are per core.** The core that calls `irq_set_enabled` is the one that gets interrupted. To have core 1 serve a PIO, make the call from core 1.

### The pattern: stall and ask

Putting the last few pieces together gives the most useful PIO pattern there is:

```
    ; ... something happened that needs a decision ...
    irq set 0 rel        ; wake the CPU
    pull block           ; and stall here until it answers
    out pc, 5            ; go wherever it said
```

The SM freezes at exactly the right moment, the CPU takes as long as it needs, and the SM continues with exact timing again. Since a stalled SM holds its pins, it can even hold a bus still while it waits. Lesson 13 is a full design built on this.

---

## 10. A complete example: a UART transmitter

A UART frame: line idles high, then a **start bit** (low), **8 data bits** LSB first, a **stop bit** (high). Every bit lasts the same time.

```
.program uart_tx
.side_set 1 opt
    pull       side 1 [7]   ; stop bit / idle: line high. Wait for a byte.
    set x, 7   side 0 [7]   ; start bit: line low. 8 bits to send.
bitloop:
    out pins, 1             ; one data bit
    jmp x-- bitloop   [6]   ; 8 data bits
```

Four instructions. Counting cycles:

| Part | Instructions | Cycles |
|---|---|---|
| Stop bit | `pull side 1 [7]` | 1 + 7 = **8** |
| Start bit | `set x, 7 side 0 [7]` | 1 + 7 = **8** |
| Each data bit | `out` (1) + `jmp [6]` (1 + 6) | **8** |

Every bit is exactly 8 cycles. So for a baud rate:

```
divider = clk_sys / (8 × baud)        e.g. 150 MHz / (8 × 115200) = 162.76
```

Notice how much the design uses what the earlier lessons covered:

- **Side-set drives the start and stop levels**, so they cost no extra instruction.
- **`pull` with a delay** is both "wait for data" and "the stop bit". If the CPU has nothing to send, the SM sits there with the line high, which is exactly idle.
- **The loop counter starts at 7** for 8 iterations.
- The data pin and the side-set pin are the **same GPIO**: OUT and side-set both point at the TX pin.

For LSB first, set the OUT shift direction to **right**, with autopull off (the program pulls explicitly).

---

## 11. The C side in full

### From `.pio` to header

The build runs `pioasm` on each `.pio` file listed with `pico_generate_pio_header`. The header contains:

| Generated | What it is |
|---|---|
| `static const uint16_t <prog>_program_instructions[]` | the encoded instructions |
| `static const pio_program_t <prog>_program` | the program, ready for `pio_add_program`, including `.length` |
| `#define <prog>_wrap_target`, `<prog>_wrap` | the wrap points |
| `#define <prog>_offset_<label>` | each `public` label's position |
| `#define <prog>_<NAME>` | each `.define public` |
| `<prog>_program_get_default_config(offset)` | a config with wrap, side-set and directives already applied |
| your `% c-sdk { ... %}` block | copied verbatim — the usual home for an init function |

`pioasm` also checks the budget: a program longer than 32 instructions fails to assemble.

### Loading

```c
uint offset = pio_add_program(pio, &prog_program);             // panics if no room
bool fits   = pio_can_add_program(pio, &prog_program);         // check first
uint sm     = pio_claim_unused_sm(pio, true);                  // true = panic if none
// or all three at once, searching every block:
bool ok = pio_claim_free_sm_and_add_program(&prog_program, &pio, &sm, &offset);
```

### Configuring

| Call | Sets |
|---|---|
| `sm_config_set_out_pins(&c, base, count)` | OUT group |
| `sm_config_set_set_pins(&c, base, count)` | SET group (count ≤ 5) |
| `sm_config_set_in_pins(&c, base)` | IN group base |
| `sm_config_set_in_pin_count(&c, count)` | IN mask (RP2350) |
| `sm_config_set_sideset_pins(&c, base)` | side-set group base |
| `sm_config_set_jmp_pin(&c, pin)` | the JMP pin |
| `sm_config_set_in_shift(&c, right, autopush, threshold)` | ISR behaviour |
| `sm_config_set_out_shift(&c, right, autopull, threshold)` | OSR behaviour |
| `sm_config_set_clkdiv(&c, div)` | clock divider (float) |
| `sm_config_set_clkdiv_int_frac8(&c, int, frac)` | same, exact integer + n/256 |
| `sm_config_set_fifo_join(&c, mode)` | FIFO join |
| `sm_config_set_wrap(&c, target, wrap)` | wrap points (default config already has them) |

The side-set *count* and `opt`/`pindirs` come from the `.side_set` line, through the default config. You only add the base pin.

### Starting

```c
pio_gpio_init(pio, pin);                                   // each pin it drives
pio_sm_set_pins_with_mask(pio, sm, values, mask);          // initial values
pio_sm_set_pindirs_with_mask(pio, sm, dirs, mask);         // initial directions
pio_sm_init(pio, sm, initial_pc, &c);                      // apply; SM stays stopped
pio_sm_set_enabled(pio, sm, true);                         // run
```

`pio_sm_init` clears the FIFOs, resets the shift counters, applies the config and sets the PC. The `initial_pc` doesn't have to be the start of the program: pass `offset + prog_offset_<label>` to start at a label.

To start several SMs on the same cycle: `pio_enable_sm_mask_in_sync(pio, mask)`.

### Running

| Call | Does |
|---|---|
| `pio_sm_put(pio, sm, word)` | write to TX FIFO; **dropped if full** |
| `pio_sm_put_blocking(pio, sm, word)` | write, waiting for room |
| `pio_sm_get(pio, sm)` | read from RX FIFO; **garbage if empty** |
| `pio_sm_get_blocking(pio, sm)` | read, waiting for data |
| `pio_sm_is_tx_fifo_full`, `pio_sm_is_rx_fifo_empty`, `pio_sm_get_rx_fifo_level` | FIFO state |
| `pio_sm_exec(pio, sm, instr)` | run one instruction now |
| `pio_sm_get_pc(pio, sm)` | where the SM is — the first thing to check when it's stuck |
| `pio_sm_clear_fifos`, `pio_sm_restart` | reset parts of the state |

### Interrupts

```c
int irqn = pio_get_irq_num(pio, 0);                          // PIOx_IRQ_0
pio_set_irq0_source_enabled(pio, pis_interrupt0 + sm, true); // which flag
irq_set_exclusive_handler(irqn, handler);
irq_set_enabled(irqn, true);                                 // on THIS core
```

In the handler: check which flag (`pio_interrupt_get`), clear it (`pio_interrupt_clear`), do the work. If the handler runs while a bus is being held, mark it `__not_in_flash_func` so a flash cache miss can't add delay.

---

## 12. Budgeting: cycles and the 32 instructions

### The cycle budget

First question for any protocol: how many SM cycles per bit?

```
cycles per bit = (clk_sys / divider) / bit rate
```

At 150 MHz and divider 1, a 1 MHz signal gives 150 cycles per bit. Plenty. A program that needs 10 instructions per bit is fine up to 15 MHz.

For signals **you receive** (where someone else owns the clock), run the SM much faster than the signal: **oversample**. Then a `WAIT` catches an edge within a few cycles of it happening. EmuWire's I2C slave runs at 64 cycles per SCL period.

### The instruction budget

32 instructions per block, shared by all four SMs. Count on every change. `pioasm` prints the length, and refuses anything over 32.

Ways to save instructions, roughly from easiest to cleverest:

1. **Side-set instead of `SET`** for pins that change alongside other work.
2. **Delays instead of `nop`s.**
3. **`.wrap` instead of a closing `jmp`.**
4. **One loop, several entry points.** Enter the same bit loop with different counter values instead of writing it twice.
5. **One FIFO word, several fields.** A flag in bit 31, an address in the next 5 bits, data below: one `pull`, then several `out`s.
6. **`out pc` instead of chains of conditional jumps.**
7. **Move decisions to the CPU** with the stall-and-ask pattern. The CPU has unlimited code space.
8. **Split across state machines.** Each SM has its own registers, its own JMP pin, its own config. Two SMs running small programs can sometimes do what one couldn't. They still share the block's 32 instructions, so this saves instructions only if the pieces get simpler.
9. **Use another block.** The RP2350 has three, each with its own 32 instructions.

---

## 13. Case study: EmuWire's I2C slave

[`firmware/pio/i2c_slave.pio`](../../firmware/pio/i2c_slave.pio) is a real program that uses almost everything above. It's an I2C **slave**: it answers a master, and any number of addresses, with decisions made by the CPU. It fills its block exactly: **32 of 32**.

### The requirements that shape it

- I2C is **open drain**: never drive high (lesson 7).
- The master owns the clock: the program must follow it, so it **oversamples** (lesson 12).
- After each byte, the slave must answer ACK or NACK **within one clock period** — under 1 µs at fast speeds. Too short for an address lookup on the CPU.
- A master can reframe at any byte boundary with a repeated START, or end with a STOP.

### One mechanism per pin

```
SET / OUT  →  SDA only (count 1)   : the ACK and the data bits
side-set   →  SCL only, pindirs    : "side 1" holds the clock low, "side 0" releases it
IN         →  SDA, masked to 1 pin : so "mov x, pins" reads SDA and nothing else
JMP pin    →  SCL                  : "jmp pin" and "wait jmppin" both read the clock
```

Because SDA and SCL are written by different mechanisms, no single instruction can change both. An ACK can never release the clock by accident.

### The core trick: stretch, ask, answer

The ACK deadline is too short for the CPU. But I2C lets a slave **hold SCL low** — "clock stretching" — and the master must wait. So after every byte:

```
handshake:
    wait 0 jmppin                    ; the 8th clock pulse has ended
    irq nowait 0 rel         side 1  ; HOLD SCL LOW, and wake the CPU
    pull block                       ; stall — the bus is frozen — until it answers
    out pindirs, 1 [7]               ; the answer's ACK bit onto SDA, let it settle
    wait 1 jmppin            side 0  ; RELEASE SCL, wait for the 9th clock to rise
    wait 0 jmppin                    ; ... and fall
    set pindirs, 0                   ; release SDA
    out pc, 5                        ; continue wherever the CPU said
```

Look at what each line uses:

- **side-set at the start of a stalling instruction** — `wait 1 jmppin side 0` releases the clock *and* waits for it in one instruction (lesson 6).
- **a stall holds pins** — while `pull block` waits, SCL stays low and the whole bus waits with it (lesson 3).
- **one word, three fields** — the CPU's answer holds the ACK bit, a continuation address, and (for reads) the byte to send (lessons 8, 12).
- **`out pc`** — the CPU chooses: go idle, receive another byte, or transmit (lesson 9).
- **`rel`** — the same program on four SMs would raise four different flags (lesson 9).

The CPU now has as long as it likes, and the hard real-time deadline has become a soft one.

### Watching for a repeated START

A repeated START is "SDA falls while SCL is high". It can only happen at a byte boundary, so only the first bit of each received byte needs watching:

```
public rx_path:
    wait 1 jmppin            ; the clock rises
    mov y, pins              ; SDA right now
rx_watch:
    mov x, pins              ; SDA again
    jmp x!=y, got_start      ; changed while the clock is high: START or STOP
    jmp pin rx_watch         ; clock still high: keep watching
    in y, 1                  ; clock fell, SDA held still: a real data bit
```

This uses `mov x, pins` with the RP2350's IN pin count set to 1, so X and Y hold SDA alone, and `jmp x!=y` — the only comparison PIO has.

### Telling data from addresses

After a byte, the CPU must know if it was an address or data. The receive path sets **IRQ flag 4 + sm** before the byte completes; the address phase clears it. Flags 4–7 can't interrupt the CPU, so the flag acts as a label that the handler reads when flag 0 wakes it (lesson 9).

### Read the program itself

The file has comments on every block, explaining both the mechanism and why. The CPU side is [`firmware/src/bus/i2c_bus.c`](../../firmware/src/bus/i2c_bus.c).

---

## 14. Pitfalls

Each one: what you see, why, and the fix. The first two happened to this project on real hardware.

**1. The SM waits forever on a pin that is clearly toggling.**
On RP2350, the IN pin count masks `WAIT PIN`. With the count at 1, `wait 1 pin 1` reads pin 1 as 0 forever.
*Fix:* wait on that pin with `wait jmppin` or `wait gpio`, or raise the count. Remember `mov x, pins` then sees the extra pin too.

**2. Every received byte reads as 0x00.**
IN shifting left puts the byte in bits 7..0. Reading `word >> 24` gives the empty top.
*Fix:* `(uint8_t)word` for IN-left; `word >> 24` only for IN-right.

**3. A loop reads the same bit eight times.**
`wait 1 pin n` returns immediately if the pin is already high.
*Fix:* wait for 0 first, then 1, to catch a real rising edge.

**4. The program runs but the pin does nothing.**
The pin's function select wasn't given to PIO, or its direction is input.
*Fix:* `pio_gpio_init()`, and set pindirs.

**5. The first bits of each byte are garbage.**
Autopull counts every bit shifted out. If you took some bits for something else first, the refill comes late.
*Fix:* explicit `pull` per word when a word has several fields.

**6. Data from the CPU goes missing.**
`pio_sm_put()` on a full FIFO drops the word silently.
*Fix:* `pio_sm_put_blocking()`, or check `pio_sm_is_tx_fifo_full()` first.

**7. `pioasm` rejects a delay that looks small.**
Side-set takes bits from the delay field. With `.side_set 2 opt`, the maximum delay is 3.
*Fix:* fewer side-set pins, drop `opt`, or split the delay over two instructions.

**8. A pin changes earlier than expected.**
Side-set applies at the **start** of an instruction, even one that then stalls.
*Fix:* put the side-set on the instruction after the wait.

**9. The SM starts in the middle of the program.**
`pio_sm_init` starts at whatever PC you pass. If the program begins with a loop body, starting at `offset` begins mid-transaction.
*Fix:* pass `offset + prog_offset_<entry>`.

**10. A bus is held down the moment the SM starts.**
A SM that holds a line and waits for the CPU, started before the CPU is ready to answer, holds it for good.
*Fix:* install the interrupt handler first, *then* enable the SM.

**11. Two SMs can't both fit.**
They share 32 instructions.
*Fix:* lesson 12's list, or another block.

**12. `set pins, 32` is an error.**
`SET` has 5 bits of data: 0 to 31, and at most 5 pins.
*Fix:* `out` or `mov` for anything bigger.

**13. Only one pin can be tested with `jmp pin`.**
Each SM has one JMP pin.
*Fix:* compare with `mov`/`jmp x!=y`, or use a second SM with a different JMP pin.

**14. Edges wobble by a few nanoseconds.**
A fractional clock divider alternates between two cycle lengths.
*Fix:* whole-number dividers where edge timing matters.

**15. An interrupt keeps firing.**
The handler didn't clear the IRQ flag.
*Fix:* `pio_interrupt_clear()` in the handler.

**16. An instruction isn't recognised.**
RP2350-only instructions need `.pio_version 1` at the top of the program.

---

## 15. Debugging PIO

PIO has no debugger and no `printf`. But it's simple enough that a few tools go a long way.

**Where is it?** `pio_sm_get_pc()` tells you which instruction the SM is on. Compare it with the instruction list in the generated header. A SM stuck on a `wait` tells you exactly which condition never came true. This finds most bugs.

**Did a FIFO overflow?** The block's `FDEBUG` register has sticky flags per SM: TX stalled, TX overflow (CPU wrote into a full FIFO), RX stalled, RX underflow (CPU read an empty FIFO). Read it from C, write ones to clear.

**Use a spare pin as a probe.** Add a side-set pin and toggle it at the point you care about. On a logic analyzer you then see exactly when the program got there. Side-set costs no instruction, so the probe doesn't change the timing.

**Logic analyzer.** For anything that talks to the outside, capture the pins. PulseView and `sigrok-cli` decode I2C, SPI, UART and more. Sample at least 4–10× faster than the signal; more if you're measuring small timing differences. `sigrok-cli -P i2c:scl=D0:sda=D1 --protocol-decoder-samplenum` prints every bit with its sample number, so timings can be measured exactly rather than by eye.

**Step by hand.** Disable the SM, then use `pio_sm_exec()` to run one instruction at a time and inspect the result.

**Change one thing at a time, and test on hardware before stacking.** A PIO program that's never run on silicon hides its bugs well. Both pitfalls 1 and 2 survived careful review and only showed up on the bench.

---

## 16. Cheat sheet

### Instructions

| Instruction | Syntax | Notes |
|---|---|---|
| `JMP` | `jmp [cond,] label` | cond: `!x` `x--` `!y` `y--` `x!=y` `pin` `!osre` |
| `WAIT` | `wait 0\|1 gpio n` · `pin n` · `irq n [rel]` · `jmppin [+n]` | returns at once if already true; `jmppin` is RP2350 |
| `IN` | `in src, n` | src: `pins` `x` `y` `null` `isr` `osr`; n 1–32 |
| `OUT` | `out dst, n` | dst: `pins` `pindirs` `x` `y` `null` `isr` `pc` `exec` |
| `PUSH` | `push [iffull] [block\|noblock]` | ISR → RX FIFO, clears ISR |
| `PULL` | `pull [ifempty] [block\|noblock]` | TX FIFO → OSR; noblock+empty copies X |
| `MOV` | `mov dst, [!\|~\|::]src` | `nop` = `mov y, y`; `pindirs` dst is RP2350 |
| `IRQ` | `irq [next\|prev] set\|nowait\|wait\|clear n [rel]` | flags 0–3 reach the CPU; next/prev RP2350 |
| `SET` | `set dst, 0..31` | dst: `pins` `pindirs` `x` `y`; ≤ 5 pins |

Every instruction: 1 cycle, plus `[delay]`, plus any stall.

### Directives

| Directive | Meaning |
|---|---|
| `.program name` | start a program |
| `.pio_version 1` | allow RP2350 instructions |
| `.side_set n [opt] [pindirs]` | side-set width, optional, and pins or directions |
| `.wrap_target` / `.wrap` | the free loop |
| `.define [public] NAME value` | constant, exported if public |
| `public label:` | export the label's offset |
| `.origin n` | load at a fixed address |
| `.in n left\|right [auto] [threshold]` | IN config (RP2350 for count < 32) |
| `.out n left\|right [auto] [threshold]` | OUT config |
| `.set n` | SET pin count |
| `.fifo txrx\|tx\|rx\|txput\|txget\|putget` | FIFO mode |
| `% c-sdk { ... %}` | C code copied into the header |

### Delay limits

| Side-set | Max delay |
|---|---|
| none | 31 |
| `.side_set 1` | 15 |
| `.side_set 1 opt` | 7 |
| `.side_set 2 opt` | 3 |

### Formulas

```
SM rate        = clk_sys / divider                (150 MHz on RP2350 by default)
cycles per bit = SM rate / bit rate
UART divider   = clk_sys / (cycles_per_bit × baud)
loop count     = set x, N-1   →  jmp x-- runs the body N times
```

### Shift directions

| | shift left | shift right |
|---|---|---|
| **IN** | enters at bit 0 → byte in **7..0** | enters at bit 31 → byte in **31..24** |
| **OUT** | leaves from bit 31 (MSB first) | leaves from bit 0 (LSB first) |

---

## 17. Further reading

- **[RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf), the PIO chapter.** The authoritative reference: every instruction's exact encoding and every register.
- **[Raspberry Pi Pico C/C++ SDK](https://datasheets.raspberrypi.com/pico/raspberry-pi-pico-c-sdk.pdf)** — the `hardware_pio` API and `pioasm`.
- **[pico-examples, `pio/` folder](https://github.com/raspberrypi/pico-examples/tree/master/pio)** — working programs for UART, SPI, I2C master, WS2812 and more. Read them alongside this course.
- **In this repo:** [RP2350 / Pico 2 architecture](../rp2350.md) for where PIO sits in the chip, [the toolchain](../toolchain.md) for `pioasm` and the build, and [`firmware/pio/i2c_slave.pio`](../../firmware/pio/i2c_slave.pio) for a full program with every decision explained.
