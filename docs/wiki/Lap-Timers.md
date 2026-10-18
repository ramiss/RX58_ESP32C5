## Lap timers: antenna or no antenna

The antenna decides how far out the receiver starts to see a quad. Both
setups use the same firmware and settings (default gain 51); only the
host's thresholds differ.

| | With the antenna | Without the antenna |
|---|---|---|
| Sensitivity | Like an RX5808 | 30-40 dB lower |
| Sees a 25 mW quad from | Across a house, like an RX5808 | About 1-2 m |
| Near the gate | Rises ~4 dB past where an RX5808 flattens out | Still rising at the gate: the peak is the pass |
| Best for | Drop-in RX5808 behaviour, wide gates, scanners | Gates the quads fly within ~1-2 m of; ignoring quads elsewhere |

Bench measurement, one 25 mW VTX held at a series of distances, the
receivers side by side (RSSI-pin voltages; the antenna-less column is
computed from the measured level with the current curve):

| VTX position | RX5808 (FPVScanner, with antenna) | C5 with antenna | C5 without antenna |
|---|---|---|---|
| Another room | 0.41-0.50 V | 0.51 V | 0.37 V (no signal) |
| Half way | 0.70-0.85 V | 0.65-0.82 V | 0.37 V (no signal) |
| About 1-2 m | 1.0-1.08 V (saturating) | 1.06-1.19 V | ~0.56 V |
| About 5 cm | 1.08 V (saturated) | 1.19 V (clipped) | ~0.80-0.85 V |

**Why a lap timer might leave it off.** An RX5808 at race power
saturates metres before the gate, so its RSSI is flat across the pass;
a timer that stamps the first sample of the maximum (FPVRaceOne, for
example) then stamps early by about half the plateau. Without the antenna
the C5 is still climbing as the quad goes through, so the maximum is the
pass itself, and quads in the pits or across the field stay at the floor.

**What changes for the host:**
- **Lower thresholds.** The swing is smaller: from ~0.37 V to ~0.85 V at
  25 mW (more at higher VTX power; at 600 mW it reaches the top of the
  range within ~5 cm). Run the host's calibration (FPVRaceOne's
  calibration wizard, RotorHazard's enter/exit tuning) with the antenna
  off.
- **Gate geometry.** The quads must pass within about 1-2 m of the
  receiver at 25 mW; mount it at the gate, close to the flight line.
- Everything else is unchanged: tuning, both modes, the bus extension.

These are bench numbers from one VTX in one house; the exact distances
depend on VTX power, antenna orientation and the gate. Track tests (and
near/far with several quads) are still to come.

---
