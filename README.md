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
| B3 | RX5808-mode RSSI -> PWM RSSI pin; tune-to-valid < 35 ms | **Done 2026-10-17 (mode without video)**: 10-bit snapshots, 4360 readings/s, max gap 0.5 ms, tune-to-valid 0.35 ms (results below; `-e rx`). PWM ripple: needs the RC filter on the bench. Video mode's RSSI moves to D |
| C1 | Legacy SPI slave: RX5808 frames at any speed, register read-back | **Done 2026-10-18**: bench tests 18/18 (three runs; results below; test host `tools/spi_host`), RSSI RC filter checked, and an unmodified FPVRaceOne (XIAO C6) tunes the C5, tracks its RSSI and times laps |
| C2 | Extension: EXT register, mode switch, peak-hold, fast readout | **Bench tests pass 2026-10-18** (`spi_host.py c2` 22/22, C1 suite 18/18 on the same firmware; results below). Open: readout at 4 MHz, legacy reads during multi-channel mode |
| D | Video mode (C5VRX CVBS path) with gain-ranged 4-bit RSSI; video and RSSI under real conditions | Not started |

Modes: **RX5808 mode** (default; one channel, continuous RSSI from 10-bit
snapshots, no video), **video mode** (one channel, CVBS out, RSSI from the
video path's 4-bit samples with gain ranging; Phase D) and **multi-channel
mode** (up to 8 channels hopped at ~1 kHz each, no video; the host reads
per-channel peak RSSI over the same 3 wires). Video and full-range RSSI
can't share the radio's capture engine (Phase B3 results), hence separate
modes (decided 2026-10-17). One firmware image: every power-on starts in
RX5808 mode; extension-aware hosts request their mode over the bus (EXT
MODE) once they have found the receiver and confirm it by reading INFO.
Switches happen in place, no restart: RX5808 <-> multi-channel share the
snapshot engine (within ~10 ms), video up to 2 s (datasheet 7.4).

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

### Phase B3 results (2026-10-17)

RX5808 mode, bench firmware `-e rx` (`src/rx_bench.c`), VTX on R3, gain 60,
no antenna.

- **Why not the video path.** Three routes to RSSI alongside video were
  tested:
  - *The writer's 10-bit ring.* The continuous MODEM_DIAG writer's
    position is in `DUMP_PTR_MODE` bits 0-13 (a 16384-word ring at
    80 MS/s), but with its bank-owner setting nothing reaches the dump
    bank, and with the snapshot setting (owner 2) a CPU read of the bank
    is a load access fault (panic, USB dead until replug).
  - *The 4-bit video lanes* (`src/capture_ring.c`, C5VRX's PARLIO RX
    ring on our pad map) work, but at a fixed gain one 4-bit step is
    already ~-30 dBFS, above the noise floor (~-43): empty channel to VTX
    was 15.5 dB (ultrafine, folding at the VTX), 14.7 (fine), 8.6
    (coarse).
  - So RX5808 mode uses the B2 snapshots, without video.
- **Readings: 4360/s, worst gap 0.5 ms**, each the mean of 4 snapshots of
  256 samples (~200 us of CPU per reading, no retune on a fixed channel).
  Noise 0.39 dB SD per reading on an empty channel, 0.08 dB on the VTX.
- **Range: 28-33 dB** from empty channels (-43 to -48 dBFS) to the VTX
  on the bench (-15 dBFS); no clipping, no failed snapshots.
- **Tune-to-valid: 0.35 ms** (target < 35 ms): the first reading after a
  retune is already at the new level, 80 of 80 retunes (R3<->R7, R3<->R8).
- **PWM** (`src/rssi_out.c`): LEDC 10-bit at 78 kHz on D0, linear in dB
  between two set points (default -60..0 dBFS); follows every reading.
  Ripple and the dB -> volt curve still need the RC filter and a real
  RX5808 to compare against (Phase C).
- Lesson: polling the USB driver (and the GDMA registers) in a tight loop
  left the USB dead with no panic; bench loops poll the console once per ms.

### Phase C1 results (2026-10-17)

RX5808 bus slave (`src/rx5808_bus.c`, `src/regs.c`) in the RX5808-mode
firmware (`-e rx`), driven by a XIAO ESP32-C6 test host (`tools/spi_host`:
firmware + `spi_host.py test`) on 10-15 cm jumpers: C6 D10/D1/D8 -> C5
D10/D8/D1 (DATA/SELECT/CLOCK), GND. 18/18 checks pass, three runs in a row:

- **Writes at every host speed:** FPVRaceOne's exact timing (300 us per
  phase, 1.1 kbit/s), 250 kHz (ESP32 scanners), and 32-clock frames at
  0.25, 1 and 4 MHz with DATA changing on CLOCK falling (hardware-SPI
  hosts such as the AVR scanner). Frames of 25-32 clocks all decode.
- **Back-to-back frames (C1-0): 10/10 with SELECT high only 3 us and 1.5
  us between frames** (the FPV Scanner ESP32 case). Needed: decoding each
  frame in the SPI slave driver's interrupt callback and re-queueing its
  transaction there (re-queueing from a task lost the 9th and 10th).
- **Register read-back, RotorHazard style** (900 us address bits, DATA
  released 300 us after R/W, 10 us phases): 0x01 reads back as written;
  0x0E returns the identify word 0xC5009.
- **Tuning:** a 0x01 write retunes the receiver (5732 MHz -> 5731, the
  RX5808 2 MHz step); RSSI follows (-24 dBFS on the VTX, -45 dBFS 148 MHz
  away).
- **Power:** 0xF2FFF (Chorus RF Laptimer) keeps it on, 0xFFFFF powers down
  (RSSI pin 0), a 0x0F write wakes it and restores the defaults.
- **Shared bus:** a frame clocked with SELECT high is ignored.
- **Boot:** the bus is up 51 ms after power-on (target <= 150 ms), before
  the radio; the receiver starts on 5865 MHz like an RX5808.
- **RSSI pin through the RC filter** (2.2k/1k + 220 nF, then 1k + 100 nF;
  `spi_host.py rc`): level within 4 mV of the design at every duty
  (0-1.03 V), settled within 50 ms; residual spread is the C6 ADC's own
  noise (~24 counts SD, rare outliers), not ripple.
- **Unmodified FPVRaceOne end-to-end** (2026-10-18): FPVRaceOne on the XIAO
  C6, same wiring plus the filtered RSSI into its D2/A2. It tunes the C5,
  its calibration graph shows floor ~20-40, VTX ~100-107 and hand-over-
  antenna dips to ~45-50, and laps register.
- Lessons:
  - **Hardware-SPI hosts glitch SELECT.** On bench wiring, DATA and CLOCK
    edges at the same instant made SELECT glitch at the C5 (one frame
    arrived as nine 1-4 bit fragments). Fixed with the C5's 2-cycle pin
    glitch filters on all three lines. A 250 ns flex filter on SELECT
    also worked, but its state went bad during operation.
  - **Enable the filters after the radio starts:** filters created
    before Wi-Fi starts blocked the pins.
  - **The stalled-frame wait is bounded:** a frame stuck with SELECT low
    (e.g. an open SELECT wire) once stalled the radio task and the
    console; now the wait gives up after 5 ms without a clock edge.
- Test-host notes: the C6's SPI peripheral did not drive these pins and
  plain GPIO writes top out at ~0.8 MHz, so fast frames use dedicated GPIO.
  esptool has no watchdog reset for the C6, so `tools/pio_upload.py` arms
  its LP watchdog by hand (BOOT-free re-flashing, as on the C5).

### Phase C2 results (2026-10-18)

The receiver (`src/receiver.c`, bus `src/rx5808_bus.c`, registers
`src/regs.c`; firmware `-e rx`) with the extension of datasheet v0.9.4,
tested from the XIAO C6 host (`python tools\spi_host\spi_host.py c2`), VTX
on R3. 22/22 checks; the C1 legacy suite still passes 18/18.

- **One firmware, both modes on the snapshot engine.** Power-on is RX5808
  mode (4,900 readings/s); EXT MODE = 1 switches to multi-channel mode in
  place: **applied in 29-770 us** (target <= 10 ms), confirmed by the host
  with one INFO readout. Writing the current mode is a no-op.
- **Multi-channel: 1035-1095 readings/s per slot** with 8 slots (~0.9 ms
  per cycle, worst gap ~1 ms). The VTX slot's peak stands ~30 dB above the
  other seven (-11.8 vs -41..-47 dBFS).
- **Fast readout** (C5 SPI slave driving DATA during the readout frame):
  INFO 16 bytes in 0.55 ms and PEAKS for 8 slots (60 bytes) in 2 ms at
  250 kHz, 20/20 INFO readouts at ~0.8 MHz (the C6's plain-GPIO limit),
  CRC-16 good on every block; REGS matches the legacy reads; re-arm returns
  the same snapshot; an unknown block reads 0xFF.
- **Peak-hold:** per slot, first reading equal to the peak, `age_us`
  inside the interval; the sequence number counts snapshots.
- **EXT settings:** slots, RSSI-pin slot (a legacy 0x01 write in
  multi-channel mode moves it), GAIN (INFO shows 40, back to 60 on 0),
  OPTIONS (median-of-3 smoothing).
- Lessons:
  - **Keep interrupts on during a retune.** `hop_tune` masked them for
    ~60 us; in multi-channel mode that delayed the SPI interrupt past the
    host's 50 us request-to-readout gap. The readout then came back empty,
    and its all-ones bits arrived as a "0x0F reset" write. Now only the
    scheduler is held during a retune, and all-ones frames are ignored.
  - **A block must fit 64 bytes,** the SPI slave's limit without DMA. That
    forced the PEAKS repack (datasheet 0.9.4).
  - **The readout frame must follow its request directly:** at SELECT fall
    the slave can't tell a readout frame from a write.

### Video notes (kept from Phase B on)

C5VRX already demonstrates composite video from a XIAO C5. MODEM_DIAG I/Q
is looped out through GPIO pads into PARLIO RX (40 MS/s, Q4/I4) and a
circular GDMA ring; a BitScrambler demodulates it (phase delta at 20 MS/s);
PARLIO TX clocks it out to a 6-bit binary-weighted resistor ladder
(240 R .. 8.2 k) that sums onto one VIDEO node. It uses almost no CPU, so
the RSSI can be computed by the CPU from the same ring. Keeping video sets
these constraints:

- **One capture for both (video mode only).** The video demodulator needs
  Q4/I4 (the top 4 bits of the 10-bit I and Q) and PARLIO RX has at most 8
  lanes, so in video mode the RSSI must use the same 4-bit samples. Measured
  at a fixed gain: only ~15 dB from empty channel to VTX (Phase B3
  results), so video mode needs gain ranging. RX5808 mode without video
  uses 10-bit snapshots instead.
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
tools/phase_b/                                       hop engine / RX5808-mode bench tool (Python)
tools/spi_host/                                      RX5808 bus test host: XIAO C6 firmware + spi_host.py
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

This project adapts, from [C5VRX](https://github.com/Twotoz/C5VRX) by
Twotoz and the C5VRX contributors (all at commit `14e190d`):
- the ESP32-C5 receive-only RF bring-up and continuous MODEM_DIAG writer
  (`src/rf_min.c`, radio settings in `sdkconfig.defaults`; from
  `main/rf.c` and `sdkconfig.defaults`);
- the MODEM_DIAG -> GPIO loopback lane routing with its coarse / fine /
  ultrafine lane tables, and the PARLIO RX endless GDMA ring with the
  newest-descriptor access (`src/capture_ring.c`; from `main/rf.c`
  `route_modem_iq` and the RX half of `main/transport.c`). Changed: our
  pad map, RX only (no BitScrambler/TX yet), our own power metrics;
- and plans to build on its live CVBS video path.

Visit the [official C5VRX website](https://twotoz.github.io/C5VRX/)
for the original project and its Discord invite. See the source and license
notices for the applicable terms; this derivative is not an official C5VRX
release. Phase A uses the stock firmware of
[esp-sdr](https://github.com/ESPARGOS/esp-sdr) by ESPARGOS.

The multi-channel hop engine (`src/hop_engine.c`) takes its PLL retune and
RF-dump trigger sequences from Espressif's own ESP-IDF libraries.
