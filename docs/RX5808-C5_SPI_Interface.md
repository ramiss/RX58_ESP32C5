# RX5808-C5 SPI Interface

**Datasheet for hosts of the ESP32-C5 RX5808 replacement receiver**

Version 0.9 (draft), 2026-10-16. This document is licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/): anyone may
implement this interface in host software under any license, including MIT.
The receiver firmware itself is GPL-3.0-only and is a separate work.

> **Status.** This is a specification written ahead of the firmware. Values
> marked **[TBV]** are targets still to be verified on hardware; they may
> change in version 1.0. Everything else follows the Richwave RTC6715 /
> RX5808 behaviour that existing hosts rely on.

---

## Contents

1. [Overview](#1-overview)
2. [Pins and electrical](#2-pins-and-electrical)
3. [Bus protocol](#3-bus-protocol)
4. [RX5808 registers (legacy)](#4-rx5808-registers-legacy)
5. [Register read-back](#5-register-read-back)
6. [Compatibility summary](#6-compatibility-summary)
7. [Extension: the EXT register](#7-extension-the-ext-register)
8. [Extension: fast readout](#8-extension-fast-readout)
9. [Readout blocks](#9-readout-blocks)
10. [RSSI output and units](#10-rssi-output-and-units)
11. [Timing summary](#11-timing-summary)
12. [Host integration guide](#12-host-integration-guide)
13. [Revision history](#13-revision-history)

---

## 1. Overview

The RX5808-C5 is a Seeed XIAO ESP32-C5 running receiver firmware that makes
it behave like an RX5808 5.8 GHz FPV receiver module. A host that already
drives an RX5808 over its 3-wire bus and reads its analog RSSI pin works
without changes.

The receiver has two modes:

| Mode | What it does | RSSI pin | Video pin |
|---|---|---|---|
| **RX5808 mode** (default) | Receives one channel continuously, like an RX5808 | That channel | Composite video (CVBS) |
| **Multi-channel mode** (extension) | Hops between up to 8 channels, about 1000 readings per second on each | One selected channel | Blanked |

Multi-channel mode is time-multiplexed, not simultaneous: the receiver sees
one channel at a time and visits each in turn, roughly every millisecond.
The host reads every channel's **peak RSSI and the time of that peak** over
the same 3 wires (sections 8-9), so one host can time up to 8 pilots.

Hosts that never use the extension see a plain RX5808. Every extension frame
uses a register address the RTC6715 marks as reserved, so legacy hosts never
send one by accident.

---

## 2. Pins and electrical

### 2.1 Pins

| RX5808 pin | Function | XIAO ESP32-C5 pad (GPIO) | Direction |
|---|---|---|---|
| CH1 | SPI DATA | D10 (GPIO10) | Bidirectional |
| CH2 | SPI SELECT (latch enable, active low) | D8 (GPIO8) | Input |
| CH3 | SPI CLOCK | D1 (GPIO0) | Input |
| RSSI | Analog RSSI | D0 (GPIO1), PWM through an RC filter | Output |
| VIDEO | Composite video | 6-resistor ladder on D3-D9 | Output |
| A6.5M | Audio | Not provided | - |
| +5V | Supply | XIAO 5V pin | - |
| GND | Ground | XIAO GND | - |

The RX5808 module needs the "SPI_EN mod" to use its bus; the C5 is always in
SPI mode.

### 2.2 Electrical

| Parameter | Value |
|---|---|
| Supply | 5 V on the XIAO 5V pin (USB-C may power it instead) |
| Supply current | [TBV] (Wi-Fi receive-only; expected below 150 mA) |
| Logic levels | **3.3 V.** The C5's pins are **not 5 V tolerant.** Hosts with 5 V logic (Arduino Uno/Nano/Mega nodes) need a level shifter on all three bus lines. 3.3 V hosts (ESP32, STM32, RP2040) connect directly. |
| DATA | Driven by the host for writes; driven by the C5 only during read frames while its SELECT is low. A **330 Ω series resistor** in the DATA line is recommended to limit current if host and C5 ever drive at once. |
| SELECT, CLOCK | Inputs. Keep them defined at power-up (host pin driven, or 10 kΩ pull-up on SELECT). |
| RSSI output | 0 to about 1.03 V, source impedance about 1 kΩ (section 10) |
| Video | About 1 Vpp into 75 Ω (from the C5VRX resistor ladder) [TBV] |

### 2.3 Shared bus

Several receivers may share DATA and CLOCK, each with its own SELECT (as
RotorHazard's 8-node boards do). A receiver ignores CLOCK and DATA while its
SELECT is high and never drives DATA unless its own SELECT is low.

---

## 3. Bus protocol

### 3.1 Frame

A frame is SELECT going low, at least 25 CLOCK pulses, and SELECT going high.
Bits are sent **LSB first** and sampled on the **rising** edge of CLOCK.

| Bits on the wire | Field |
|---|---|
| 1-4 | Register address A0, A1, A2, A3 |
| 5 | R/W: **1 = write**, 0 = read |
| 6-25 | Data D0 ... D19 |

```
SELECT ‾‾‾‾\_______________________________________________/‾‾‾‾
CLOCK  _____/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_ ... _/‾\_/‾\_______________
DATA   ======A0==A1==A2==A3==RW==D0== ... =D18=D19=============
             ^ sampled on each rising edge          ^ latched on SELECT rising
```

- **Writes take effect when SELECT rises.**
- **Extra clocks** after the 25th are ignored, so hosts that send 32-clock
  (4-byte) frames work.
- **Frames of fewer than 25 clocks** are discarded.
- **CLOCK idles low** (SPI mode 0). DATA may change while CLOCK is low.

### 3.2 Bus speed

| | Minimum | Maximum |
|---|---|---|
| Write frames | any (the C5 has no timeout) | 4 MHz [TBV] |
| Legacy read frames (section 5): address and R/W bits | 40 µs per bit | - |
| Legacy read frames: data bits | CLOCK low 10 µs, high 5 µs | - |
| Fast readout frames (section 8) | - | 4 MHz [TBV] |
| Time between frames (SELECT high) | 10 µs | - |

Existing hosts range from about 1.1 kbit/s (FPVRaceOne, RotorHazard) to
4 MHz hardware SPI; all are within range for writes.

---

## 4. RX5808 registers (legacy)

All registers are 20 bits. Writes to them are stored and read back as written
(section 5), whether or not the C5 acts on them.

| Addr | RTC6715 name | Default | C5 behaviour |
|---|---|---|---|
| 0x00 | Synthesizer A (R counter) | 0x00008 | **Used.** R in the frequency formula; 8 for an 8 MHz reference. |
| 0x01 | Synthesizer B (N, A) | 0x02A05 (5865 MHz) | **Used.** Tunes the receiver (4.1). |
| 0x02 | Synthesizer C | 0xDB643 | Stored only |
| 0x03 | Synthesizer D | 0x03980 | Stored only |
| 0x04 | VCO switch-cap control | 0x1E1CE | Stored only |
| 0x05 | DFC control | 0x7E1D2 | Stored only |
| 0x06 | 6 MHz audio demodulator | 0x82408 | Stored only |
| 0x07 | 6.5 MHz audio demodulator | 0x82408 | Stored only |
| 0x08 | Receiver control 1 | 0x03F40 | Stored only |
| 0x09 | Receiver control 2 | 0xB2018 | Stored only |
| 0x0A | Power-down control | 0x10C13 | **Used** (4.2) |
| 0x0B-0x0D | Reserved | 0 | Stored only |
| 0x0E | Reserved in the RTC6715 | - | **EXT register** (section 7); read = identify |
| 0x0F | State | 0 | **Write = reset/wake** (4.3) |

Defaults are the RTC6715 datasheet's binary rows.

### 4.1 Tuning (0x01)

```
tf = (f_MHz - 479) / 2          (integer division)
N  = tf / 32,  A = tf % 32
register 0x01 = (N << 7) | A
```

The C5 decodes the register back to a frequency and tunes to it:

```
f_MHz = 2 * (N * 32 + A) * (8 / R) + 479      (R from register 0x00, default 8)
```

As on a real RX5808 the step is 2 MHz: an even frequency lands 1 MHz low
(5658 tunes 5657). That is well inside the channel bandwidth.

| Range | Behaviour |
|---|---|
| 5645-5945 MHz (all FPV bands except L) | Tuned and verified |
| 5180-5644 MHz | Tuned; outside FPV use [TBV] |
| Outside 5180-5945 MHz | Stored; the receiver stays on its previous frequency |

**Worked example, 5658 MHz (RaceBand R1):** tf = 2589, N = 80, A = 29,
register = 0x0281D. On the wire, in order:

```
A0..A3 = 1 0 0 0      (address 0x1)
R/W    = 1            (write)
D0..D19= 1 0 1 1 1 0 0 0 0 0 0 1 0 1 0 0 0 0 0 0
```

RSSI is valid within **35 ms** of the frame ending (SELECT rising) [TBV];
hosts already wait at least that long.

### 4.2 Power-down (0x0A)

| Value written | Effect |
|---|---|
| 0xFFFFF (all ones) | Power down: RSSI pin at about 0 V, video blanked |
| Anything else (0x10C13 default, 0xD0DF3, 0xD55F3, 0xF2FFF, ...) | Receiver on, video on |

Only the all-ones value powers down. The per-bit meanings are not
documented reliably and hosts disagree: Chorus RF Laptimer reads RSSI after
writing 0xF2FFF, which sets mixer and IF power-down bits, so decoding
individual bits would break working hosts. The video power-down bits
(18 PD_VAMP, 19 PD_VCLAMP) are ignored by default, because RSSI-only hosts
set them and goggle hosts must not lose video; an option may honour them
later.

### 4.3 Reset (0x0F)

Writing 0x0F (any data, hosts use 0) resets every register to its default,
leaves power-down, and keeps the current frequency until 0x01 is written.
Legacy hosts write 0x0F to wake from power-down.

---

## 5. Register read-back

A real RX5808 does not answer reads. The RX5808-C5 does, using the
RTC6715 read protocol, so hosts can verify tuning (RotorHazard's
`TEST_RX_REGISTER`) and detect the C5 (section 7.3).

### 5.1 Read frame

1. SELECT low.
2. Send the address A0-A3 and R/W = **0** on rising edges, as for a write.
   **Each of these 5 bits must take at least 40 µs** (25 kbit/s); the C5 uses
   this to tell a read from a fast write.
3. After the R/W bit, the host releases DATA (input with pull-up) while
   CLOCK is low.
4. The C5 drives D0 **only once the host has released DATA**, and D0 is
   valid within 5 µs of that release. (If D0 is 0 the C5 may drive it
   earlier: the host is already holding the line low after sending R/W = 0,
   so the two never disagree.) Each following bit is valid within 5 µs of
   each following falling edge of CLOCK.
5. The host samples DATA while CLOCK is low, at least 10 µs after the
   falling edge, then raises CLOCK and holds it high at least 5 µs. It
   reads 20 bits (fewer is allowed).
6. SELECT high. The C5 releases DATA within 5 µs.
7. The host resumes driving DATA no sooner than 10 µs after SELECT rises.

RotorHazard's timing meets this: 900 µs per address bit, release of DATA
about 300 µs after the R/W bit, then 10 µs per clock phase.

```
SELECT ‾‾\____________________________________________________/‾‾‾
CLOCK  ___/‾\_/‾\_/‾\_/‾\_/‾\______/‾‾\____/‾‾\__ ... __/‾‾\_______
DATA   ==A0==A1==A2==A3==R/W=0==>|-D0------|-D1----- ... -D19-|  (released)
       host drives (>= 40 us/bit)   C5 drives, changing after each falling edge
```

### 5.2 What a read returns

| Register | Returns |
|---|---|
| 0x00-0x0D | The stored value (as last written, or the default) |
| 0x0E | Identify word (section 7.3) |
| 0x0F | 0x00000 |

**Example:** after tuning 5658 MHz, a read of 0x01 returns 0x0281D.

---

## 6. Compatibility summary

| RX5808 / RTC6715 function | RX5808-C5 | Notes / alternative |
|---|---|---|
| Tuning via 0x01 | Yes | 5645-5945 MHz verified; 2 MHz step like the RX5808 |
| R counter 0x00 | Yes | |
| Power-down 0x0A = 0xFFFFF | Yes | |
| Reset/wake via 0x0F | Yes | |
| Analog RSSI pin | Yes | PWM + RC; curve matched to a real module [TBV] |
| RSSI valid after tune | Yes, within 35 ms [TBV] | |
| Register read-back | **Yes (better than the original)** | Real modules never answer |
| Composite video | Yes (RX5808 mode) | 6-bit; image quality below an RX5808 at long range [TBV] |
| Video off via PD_VAMP / PD_VCLAMP | Ignored by default | RSSI-only hosts set these bits; honouring them would blank goggle hosts' video |
| Audio output (A6.5M) | **No** | No pin or audio decoder. Possible on a future custom board. |
| Synthesizer/VCO/DFC/audio/receiver tuning registers 0x02-0x09 | Stored only | Different radio; these have no meaning on the C5 |
| RSSI squelch, IF/video gain registers | No | Use the EXT GAIN register (7.2) instead |
| Parallel channel select (no SPI_EN mod) | Optional, off by default | Selects band E from the CH1-3 levels. Enabled in the receiver's settings; it can't be auto-detected because an idle SPI bus looks like a valid channel code. |
| 5 V logic | **No** | Level shifter needed; see 2.2 |
| Multiple channels at once | **Extension** | Multi-channel mode (sections 7-9) |

---

## 7. Extension: the EXT register

### 7.1 EXT write (address 0x0E)

An EXT write is a normal write frame to address 0x0E. Its 20 data bits hold
a sub-register number and a 16-bit value:

| Data bits | Field |
|---|---|
| D19-D16 | Sub-register (0-15) |
| D15-D0 | Value |

### 7.2 Sub-registers

| Sub | Name | Value | Meaning |
|---|---|---|---|
| 0x0 | MODE | 0 or 1 | 0 = RX5808 mode, 1 = multi-channel mode. The receiver restarts its radio in the new mode, keeping every register and slot setting; this takes up to 2 s [TBV]. During the switch the RSSI pin holds its floor level and INFO reports the switch (9.1). |
| 0x1-0x8 | SLOT1_MHZ ... SLOT8_MHZ | 0 or 5180-5945 | Frequency of slot *n* in MHz (exact, no 2 MHz step); 0 = slot off. Takes effect at the next hop cycle. |
| 0x9 | RSSI_PIN_SLOT | 1-8 | The slot whose RSSI drives the analog pin in multi-channel mode. Default 1. |
| 0xA | GAIN | 0, or 1-83 | 0 = firmware default (60). 1-83 forces that receiver gain index. Lower values avoid overload with a quad very close to the receiver. |
| 0xB | READ_SELECT | 0-3 | Latch block *n* for the next fast readout frame (section 8). |
| 0xC | OPTIONS | bit field | Bit 0: smoothing (median of 3, 1 = on, default on). Others reserved, write 0. |
| 0xD-0xE | Reserved | 0 | |
| 0xF | NOP | any | Does nothing |

In multi-channel mode, a legacy 0x01 write sets the frequency of the
RSSI_PIN_SLOT slot, so the RSSI pin keeps following the host's tuning.

**Example: enter multi-channel mode.** Address 0xE, write, data 0x00001:

```
A0..A3 = 0 1 1 1      R/W = 1
D0..D19= 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0   0 0 0 0   (D16-D19 = sub 0)
```

**Example: slot 3 = 5732 MHz (R3).** Data = (3 << 16) | 5732 = 0x31664.

### 7.3 Identify

A read of register 0x0E (section 5) returns:

| Bits | Value |
|---|---|
| D19-D12 | 0xC5 (identify byte) |
| D11-D8 | Protocol major version (0) |
| D7-D0 | Protocol minor version (9 for this document) |

So version 0.9 returns **0xC5009**. A real RX5808 does not drive DATA; with
a pull-up the host reads 0xFFFFF, and without one an undefined value that
won't match. Hosts should only use the extension after a matching identify.

---

## 8. Extension: fast readout

Fast readout moves a block of data from the C5 to the host at any speed up
to 4 MHz, using the C5's SPI hardware. It takes two frames:

1. **Request.** An EXT write of READ_SELECT = *n* (section 7.2). When SELECT
   rises, the C5 takes a snapshot of block *n*. For the PEAKS block this also
   starts a new peak interval (9.2).
2. **Wait** at least **50 µs**.
3. **Readout frame:**
   1. The host sets DATA as input with pull-up and takes SELECT low.
   2. It waits at least **20 µs**, then sends 8 × L clocks (L = block
      length in bytes).
   3. The C5 drives DATA throughout. Bit 0 of byte 0 is valid before the
      first rising edge, and each next bit follows the next falling edge.
   4. The host samples on each rising edge.
   5. SELECT high. The C5 releases DATA within 5 µs.
4. The host may drive DATA again **10 µs** after SELECT rises.

A host may stop early (fewer clocks) or clock past the end; bits past the
end read as 1. Each request latches one snapshot: further readout frames
return the same bytes until the next request, so a host can retry a
readout whose CRC failed.

```
         request (write, READ_SELECT=1)        readout frame
SELECT ‾\________________________/‾‾‾ >=50us ‾\__ >=20us __________________/‾‾
CLOCK  _/‾\_/‾\ ... _/‾\_/‾\_________________________/‾\_/‾\_/‾\ ... /‾\____
DATA   ==host drives 25 bits====  (host input) ======b0===b1==b2 ... ==== (C5)
```

Byte order: blocks are little-endian, and each byte is sent LSB first, so
the whole block is one LSB-first bit stream. Every block ends with a
**CRC-16/CCITT-FALSE** (polynomial 0x1021, initial value 0xFFFF, no
reflection, no final XOR), sent little-endian, over all the bytes before
it. Test vector: the 9 ASCII bytes `123456789` give 0x29B1. A host must
discard a block whose CRC does not match, and may read it again (see above).

---

## 9. Readout blocks

### 9.1 Block 0: INFO (16 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | 0xC5 (identify) |
| 1 | 1 | Protocol major version |
| 2 | 1 | Protocol minor version |
| 3 | 1 | Capabilities: bit 0 video, bit 1 multi-channel, bit 2 fast readout, bit 3 legacy read-back |
| 4 | 2 | Firmware version (major × 256 + minor) |
| 6 | 1 | Current mode (0 RX5808, 1 multi-channel) |
| 7 | 1 | Flags: bit 0 powered down, bit 1 tuning or mode switch in progress (RSSI not valid), bit 2 receiver overload seen since the last INFO read |
| 8 | 2 | RX5808 mode: tuned frequency in MHz. Multi-channel mode: RSSI-pin slot's frequency. |
| 10 | 1 | Active slot mask (bit *n*-1 = slot *n* on) |
| 11 | 1 | Gain index in use |
| 12 | 2 | Readings per second per active slot (multi-channel), or block-power rate (RX5808 mode) |
| 14 | 2 | CRC-16 |

### 9.2 Block 1: PEAKS (8 + 8 × n + 2 bytes)

Each request starts a new **peak interval**. The block reports, for each
active slot, the highest RSSI seen in the interval just ended, how long
before the snapshot it occurred, and the latest reading.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Sequence number, +1 per PEAKS snapshot |
| 2 | 4 | `snap_us`: the C5's microsecond clock at the snapshot (for gap and continuity checks) |
| 6 | 1 | Slot mask: which slots follow, in ascending order |
| 7 | 1 | Flags: bit 0 readings were missed in this interval (mode switch or retune) |
| 8 + 8k | 2 | Slot *k*: peak RSSI (section 10.2), 0 if no reading |
| 10 + 8k | 2 | Slot *k*: `age_us`, time from the first reading equal to the peak to the snapshot; 0xFFFF = 65.5 ms or more |
| 12 + 8k | 2 | Slot *k*: latest RSSI |
| 14 + 8k | 1 | Slot *k*: number of readings in the interval (saturates at 255) |
| 15 + 8k | 1 | Slot *k*: flags: bit 0 valid, bit 1 clipped (receiver overloaded) in this interval |
| 8 + 8n | 2 | CRC-16 |

8 slots = 74 bytes = 592 clocks, about 0.6 ms at 1 MHz.

The snapshot is taken when the request frame's SELECT rises, so the peak
happened `age_us` before that moment on the host's own clock. No clock
synchronisation is needed (section 12.3).

In RX5808 mode the block has one slot (mask 0x01) carrying the tuned
channel's peak over the interval.

### 9.3 Block 2: REGS (50 bytes)

Registers 0x00-0x0F as 16 × 3 bytes, little-endian, the 20-bit value in
the low bits, followed by the CRC-16.

### 9.4 Block 3: reserved for a band-occupancy scan (future)

A readout of an unimplemented block returns 0xFF bytes with a CRC that will
not match.

---

## 10. RSSI output and units

### 10.1 Analog pin

- **Generation:** a PWM output filtered by two RC stages.
- **Range:** 0 to about 1.03 V. The default curve maps the receiver's noise
  floor to about 0.5 V and a very strong signal to about 1.1 V, rising
  monotonically and roughly linearly in dB, like an RX5808. The exact curve
  is matched to a measured RX5808 [TBV].
- **Response:** about 0.5 ms (10-90 %). Ripple is below 1 mV, so a host that
  takes the peak of fast ADC frames (FPVRaceOne) is not biased.
- **Never perfectly flat:** a live receiver always shows a little movement,
  so host "dead receiver" checks behave as with an RX5808.
- **During a tune or mode switch:** the pin holds the floor level until the
  new reading is valid.
- **In power-down:** about 0 V.
- **Source impedance:** about 1 kΩ. A 100 kΩ load (FPVRaceOne's pull-down)
  costs about 1 %.

### 10.2 Digital RSSI units

| Property | Value |
|---|---|
| Type | Unsigned 16-bit |
| Resolution | **1/16 dB** per count |
| Zero point | 0 = no reading |
| Usable span | about 50-60 dB above the noise floor [TBV] |
| Mapping to absolute power | Not calibrated in dBm |

Readings are comparable between slots of the same receiver and mode. The
firmware compensates for its own gain so a reading does not jump when the
gain changes. Hosts should calibrate enter/exit thresholds per receiver, as
they do for an RX5808.

---

## 11. Timing summary

| Event | Time |
|---|---|
| Power-on to accepting bus frames | <= 150 ms [TBV]. Frames that arrive while the radio is still starting are kept and applied once it is up. |
| Power-on to valid RSSI | <= 1.5 s [TBV] |
| Gap between frames (SELECT high) | >= 10 µs |
| Tune (0x01 write) to valid RSSI | <= 35 ms [TBV] |
| Legacy read: address/R/W bit time | >= 40 µs per bit |
| Legacy read: data bits, CLOCK low / high | >= 10 µs / >= 5 µs |
| Legacy read: C5 drives each bit after CLOCK falls | <= 5 µs |
| Fast readout: request to readout frame | >= 50 µs |
| Fast readout: SELECT low to first clock | >= 20 µs |
| C5 releases DATA after SELECT rises | <= 5 µs |
| Host drives DATA after a read frame | >= 10 µs after SELECT rises |
| MODE switch | <= 2 s [TBV] |
| Multi-channel: readings per slot (8 slots) | about 1000 per second [TBV] |
| Multi-channel: time between readings of one slot | about 1 ms, at most 2 ms [TBV] |
| Max bus clock | 4 MHz [TBV] |

---

## 12. Host integration guide

### 12.1 Detecting the C5

1. Write register 0x0F (reset) and 0x0A, as usual.
2. Read register 0x0E (section 5). **0xC5xxx** means an RX5808-C5 with
   protocol version xxx. Anything else means a real RX5808 (or nothing) -
   carry on as before.
3. Optionally read block 0 (INFO) to learn capabilities and firmware version.

### 12.2 Single-pilot timing (unchanged hosts)

Nothing changes: tune with 0x01, wait 35 ms, read the analog RSSI pin.

### 12.3 Multi-pilot timing

1. Write SLOT1-SLOTn with the pilots' frequencies, then MODE = 1.
2. Poll INFO until it reports multi-channel mode with no switch in
   progress (up to 2 s).
3. Every **5-10 ms**: request PEAKS (READ_SELECT = 1), note the host time
   `t_req` when the request frame's SELECT rises, wait 50 µs, read
   8 + 8n + 2 bytes, and check the CRC.
4. For each valid slot, the peak happened at `t_req - age_us` on the host's
   own clock. Feed `(peak, t_req - age_us)` into the lap logic as one sample
   per poll. Because the C5 holds the peak over the whole interval, a pass
   between polls is never missed.

No clock synchronisation is needed: the age spans one poll interval, so
crystal drift between the two clocks (tens of ppm) adds well under 1 µs.
Use `snap_us` and the sequence number only to detect lost polls.

**FPVRaceOne notes:**
- Its DMA ADC path already reduces samples to a peak per frame. The PEAKS
  block is the digital equivalent, delivered per slot.
- Its lap logic stamps a lap at the first sample of the maximum; `age_us`
  is defined the same way (first reading equal to the peak).
- The per-slot clipped flag marks a pass where the receiver was overloaded
  and the peak may be a flat plateau; the first reading of a plateau is
  early by about half its length.
- Its bus pins (XIAO C6: DATA GPIO18, SEL GPIO1, CLK GPIO19) need no wiring
  change. DATA must switch to input with pull-up for read frames.
- At its current 300 µs clock phases a PEAKS readout takes about 0.35 s,
  far too slow. Use short clock phases for readout frames: the C5 accepts up to
  4 MHz, and a 2 µs phase (250 kHz) reads 8 slots in about 2.4 ms.

### 12.4 RotorHazard

- RotorHazard's node firmware writes RX5808 frames and, from node API 36,
  verifies each tune by reading 0x01. The C5 answers, so the check passes.
- Nodes with 5 V logic (Arduino) need level shifting (2.2).
- RotorHazard's 8-node shared-bus board can carry up to 8 RX5808-C5
  receivers.

---

## 13. Revision history

| Version | Date | Changes |
|---|---|---|
| 0.9 | 2026-10-16 | First draft, written ahead of the firmware. |

---

*RX5808 and RTC6715 behaviour follows the Richwave RTC6715 datasheet
(Rev V0.2) and the behaviour of existing hosts (FPVRaceOne, RotorHazard,
rx5808-pro-diversity, Chorus RF Laptimer). The receiver builds on
[C5VRX](https://github.com/Twotoz/C5VRX) by Twotoz and the C5VRX
contributors.*
