# The wire protocol

How the host SDK and the board talk to each other. Read this if you're implementing a client in another language, debugging a link, or changing the protocol.

> **The `protocol/` directory is the specification.** Every message, field, type and width lives there, and `protocol/codegen.py` generates both implementations from it. This page explains the *design* — the reasoning, the flow, the byte-level examples. It deliberately does not re-list every field, because a hand-maintained copy of a generated table is a copy that goes stale.

## Where things live

[`protocol/protocol.yaml`](../protocol/protocol.yaml) is the entry point. It holds the metadata, the include list, the ID allocation ranges, and the list of invariants the loader enforces — but no message definitions of its own. Codegen loads it, merges the includes, validates, and emits.

| File | Contents |
|---|---|
| [`protocol.yaml`](../protocol/protocol.yaml) | Entry point: version, includes, ID ranges, validation rules |
| [`frame.yaml`](../protocol/frame.yaml) | Frame layout and CRC parameters |
| [`constants.yaml`](../protocol/constants.yaml) | Named sentinel values (`PIN_UNUSED`, `DEV_ID_ALL`, …) |
| [`status.yaml`](../protocol/status.yaml) | Status codes and their message templates |
| [`enums.yaml`](../protocol/enums.yaml) | Wire vocabulary, bus-independent — address modes, fault types, triggers |
| [`bitmasks.yaml`](../protocol/bitmasks.yaml) | Flag fields |
| [`structs.yaml`](../protocol/structs.yaml) | Composite payload members |
| [`buses/i2c.yaml`](../protocol/buses/i2c.yaml) | I2C: pin roles, addressing, reserved ranges, speed grades, supported faults |
| [`buses/spi.yaml`](../protocol/buses/spi.yaml) | SPI: pin roles, CPOL/CPHA modes, word widths, supported faults |
| [`messages/system.yaml`](../protocol/messages/system.yaml) | `PING`, `INFO`, `RESET` |
| [`messages/bus.yaml`](../protocol/messages/bus.yaml) | `BUS_CREATE`, `BUS_DESTROY`, `BUS_LIST` |
| [`messages/device.yaml`](../protocol/messages/device.yaml) | `DEV_ATTACH`, `DEV_DETACH`, `DEV_WRITE_REGS`, `DEV_READ_REGS` |
| [`messages/fault.yaml`](../protocol/messages/fault.yaml) | `FAULT_SET`, `FAULT_CLEAR`, `FAULT_LIST` |
| [`messages/trace.yaml`](../protocol/messages/trace.yaml) | `TRACE_START`, `TRACE_STOP` |
| [`messages/events.yaml`](../protocol/messages/events.yaml) | The four `EVT_*` async events |

Top-level keys are unioned across files. A key defined twice is an **error**, not a silent override — the loader rejects it rather than picking a winner.

### Adding a bus protocol

Each protocol lives in exactly one file under `buses/`, declaring its wire id, which signal each `BUS_CREATE` pin field carries, and which `fault_type` values are meaningful on it.

The `bus_protocol` enum is **synthesised** from those files — it isn't written by hand anywhere. That means a protocol cannot appear on the wire while its pin roles or fault support are missing, and adding 1-Wire or SENT later is one new file plus one line in `includes`.

Declaring fault support as data also makes `ERR_UNSUPPORTED_FAULT` real rather than aspirational. SPI supports 6 of the 12 fault types, and the exclusions are structural rather than unimplemented:

| Excluded on SPI | Why |
|---|---|
| `NACK_ON_READ`, `NACK_ON_WRITE`, `ACK_GLITCH` | SPI has no acknowledge bit |
| `BUS_HANG`, `CLOCK_STRETCH`, `CLOCK_SKEW` | A slave cannot hold or stretch a master's clock |

Arming one of those on an SPI bus is rejected, rather than accepted as a fault that could never fire.

## Transport

USB CDC — the board appears as a serial port with no driver installation on any modern OS. The link is point to point, so direction is always unambiguous.

USB on RP2350 is **full speed, 12 Mbit/s**. That's a real ceiling worth knowing about before planning anything high-volume.

## The frame

```
+--------+--------+--------+-----------+---------+-----------+
| MAGIC  | TYPE   | SEQ    | LEN       | PAYLOAD | CRC16     |
| u8     | u8     | u8     | u16 LE    | LEN B   | u16 LE    |
| 0xA5   |        |        |           |         |           |
+--------+--------+--------+-----------+---------+-----------+
         |<------------ CRC covers this ------------>|
```

Seven bytes of overhead. Every multi-byte field is **little-endian**, without exception.

The CRC covers `TYPE`, `SEQ`, `LEN` and `PAYLOAD` — not `MAGIC` (it's a constant, and including it would let a corrupted magic byte still checksum correctly) and not itself.

### Worked example

A `PING` request with sequence number 1 and no payload:

```
A5 01 01 00 00 44 C5
│  │  │  └──┬──┘ └─┬─┘
│  │  │     │      └── CRC16 = 0xC544
│  │  │     └───────── LEN = 0
│  │  └─────────────── SEQ = 1
│  └────────────────── TYPE = 0x01 (PING)
└───────────────────── MAGIC
```

And its response — status `OK`, uptime 123,456 ms:

```
A5 01 01 05 00 00 40 E2 01 00 D9 5E
      │        │  └────┬─────┘
      │        │       └── uptime_ms = 0x0001E240 = 123456
      │        └────────── status = 0x00 (OK)
      └─────────────────── SEQ = 1, echoed back
```

Note the response reuses `TYPE = 0x01`. There is no separate response type — see below.

## Four design decisions

### 1. Bit 7 of TYPE marks an asynchronous event

```
0x01-0x0F  system      0x10-0x1F  bus        0x20-0x2F  device
0x30-0x3F  fault       0x40-0x4F  trace      0x80-0xFF  async events
```

A dispatcher can tell a command from an event with a single bit test, with no table lookup — useful in a hot path, and self-documenting when you're staring at a hex dump. The convention is machine-checked, so it can't rot.

### 2. Responses reuse the request's TYPE and echo its SEQ

There is no separate response type per command. The host allocates `SEQ` in the range 1–255 and matches replies by it; direction is implicit because the link is point to point.

This keeps the type table at 19 entries rather than 34, and makes `TYPE` mean "what this message is about" rather than "which half of an exchange this is".

### 3. Async events always use SEQ = 0

`SEQ = 0` is reserved and never allocated by the host. That single rule is what lets a trace stream and a command response share one pipe without ambiguity: a frame with `SEQ = 0` is unsolicited, always, and needs no correlation.

Combined with bit 7, a client can route any frame correctly before parsing its payload.

An unsolicited event, `EVT_ERROR` reporting a reserved address:

```
A5 83 00 14 00 08 20 03 00 FF 00 00 00 78 00 00 00 06 12 0F 00 00 00 00 00 36 46
   │  │
   │  └── SEQ = 0 → unsolicited
   └───── TYPE = 0x83, bit 7 set → async event
```

### 4. The CRC variant is pinned exactly

"CRC16-CCITT" names several mutually incompatible algorithms. The spec states all five parameters:

| | |
|---|---|
| Name | CRC-16/IBM-3740 (also published as CRC-16/CCITT-FALSE) |
| Polynomial | `0x1021` |
| Init | `0xFFFF` |
| Reflect in / out | false / false |
| XOR out | `0x0000` |
| **Check** | **`0x29B1`** for the ASCII string `123456789` |

Why this matters — the *same polynomial* under other common variants:

| Variant | CRC of `123456789` |
|---|---|
| **CRC-16/IBM-3740** (ours) | **`0x29B1`** |
| CRC-16/XMODEM | `0x31C3` |
| CRC-16/KERMIT | `0x2189` |
| CRC-16/GENIBUS | `0xD64E` |

Two implementations that both believe they implement "CCITT" can disagree on every frame. Any implementation must reproduce `0x29B1` before it is trusted.

## The decoder contract

A receiver has to survive noise, a half-open port, a board reset mid-frame, and a host that disconnects. The rules:

1. **Scan for `MAGIC`.** Anything before it is discarded.
2. **Validate `LEN` against your own buffer size before reading the payload.** A length field is noise-controlled — never allocate or read based on it unchecked.
3. **Verify the CRC.** On mismatch, discard the frame and resume scanning *from the byte after the magic you just tried* — not from after the claimed payload, which you have no reason to trust.
4. **Never wedge.** Any byte sequence, including hostile or random input, must leave the decoder able to find the next valid frame.

That last point is tested by fuzzing a random byte stream through the decoder and asserting it still parses a valid frame afterwards.

## Message flow

A bus is a first-class object, and devices attach to it. This ordering is not incidental — it's the modelling decision the whole design rests on.

```
      host                                board
        │                                   │
        │──── PING ────────────────────────▶│   is it alive?
        │◀─── status, uptime ───────────────│
        │                                   │
        │──── INFO ────────────────────────▶│   what can it do?
        │◀─── capabilities ─────────────────│
        │                                   │
        │──── BUS_CREATE ──────────────────▶│   allocate pins + a PIO state machine
        │◀─── bus_id ───────────────────────│
        │                                   │
        │──── DEV_ATTACH (bus_id, addr) ───▶│   attach an emulated device
        │◀─── dev_id ───────────────────────│
        │──── DEV_ATTACH (bus_id, addr) ───▶│   ...and another, same bus
        │◀─── dev_id ───────────────────────│
        │                                   │
        │──── FAULT_SET (bus_id, dev_id) ──▶│   arm a fault on one device
        │◀─── fault_id ─────────────────────│
        │                                   │
        │──── TRACE_START (bus_id) ────────▶│
        │◀─── status ───────────────────────│
        │                                   │
        │           ... the DUT drives the bus ...
        │                                   │
        │◀═══ EVT_DEV_ACCESS    (SEQ=0) ════│   unsolicited, interleaved
        │◀═══ EVT_FAULT_FIRED   (SEQ=0) ════│
        │◀═══ EVT_TRACE_DATA    (SEQ=0) ════│
        │                                   │
        │──── TRACE_STOP ──────────────────▶│
        │◀─── captured, dropped, overflowed │
```

A `dev_id` is only meaningful relative to its `bus_id`; every device operation carries both.

### INFO is load-bearing

`INFO` reports live capacity — free PIO state machines, free instruction slots per PIO block, maximum devices per bus, supported protocols, buffer sizes. The values change as buses are created; they are not compile-time constants.

The SDK calls it on connect and validates every later request against it, so a user gets a specific, local error *before anything reaches the wire*:

> No free PIO state machine. 3 of 4 are in use: bus 'sensors' holds PIO0 SM0–1.

rather than a command that fails on the board and returns a number.

## The error model

There are exactly two shapes a failure may take:

| | |
|---|---|
| **Synchronous** | The handler returns a `status` in its response. Every command response begins with this field — machine-checked, no exceptions. |
| **Asynchronous** | `EVT_ERROR`, carrying the code, the message type and SEQ that caused it, the bus and device involved, a detail value, and a timestamp. |

**There is no third shape, and nothing fails silently.** This is a test instrument: a wrong answer the user trusts is worse than a crash, because they ship on the strength of it.

Every status code carries a message template in the spec, so errors reaching a human are actionable rather than numeric:

```
ERR_PIN_UNAVAILABLE
  "Pin GP{pin} cannot be used for {function} on {board}. Valid pins are {valid}."

ERR_ADDRESS_IN_USE
  "Address {address:#04x} is already taken by '{existing}' on this bus.
   Two devices cannot share an address — that is physically impossible on
   real hardware."
```

## Tracing carries raw edges

`EVT_TRACE_DATA` streams **raw pin-state edges**, not decoded transactions. The SDK reconstructs transactions from them.

Each `trace_edge` is a fixed **8 bytes**: a 32-bit timestamp in ticks, the pin state, flags, padding. Three reasons for that split:

- **Fixed-size records make the ring buffer trivially correct.** Overflow accounting over variable-length records is where trace buffers get their bugs, and a miscounted gap is the one failure this project refuses to ship.
- **DMA can write them directly**, with no CPU involvement.
- **Decoding must cope with malformed traffic** — which is exactly what people trace. That belongs on the host, where it's testable against saved captures with no hardware attached.

8 bytes is a power of two, so ring-buffer index arithmetic is a mask.

### Overflow is reported, never silent

If edges are lost, firmware inserts a record with the `OVERFLOW_GAP` flag **at the position in the stream where the loss happened**, carrying the number dropped. `TRACE_STOP` additionally reports `dropped_edges` and an `overflowed` flag.

A capture with an unreported gap is worse than no capture, because the user trusts it.

## Changing the protocol

1. Edit the relevant file under `protocol/`. Never a generated file. A new message goes in its group's file under `protocol/messages/`, taking the next free ID **within that group's reserved range** — IDs are permanent, so append rather than renumber.
2. Run `python protocol/codegen.py`.
3. Commit the regenerated output **in the same commit** as the YAML. CI regenerates and fails on any difference, so a hand-edit turns the build red rather than silently diverging.
4. Add or update round-trip tests for the changed messages.
5. If the frame format itself changed, re-run the fuzz and resync tests.
6. Consider whether `INFO` should report the new capability.
7. Bump `protocol.version` for any incompatible change — the SDK refuses to talk to firmware it doesn't understand rather than misparsing it.
