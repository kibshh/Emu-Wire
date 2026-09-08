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
