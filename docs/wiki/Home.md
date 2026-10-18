
Firmware that turns a **Seeed Studio XIAO ESP32-C5** into a drop-in
replacement for the **RX5808** 5.8 GHz FPV receiver module, plus a
**multi-channel mode** that measures up to **8 channels at once** (about
1,000 readings per second each) from one receiver.

- **Legacy hosts need no changes.** The C5 speaks the RX5808's 3-wire bus
  and drives an analog RSSI pin; an unmodified FPVRaceOne tunes it and times
  laps with it.
- **New hosts get more:** register read-back, digital RSSI in 1/16 dB,
  per-channel peak-hold with timestamps, and up to 4 MHz readout over the
  same three wires.

The host interface is specified in the
**[RX58_ESP32C5 SPI Interface datasheet](Datasheet)**
(CC BY 4.0, so hosts under any license can implement it). This README
covers the hardware, flashing, and how to integrate a host.

> **Status: pre-release (firmware 0.1, interface v0.9.8).** RX5808 mode,
> multi-channel mode and the bus extension are implemented and bench-tested
> (see [What's verified](Behaviour#whats-verified)), and the RSSI pin is matched to
> real RX5808 modules ([RSSI curve](Hardware#rssi-curve)). Composite video is
> planned (video mode) but not implemented yet.

---

## At a glance

| | |
|---|---|
| Board | Seeed Studio XIAO ESP32-C5 with its U.FL antenna (no other RF parts; lap timers may leave the antenna off) |
| Host interface | RX5808 / RTC6715 3-wire bus (DATA, SELECT, CLOCK), LSB first, 25-bit frames |
| Bus speed | Any host speed up to **4 MHz** (writes and fast readout) |
| Frequencies | 5645-5945 MHz (R1-R8 received; tuning checked up to E8 = 5945); 5180-5644 tunable, not verified |
| Tuning | RX5808 register 0x01, same formula and 2 MHz step |
| RSSI output | Analog pin on a real RX5808's curve: ~0.4 V with no signal, ~15 mV/dB, ~1.08 V where an RX5808 saturates, then ~4 dB more up to ~1.19 V ([RSSI curve](Hardware#rssi-curve)) |
| RSSI rate, RX5808 mode | ~4,900 readings/s; valid ~0.35 ms after a retune (RX5808 spec: 35 ms) |
| Multi-channel mode | Up to 8 channels, ~1,050 readings/s each, worst gap ~1.1 ms |
| Digital RSSI | 1/16 dB, peak-hold per channel with the time of the peak |
| Register read-back | Yes (RotorHazard's register check passes) |
| Boot | Bus accepts frames 51 ms after power-on |
| Supply | 5 V on the 5V pin, or USB-C |
| Logic | **3.3 V, not 5 V tolerant** (level-shift 5 V hosts) |
| Video | Planned (video mode); not in this firmware |
| Audio | No |

---

## Pages

- [Hardware](Hardware)
- [Flashing](Flashing)
- [Host integration](Host-Integration)
- [Lap timers](Lap-Timers)
- [Behaviour](Behaviour)
- [Building](Building)
- [License and credits](License-and-Credits)
- [Datasheet](Datasheet): the host interface specification
- [Development log](Development-Log): how it was built and tested
