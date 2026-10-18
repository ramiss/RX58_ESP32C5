## Hardware

### What you need

- A **Seeed Studio XIAO ESP32-C5**, normally **with its antenna** plugged
  into the U.FL socket (the small FPC antenna the board ships with). The
  antenna gives RX5808 sensitivity; without it the C5 is 30-40 dB less
  sensitive, which a lap timer can use on purpose (see
  [Lap timers: antenna or no antenna](Lap-Timers#lap-timers-antenna-or-no-antenna)).
- For the analog RSSI pin: **3 resistors and 2 capacitors** (below).
- Recommended: a **330 Ω resistor in series with DATA**.

### Pinout

| RX5808 pin | Function | XIAO ESP32-C5 pin | Notes |
|---|---|---|---|
| CH1 | SPI DATA | **D10** (GPIO10) | Bidirectional (the C5 drives it only during read frames) |
| CH2 | SPI SELECT | **D8** (GPIO8) | Input, active low |
| CH3 | SPI CLOCK | **D1** (GPIO0) | Input |
| RSSI | Analog RSSI | **D0** (GPIO1) | PWM: needs the RC filter below |
| +5V | Supply | **5V** | Or power from USB-C |
| GND | Ground | **GND** | Common with the host |
| VIDEO | Composite video | (D3-D7, D9: future video ladder) | Not in this firmware |
| A6.5M | Audio | - | Not provided |

**Leave unconnected:** D2 and the back pads GPIO2-5 and GPIO26-28 are
reserved for the video mode's internal I/Q loopback lanes (anything attached
there will corrupt video later). D3-D7 and D9 are reserved for the video
ladder.

### RSSI filter

The C5 has no DAC, so RSSI is a 78 kHz PWM on D0, filtered in two stages.
Stage 1 also divides 3.3 V down to a 1.32 V full scale: room for the RX5808
curve (up to ~1.08 V) plus the C5's headroom above it.

```
 XIAO C5                                                  Host
  D0 ──[ R1 1.5 kΩ ]──●──[ R3 1 kΩ ]──●──────────────────► RSSI input
                      │ A             │ B
                 ┌────┴────┐          │
               R2 1 kΩ   C1 220 nF   C2 100 nF
                 │         │          │
  GND ───────────┴─────────┴──────────┴──────────────────► GND
```

| Part | Value | Between |
|---|---|---|
| R1 | 1.5 kΩ (brown-green-red; or two 3 kΩ in parallel) | D0 and node A |
| R2 | 1 kΩ | node A and GND |
| C1 | 220 nF (0.22 µF, "224"), ceramic/film | node A and GND |
| R3 | 1 kΩ | node A and node B |
| C2 | 100 nF (0.1 µF, "104"), ceramic/film | node B and GND |

Measured: output within 10 mV of the design from 0 to 1.31 V, settled
within 50 ms, source impedance ~1.6 kΩ (a host's 100 kΩ pulldown, as on
FPVRaceOne, lowers it ~1.6%). Check the values: µF capacitors give the
right voltage but respond ~1000x too slowly, and a 1.5 Ω resistor in place
of 1.5 kΩ removes the divider.

### RSSI curve

The RSSI pin follows a real RX5808: measured against two RX5808 modules
side by side with the C5 (on its antenna), one VTX at 25 mW moved from far
away to 5 cm.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/ramiss/RX58_ESP32C5/main/docs/images/rssi_curve_dark.svg">
  <img alt="RSSI pin voltage against signal level: the C5 follows two real RX5808 modules from about 0.4 V at the noise floor, at about 15 mV per dB, to about 1.08 V where both RX5808s saturate, then keeps rising about 4 dB more to 1.19 V." src="https://raw.githubusercontent.com/ramiss/RX58_ESP32C5/main/docs/images/rssi_curve_light.svg">
</picture>

| Signal (dB above the noise floor) | C5 RSSI pin | RX5808 module A | RX5808 module B |
|---|---|---|---|
| 0 (no signal) | 0.40 V | 0.40 V | 0.35 V |
| 10 | 0.55 V | 0.56 V | 0.39 V |
| 20 | 0.70 V | 0.77 V | 0.60 V |
| 30 | 0.85 V | 0.84 V | 0.66 V |
| 40 | 1.01 V | 1.01 V | 0.82 V |
| 48 | 1.11 V | 1.08 V (saturated) | 1.08 V (saturated) |
| 52 (C5 clips) | 1.19 V | 1.08 V | 1.09 V |

- **Linear in dB** like the RX5808's log detector: ~0.37-0.40 V with no
  signal, ~15 mV/dB. The two real modules differ by up to ~150 mV from each
  other (module spread); the C5 sits within that spread.
- **Headroom:** both RX5808s saturate at ~1.08 V; the C5 keeps resolving
  ~4 dB more. Near the gate a host sees a peak instead of a flat plateau,
  which matters to lap timers that stamp the first sample of the maximum.
- **Gain-independent:** the curve is referred to the receiver input, so a
  host's EXT GAIN does not shift it. Hosts calibrate thresholds per receiver
  as they do with an RX5808.

### Example: FPVRaceOne on a XIAO ESP32-C6

| Signal | XIAO C6 (FPVRaceOne) | XIAO C5 |
|---|---|---|
| DATA | D10 | D10 |
| SELECT | D1 | D8 |
| CLOCK | D8 | D1 |
| RSSI | D2 / A2 | D0, through the RC filter |
| GND | GND | GND |

---
