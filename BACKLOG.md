# Backlog

Ideas that arrive mid-phase, parked here rather than pulled into the task in hand.

## Format

```
- **<idea>** — <one line of why>
  Raised while working task <N>, <date>
```

## Deferred to v2

- Analog output — needs an external DAC **and** a digital potentiometer, since a DAC cannot emulate a resistive sensor sitting inside the DUT's own divider.
- Custom PCB on RP2350B (48 GPIO), with level shifting for 5V DUTs.

## Open

- **`address_collision` fault** — deliberately emulate two devices wired to the
  same address. Both ACK, and a read returns the bitwise AND of what each
  would have sent, with no error reported anywhere. It is a real wiring
  mistake and a genuinely hard one to diagnose, especially with two identical
  parts whose factory calibration differs: the driver reads AND-ed
  calibration and computes plausible but permanently wrong values.

  Cheap to implement — the emulation layer already chooses the byte to clock
  out, so it would compute `a & b`. No PIO change. Reproducing it with real
  hardware needs two physical parts.

  `DEV_ATTACH` deliberately rejects the accidental case; this would be the
  opt-in one.

- **Register pointer behaviour at STOP, per part** — most parts keep the
  pointer across a STOP, so a read without a pointer write continues where
  it left off. Some reset it to a fixed register. It belongs in each part's
  manifest, e.g. `pointer_on_stop: keep` or `reset_to: 0x00`, so the
  emulator matches the real silicon either way. Today it always keeps it.

  Depends on the PIO program reporting STOP and repeated START as different
  events. Both currently reach the CPU the same way, as "a new address
  byte", and the program is at 32 of 32 instructions. Decide together with
  burst reads, which need the same restructure.

  Raised while working task 14, 2026-10-05.
