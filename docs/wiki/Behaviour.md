## Behaviour reference

| | |
|---|---|
| Power-on | RX5808 mode, 5865 MHz (register 0x01 default), gain 51, smoothing on, all registers at RTC6715 defaults; nothing is kept across power cycles |
| Modes | 0 = RX5808 (one channel), 1 = multi-channel (up to 8 slots), 2 = video (not implemented: the request is ignored) |
| Mode switch | In place, no restart: applied in 0.03-0.8 ms |
| RSSI pin | The RX5808 curve ([RSSI curve](Hardware#rssi-curve)): ~0.4 V with no signal to ~1.19 V; 0 V in power-down. In multi-channel mode it follows the RSSI-pin slot (EXT RSSI_PIN_SLOT, default slot 1); a legacy 0x01 write retunes that slot |
| Gain | Fixed (index 51 by default; EXT GAIN overrides). The vendor AGC is off, so peaks aren't flattened. 51 keeps full sensitivity (within 1 dB of 60) and moves the clip point ~9 dB up |
| Readings | Each is band power around the channel from 256-sample 10-bit snapshots at 80 MS/s; RX5808 mode averages 4 snapshots per reading |
| Legacy register reads | Readings pause during a read's data bits: a ~5 ms gap per read at RotorHazard timing (hosts read right after a retune, so it doesn't affect lap timing). Extension block readouts don't pause readings |
| Bus robustness | Glitch filters on all three lines; frames under 25 clocks and all-ones frames are ignored; a stuck SELECT can't stall the receiver |
| USB console | 115200 baud (USB Serial/JTAG). `?` configuration, `T` statistics, `L` bus log, `B` reboot to the loader; more in `src/rx_bench.c` |

---

## What's verified

Bench tests on a XIAO ESP32-C5 driven by a XIAO ESP32-C6 test host, with a
VTX on R3:

- **Legacy bus:** 18/18 checks: FPVRaceOne's exact timing, 250 kHz, 32-clock
  frames up to 4 MHz, back-to-back frames 1.5 µs apart, RotorHazard-style
  read-back, power-down/wake, shared bus.
- **Extension:** 29/29 checks: identify, INFO/PEAKS/REGS readout with CRC at
  0.25-4 MHz, in-place mode switch, peak-hold, re-arm, gain and options, and
  legacy reads while hopping.
- **Unmodified FPVRaceOne** tunes the C5, follows its RSSI and times laps.
- **Separation:** in multi-channel mode the VTX channel reads ~30 dB above
  the other seven RaceBand channels.

The full test history is in the
[development log](Development-Log).

## Limitations

- **No video yet.** Video and full-range RSSI can't share the C5's capture
  hardware, so video will be its own mode (6-bit composite through a
  resistor ladder, after [C5VRX](https://github.com/Twotoz/C5VRX)).
- **Antenna:** needed for RX5808 sensitivity; without it the receiver only
  responds within a few metres (see [Lap timers](Lap-Timers#lap-timers-antenna-or-no-antenna)).
- **Overload:** a VTX within a few cm clips the C5's input (at 25 mW, about
  5 cm on the bench), ~4 dB later than an RX5808 saturates; near/far
  behaviour with two VTXs at race power is not yet measured.
- **RSSI curve accuracy:** matched to two RX5808 modules on one channel
  (R3); thresholds still need calibrating per receiver, as with an RX5808.
- **No Wi-Fi** on the receiver chip: the radio is used as the receiver.
- **Undocumented hardware paths:** the firmware uses undocumented ESP32-C5
  PHY and debug-capture features, so it is pinned to ESP-IDF 6.0.2.

---
