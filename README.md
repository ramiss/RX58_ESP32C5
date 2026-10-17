# ESP32_SDR_RX5808

A drop-in replacement for the RX5808 5.8 GHz receiver module, built on a
Seeed XIAO ESP32-C5 used as a software-defined radio.

Any host that drives an RX5808 (FPVRaceOne, RotorHazard nodes, rx5808-pro
diversity boards, ...) should be able to swap the module for this board with
no host changes: same 3-wire SPI tuning bus, same analog RSSI pin. Analog
video output (CVBS) is kept too, built on C5VRX's hardware video path, so
goggle and diversity hosts can use it.

**Why:** a XIAO C5 is about half the real-world price of an RX5808, and
RX5808 prices are rising. As a bonus, the C5 can do things the RX5808 can't:
read back its tuning, check that a carrier is actually present on the
channel, and measure power on digital VTXs too.

**How:** ESP32-C5 chips have an undocumented debug path that streams raw
I/Q samples from the Wi-Fi receiver
([esp-sdr](https://github.com/ESPARGOS/esp-sdr),
[C5VRX](https://github.com/Twotoz/C5VRX)). The C5 is the only ESP32 with a
5 GHz front end. The firmware fixes the gain, computes in-band power
continuously, and outputs that as RSSI.

## Status

| Phase | What | State |
|---|---|---|
| A | Bench-test stock esp-sdr: channel coverage 5645-5945 MHz, gain/overload, snapshot rate | **Done 2026-10-09** (results below; tool: [tools/phase_a](tools/phase_a/README.md)) |
| B0 | IQ loopback pad test (18-GPIO map on a bare XIAO) | **Done** (2026-10-16): all candidates PASS, zero errors in a 9-min soak; negative control FAILs as it must |
| B1 | SPI interface datasheet: legacy RX5808 emulation + multi-channel extension | **v0.9 draft done** ([docs/RX5808-C5_SPI_Interface.md](docs/RX5808-C5_SPI_Interface.md)) |
| B2 | Multi-channel hop engine: up to 8 channels at ~1 kHz each, bench stream over USB | **Done 2026-10-17**: 1061 Hz per channel on R1-R8, max gap 1.1 ms (results below; `-e hop`, [tools/phase_b](tools/phase_b/phase_b.py)) |
| B3 | RX5808-mode RSSI on C5VRX's capture ring -> PWM RSSI pin; tune-to-valid < 35 ms | Not started |
| C1 | Legacy SPI slave: RX5808 frames at any speed, register read-back | Not started |
| C2 | Extension: EXT register, mode switch, peak-hold, fast readout | Not started |
| D | Video in RX5808 mode (C5VRX CVBS path) with RSSI unaffected; video and RSSI under real conditions | Not started |

Two modes: **RX5808 mode** (default; one channel, continuous RSSI + video)
and **multi-channel mode** (up to 8 channels hopped at ~1 kHz each, no
video; the host reads per-channel peak RSSI over the same 3 wires).

### Phase A results (2026-10-09)

XIAO ESP32-C5, stock esp-sdr (rev e74f2a4), 10-bit captures, **no antenna**
(the lap-timer case), Mach 3 VTX. CSVs in `tools/phase_a/results/`.

- **Coverage: the whole FPV band.** R1-R8 all received (Report PASS; carrier
  25-44 dB). The synthesizer reaches E6/E7/E8 (5905/5925/5945): a VTX parked
  on R8 was found at the predicted offset with the C5 tuned to each, within
  0.01 MHz (`tunecheck`). The C5's frequency error is under ~20 kHz.
- **Fixed gain G = 60** (esp-sdr manual index, range 0-83). Noise floor
  -40 dBFS at 60; it rises a steady 2 dB per index above ~30 and sits at the
  ADC floor (~-56 dBFS) below. Gain 40 (esp-sdr's default) is effectively deaf.
- **Range: ~43 dB swing** at G = 60 with a 1000 mW VTX: -40 dBFS floor (far /
  off), smooth rise on approach, still a few dB above the floor at 12 ft.
- **Overload at contact range.** With the VTX touching the C5 the input
  clips (100%) at any useful gain: the front end saturates, so the in-band
  level stops tracking gain index (a 1 cm gain sweep looks non-monotonic for
  this reason). An RX5808 saturates close up too. FPVRaceOne stamps a lap at
  the FIRST sample of the highest value, so a clipped plateau biases the lap
  early by ~half its length. Phase B must measure at what distance clipping
  starts at race power.
- **Snapshot rate 22.7 readings/s** with stock esp-sdr, ~44x short of the
  >= 1000/s lap timing needs: the reason for Phase B.
- Quirks: the C5 delivers a mirrored spectrum (the tool conjugates it); an
  internal spur sits at 5920 MHz (148 x 40 MHz, +3.00 MHz on R8); one sweep
  showed a near-rail DC offset on R6 that the baseline did not repeat.

### Phase B2 results (2026-10-17)

Hop engine (`src/hop_engine.c`, bench firmware `-e hop`), XIAO C5, no
antenna, VTX on R3, gain 60, 256-sample snapshots, LO 10 MHz below the
pilot. The retune and trigger sequences come from Espressif's own
libphy/librftest (see the file header).

- **Rate: 1061 Hz per channel on all of R1-R8, worst gap 1.1 ms** over 60 s
  (63,655 cycles): no lost frames, CRC errors, failed snapshots or gain
  re-forces. Per slot 111.6 us: retune 62.6, snapshot 4.7, meter 44.2.
- **Retune options.** The full libphy sequence minus its 300 us wait takes
  98 us (815 Hz per channel). Skipping `phy_ckgen_5g_cal` and
  `phy_freq_mem_change_5g_` gives 62 us with no measurable cost, so that is
  the default (command `K`).
- **Settling: none needed.** All 56 ordered R-band pairs (incl. the R8->R1
  wrap) are within 1 dB of the 2 ms reading from the first 3.2 us window,
  with and without the skipped steps; the VTX channel's level does not
  depend on the channel hopped from (repeat runs within 0.2 dB).
- **Snapshot:** RF-dump at 80 MS/s confirmed (4096 words in 51.4 us), lands
  at 0x40830000, gain field = the forced index. Spectrum mirrored as in
  Phase A. **Image rejection ~38 dB**. The VTX's 6.5 MHz audio subcarriers are visible.
- **Levels:** floor -48 to -43 dBFS (R1/R2 lowest), reading noise 0.8 dB SD;
  VTX on R3 at -7 to -12 dBFS on the bench, ~35 dB above the floor.
- **Close VTX, other channels.** Waving the VTX around until R3 clipped
  (2,620 clipped readings) did not disturb the other slots: 3 readings
  above floor + 6 dB in ~445,000, none while R3 was clipping; R2/R4 track
  R3 only weakly (correlation ~0.2). Still to do at race power and gate
  distance with two VTXs (near/far).
- Open: the meter (44 us) has room to shrink; a second VTX for the
  near/far case.

### Video notes (kept from Phase B on)

C5VRX already demonstrates composite video from a XIAO C5. MODEM_DIAG I/Q
is looped out through GPIO pads into PARLIO RX (40 MS/s, Q4/I4) and a
circular GDMA ring; a BitScrambler demodulates it (phase delta at 20 MS/s);
PARLIO TX clocks it out to a 6-bit binary-weighted resistor ladder
(240 R .. 8.2 k) that sums onto one VIDEO node. It uses almost no CPU, so
the RSSI can be computed by the CPU from the same ring. Keeping video sets
these constraints:

- **One capture for both.** The video demodulator needs Q4/I4 (the top 4
  bits of the 10-bit I and Q) and PARLIO RX has at most 8 lanes, so the
  RSSI must use the same 4-bit samples: roughly 20-30 dB of instantaneous
  range, extended by C5VRX's exact +6/+12 dB lane remaps.
- **Gain conflict.** RSSI wants a fixed gain; video wants gain that tracks
  the signal. Plan: let gain move and report RSSI = measured power - current
  gain (dB) from a measured gain table. Phase A supports this: the noise
  floor rises a steady 2 dB per gain index above ~30. Risk: a gain step
  mid-pass glitches the lap peak; prefer a "hold while healthy" controller
  like C5VRX's. Front-end saturation at contact range (Phase A) is not
  fixed by gain at all.
- **Pins.** 8 IQ loopback pads + 6 ladder + 3 SPI + 1 RSSI PWM = 18 GPIOs;
  logging goes over USB. The XIAO has 11 header pins, so at least 7 IQ
  lanes must sit on pads that are not on the header. PARLIO goes through
  the GPIO matrix, so the ladder can use any output-capable pins. See the
  loopback pad table below; if the candidates fail, video needs a custom
  C5 board.
- **Quality.** C5VRX is not yet reliable at range; long sessions are
  untested. 6-bit output; ±180° phase-step ambiguity.
- **No audio** equivalent to the RX5808's audio pin.

#### IQ loopback pads (Phase B: test first)

An IQ loopback lane routes one MODEM_DIAG bit out of a pad and reads it
straight back into PARLIO RX through the same pad, at 40 MHz. The loop runs
through the pad's own driver and input buffer, so the pad needs no PCB
trace, and must have **nothing external on it**: any capacitance or load
corrupts the bit. A pad the board never connects is ideal.

What each non-header C5 pin connects to on the XIAO ESP32-C5 (from Seeed's
KiCad board file, V1.1 251225; chip U6, QFN40):

| GPIO | Chip pin | On the XIAO board | As a loopback |
|---|---|---|---|
| 2, 3, 4, 5 | 11-14 | bare test pads TP7-TP4 (JTAG) | **Clean.** 3/4/5 already proven by C5VRX |
| 26 | 36 | 1 MΩ pull-down + battery-sense load switch enable (U3, a few pF) | **Very likely OK.** The switch toggles; battery sense unused |
| 27 | 37 | yellow user LED (D3) | **Likely OK.** The LED glows dimly |
| 28 | 38 | BOOT button, 10 kΩ pull-up, 15 pF, TP2 | **Likely OK**, unless BOOT is pressed while running |
| 6 | 15 | 100k/100k battery divider **+ 1 nF** to ground | **No.** 1 nF cannot swing at 40 MHz |
| 15 | 25 (SPICS1) | nothing on the board, but it is the chip-select of the in-package 8 MB PSRAM | **No** (usable only on a C5 without PSRAM) |
| 13, 14 | 22, 23 | USB D-/D+ | No: loses native USB |
| 16-22 | 26-32 | external SPI flash (U5) | No |

GPIO2/3/26/27/28 are also strapping pins; that is fine for a loopback,
which drives them only after boot.

Resulting 18-GPIO map, with no spare pin:

- **IQ loopbacks (8):** GPIO2, 3, 4, 5, 26, 27, 28 + one header pin
- **Header (11):** 6 video ladder + 3 SPI + 1 RSSI PWM + that IQ lane
- **Strapping pins on the header:** D2 (GPIO25) and D3 (GPIO7). Keep host
  lines off them, so a host can't hold a strap at the wrong level during
  reset. Put the header IQ lane on one, and the ladder's 8.2 kΩ LSB on the
  other (a steady weak pull-down at reset). SPI and RSSI PWM go on
  ordinary header pins.
- **Versus C5VRX:** it has no SPI or RSSI (channel via the BOOT button and
  USB), so it can spend D0-D3/D10 on IQ lanes and D4-D9 on the ladder. Our
  map moves the IQ lanes off the header to free 4 pins, and gives up the
  BOOT button (GPIO28 becomes a loopback; it still works for flashing at
  power-up).

**First Phase B test:** route one known MODEM_DIAG bit to every candidate
pad (and a proven reference lane), capture with PARLIO RX at 40 MHz, and
require bit-exact agreement over a long soak. GPIO26/27/28 are the open
ones. If one fails, video on a bare XIAO is one pin short.

**First result (2026-10-16, `-e padtest`, IDF 6.0.2):** every candidate
passed with zero errors: GPIO2, 26, 27, 28 and both header lanes D2
(GPIO25) and D3 (GPIO7). Pattern test ~2 million bits per pad at 20 and at
40 MHz; live MODEM_DIAG bits 4, 9 and 14, 6-12 million samples each, every
lane identical to the references and toggling at the same rate. Only
blemish: GPIO28 (BOOT) 0.000001 mismatch on one DIAG bit (100x under the
fail threshold).

**Confirmed (2026-10-16, run with the dump-ring memory reservation):** the
same PASS on every candidate in both sets, and the **negative control
fails as it must**: a lane with its output switched off shows ~50% pattern
errors and never toggles on the live signal, while the good pads in the
same set still match exactly. So the test does detect a bad pad, and
**GPIO2, 3, 4, 5, 26, 27, 28 + D2 or D3 are confirmed as IQ loopback lanes;
the 18-GPIO map works on a bare XIAO.**

Long soak (60 s per step, 9 min): per lane, ~12M pattern bits at 20 MHz and
~12M at 40 MHz, plus 35-164M live MODEM_DIAG samples per bit, all with
**zero errors and zero lost captures** on every candidate in sets A and B.
The negative control again failed (50.0% pattern errors, no toggles).

Lessons from getting there:
- Don't drive GPIO6 as a loopback: its 1 nF load at 40 MHz wedged the chip
  (USB dead until a power cycle) every time.
- The radio's dump writer fills SRAM 0x40830000-0x4083FFFF; firmware that
  starts it must reserve that bank (`src/rf_min.c`).

## Layout

```
platformio.ini, CMakeLists.txt, sdkconfig.defaults   ESP-IDF project (XIAO C5)
src/                                                 firmware
tools/pio.ps1                                        PlatformIO wrapper (private toolchain)
tools/phase_a/                                       stock esp-sdr bench test (Python)
docs/                                                design notes, session hand-offs
```

## Building

The C5 needs ESP-IDF 6 and the pioarduino platform, which needs the
pioarduino fork of PlatformIO Core (>= 6.2). That is kept private to this
repo so it can't disturb other projects' PlatformIO installs.

One-time, from the repo root in PowerShell:

```
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install pioarduino
```

Then:

```
.\tools\pio.ps1 run                                  # build
.\tools\pio.ps1 run -t upload --upload-port COM8     # flash
.\tools\pio.ps1 device monitor --port COM8           # console
```

Use PowerShell or cmd, not Git Bash (the ESP-IDF installer refuses MSYS).
The first build downloads ESP-IDF into `%USERPROFILE%\.pio-c5`. The
framework is pinned to **ESP-IDF 6.0.2**, the version C5VRX is proven on;
its C5 `libphy.a` matches the SHA-256 C5VRX verifies.

The IQ loopback pad test (Phase B step 1, `src/padtest.c`) is its own
environment:

```
.\tools\pio.ps1 run -e padtest -t upload --upload-port COM7
.\tools\pio.ps1 device monitor --port COM7
```

**Flashing (how uploads avoid esptool's USB reset):** with ESP-IDF 6.0.2
firmware running, esptool's DTR/RTS reset of the USB Serial/JTAG port
wedges the XIAO's USB (writes time out) until it is unplugged. Bench-tested
2026-10-16:

| Firmware running when esptool resets | Result |
|---|---|
| ROM loader (BOOT held at plug-in) | works, every time |
| Plain scaffold | wedged |
| Scaffold, CPU switched to the crystal | wedged |
| Scaffold with the interrupt-driven USB driver (as esp-sdr) | wedged |

esp-sdr (built on its own pinned IDF commit) does not wedge, so the cause is
somewhere in IDF 6.0.2's startup. It wasn't pinned down further. C5VRX (also
6.0.2) avoids the reset the same way:

- Every firmware answers console key **`B`** (`src/dlboot.c`). It stops the
  radio, sets the force-download flag and restarts into the ROM loader.
- `tools/pio_upload.py` sends `B` before each upload. esptool then flashes
  with no reset. Afterwards the script clears the flag (it survives resets)
  and starts the firmware with a **watchdog reset**. A DTR/RTS
  `hard-reset` from the loader leaves this XIAO in the loader.
- `pio device monitor` opens the port with DTR/RTS low
  (`monitor_dtr/rts = 0`).

If the chip doesn't answer at all (wedged, or a firmware without the `B`
listener): unplug it, hold BOOT, plug it in, release BOOT, and upload again.
Never run plain `esptool` against a running board without
`--before no-reset`.

**Stuck console after a reset (fixed):** the watchdog reset makes the XIAO
re-enumerate. If the app starts while Windows still holds the USB bus in
reset, the serial IN FIFO can stay full for good: output is silent, input
still works. Register snapshots caught this in 7 of 9 boots after padtest.
A guard task in `src/dlboot.c` re-issues the FIFO flush and then resets the
serial IN FIFO when it stays full while the host polls. With it, 3 of 3
boots printed normally. Every firmware starts it: `dlboot_start_listener()`,
or `dlboot_start_usb_guard()` when the firmware runs its own USB driver
console.

## The drop-in contract (what the RX5808 does)

The full host interface, including register read-back and the
multi-channel extension, is specified in
**[docs/RX5808-C5_SPI_Interface.md](docs/RX5808-C5_SPI_Interface.md)**
(v0.9 draft, CC BY 4.0 so any host can implement it). The summary below is
the legacy RX5808 behaviour existing hosts depend on.

The host side, as FPVRaceOne drives it (`lib/RX5808/RX5808.cpp`):

**Pins:** CH1 = DATA, CH2 = SELECT (latch enable), CH3 = CLOCK, RSSI
(analog out), +5V, GND. The module must be in SPI mode (SPI_EN mod).

**Bus:** 3-wire, write-only, bit-banged by the host.
- SELECT high -> low starts a frame; SELECT high ends it (data latched).
- 25 bits, LSB first: 4-bit register address, 1 R/W bit (1 = write),
  20 data bits.
- DATA is set up, then CLOCK pulses high; the module samples on the rising
  edge.
- FPVRaceOne uses 300 us half-periods (~1.1 kbit/s); other hosts clock much
  faster. Design for anything up to at least 1 MHz.

**Registers the hosts use:**

| Reg | Use | Data |
|---|---|---|
| 0x1 | Synthesizer B (tune) | `(N << 7) + A`, where `tf = (MHz - 479) / 2`, `N = tf / 32`, `A = tf % 32` |
| 0xA | Power control | `0b11010000110111110011` = normal (unused blocks off); `0b11111111111111111111` = power down |
| 0xF | State/reset | 0 = reset (wake from power-down) |

**Timing hosts assume:** RSSI is valid ~35 ms after a tune
(`RX5808_MIN_TUNETIME`); hosts leave >= 30 ms between tunes.

**RSSI output:** analog, roughly 0-1 V (noise floor to strong signal),
monotonic in dB. Exact curve to be measured on a real module in Phase C. The
XIAO C5 has no DAC, so this will be PWM through an RC filter. The filter
corner sets the trade between ripple and response (a 150 mph pass is ~5 ms
at the gate).

**Electrical:** the RX5808 runs from 5 V with 3.3 V-tolerant logic. The XIAO
C5 is 3.3 V logic and can be fed 5 V on its 5V pin. Check host logic levels
before connecting a 5 V-logic host directly.

## Known risks

- The I/Q path is undocumented. An Espressif PHY library update or chip
  revision can break it. Pin the IDF version.
- The C5's tuning ceiling is undocumented. R8 (5917) and E6-E8
  (5905/5925/5945) were at risk. **Answered 2026-10-09:** all of R1-R8
  received, and the C5's synthesizer reaches E6, E7 and E8 (5945 MHz). A
  US-legal VTX can't transmit E7/E8, so this was shown by finding an R8
  carrier at the predicted offset with the C5 tuned to each
  (Phase A `tunecheck`).
- Close-range overload at the gate. Phase A: the input clips with a 1000 mW
  VTX touching the C5, at any useful gain. The distance where clipping
  starts at race power is still unmeasured (Phase B, with continuous data).
- The receiver takes over the radio, so no Wi-Fi on the receiver chip.

## License

GPL-3.0-only. Phase B builds on C5VRX (GPL-3.0-only) and esp-sdr
(GPL-3.0-or-later). This firmware runs on its own chip and talks to hosts
over wires, so MIT-licensed hosts such as FPVRaceOne are unaffected.

### Credits

This project adapts the ESP32-C5 receive-only RF bring-up and continuous
MODEM_DIAG I/Q capture (`src/rf_min.c`, radio settings in
`sdkconfig.defaults`; from `main/rf.c` and `sdkconfig.defaults` at commit
`14e190d`), and plans to build on the live CVBS video path, from
[C5VRX](https://github.com/Twotoz/C5VRX) by Twotoz and the C5VRX
contributors. Visit the [official C5VRX website](https://twotoz.github.io/C5VRX/)
for the original project and its Discord invite. See the source and license
notices for the applicable terms; this derivative is not an official C5VRX
release. Phase A uses the stock firmware of
[esp-sdr](https://github.com/ESPARGOS/esp-sdr) by ESPARGOS.
