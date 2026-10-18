# Development log

How the RX58_ESP32C5 receiver was developed and verified, phase by phase:
bench results, dead ends and lessons. For using the receiver, see the
[README](Home) and the host datasheet
([RX58_ESP32C5_SPI_Interface.md](Datasheet)).

## Phases

| Phase | What | State |
|---|---|---|
| A | Bench-test stock esp-sdr: channel coverage 5645-5945 MHz, gain/overload, snapshot rate | **Done 2026-10-09** (bench tool `tools/phase_a`, not published) |
| B0 | IQ loopback pad test (18-GPIO map on a bare XIAO) | **Done 2026-10-16**: all candidates pass, zero errors in a 9-min soak; the negative control fails as it must |
| B1 | SPI interface datasheet: legacy RX5808 emulation + multi-channel extension | **Done** (v0.9 draft 2026-10-16, now v0.9.8) |
| C3 | RSSI curve matched to real RX5808s | **Done 2026-10-18**: antenna required; gain 51; filter R1 1.5 kΩ |
| B2 | Multi-channel hop engine: up to 8 channels at ~1 kHz each | **Done 2026-10-17**: 1061 Hz per channel on R1-R8, max gap 1.1 ms (`-e hop`, bench tool `tools/phase_b`) |
| B3 | RX5808-mode RSSI -> PWM RSSI pin | **Done 2026-10-17** (mode without video): 10-bit snapshots, 4360 readings/s, tune-to-valid 0.35 ms |
| C1 | Legacy SPI slave: RX5808 frames at any speed, register read-back | **Done 2026-10-18**: 18/18 bench checks, RC filter checked, unmodified FPVRaceOne times laps |
| C2 | Extension: EXT register, in-place mode switch, peak-hold, fast readout | **Done 2026-10-18**: 29/29 bench checks incl. readout at 4 MHz |
| D | Video mode (C5VRX CVBS path) with gain-ranged 4-bit RSSI | On hold (user decision 2026-10-18: finish everything else first) |

Key decisions:
- **Modes (2026-10-17).** Video and full-range RSSI can't share the radio's
  capture engine (B3), so RX5808 mode has no video and video gets its own
  mode. One firmware image; every power-on starts in RX5808 mode; hosts
  switch modes over the bus, in place, without a restart (datasheet 7.4).
- **Fixed gain 60, AGC off** for lap timing (the vendor AGC flattens the
  peak).

## Phase A: stock esp-sdr (2026-10-09)

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
  level stops tracking gain index. An RX5808 saturates close up too.
  FPVRaceOne stamps a lap at the FIRST sample of the highest value, so a
  clipped plateau biases the lap early by ~half its length.
- **Snapshot rate 22.7 readings/s** with stock esp-sdr, ~44x short of the
  >= 1000/s lap timing needs: the reason for the custom firmware.
- Quirks: the C5 delivers a mirrored spectrum; an internal spur sits at
  5920 MHz (148 x 40 MHz, +3.00 MHz on R8); one sweep showed a near-rail DC
  offset on R6 that the baseline did not repeat.

## Phase B0: IQ loopback pads (2026-10-16)

Video needs 18 GPIOs (8 IQ loopback lanes + 6 ladder + 3 SPI + 1 RSSI PWM)
and the XIAO has 11 header pins, so most IQ lanes must sit on pads that are
not on the header. An IQ loopback lane routes one MODEM_DIAG bit out of a pad
and reads it straight back into PARLIO RX through the same pad, at 40 MHz:
the pad needs no trace but must have **nothing external on it**.

What each non-header C5 pin connects to on the XIAO ESP32-C5 (Seeed's KiCad
board file, V1.1 251225; chip U6, QFN40):

| GPIO | Chip pin | On the XIAO board | As a loopback |
|---|---|---|---|
| 2, 3, 4, 5 | 11-14 | bare test pads TP7-TP4 (JTAG) | **Clean.** 3/4/5 already proven by C5VRX |
| 26 | 36 | 1 MΩ pull-down + battery-sense load switch enable (U3, a few pF) | OK (tested) |
| 27 | 37 | yellow user LED (D3) | OK (tested); the LED glows dimly |
| 28 | 38 | BOOT button, 10 kΩ pull-up, 15 pF, TP2 | OK (tested), unless BOOT is pressed while running |
| 6 | 15 | 100k/100k battery divider **+ 1 nF** to ground | **No.** Driving it at 40 MHz wedged the chip every time |
| 15 | 25 (SPICS1) | chip-select of the in-package 8 MB PSRAM | **No** |
| 13, 14 | 22, 23 | USB D-/D+ | No |
| 16-22 | 26-32 | external SPI flash (U5) | No |

Results (`-e padtest`, IDF 6.0.2):
- Every candidate passed with zero errors: GPIO2, 26, 27, 28 and both header
  lanes D2 (GPIO25) and D3 (GPIO7). Pattern test ~2M bits per pad at 20 and
  40 MHz; live MODEM_DIAG bits 4, 9 and 14, 6-12M samples each.
- **The negative control fails as it must:** a lane with its output switched
  off shows ~50% pattern errors and never toggles on the live signal.
- **Long soak** (60 s per step, 9 min): ~12M pattern bits per lane per rate,
  35-164M live samples per bit, zero errors and zero lost captures.
- So **GPIO2, 3, 4, 5, 26, 27, 28 + D2 are confirmed as IQ loopback lanes;
  the 18-GPIO map works on a bare XIAO.**
- Lessons: never drive GPIO6 as a loopback; the radio's dump writer fills
  SRAM 0x40830000-0x4083FFFF, so every firmware reserves that bank
  (`src/dlboot.c`).

## Phase B2: multi-channel hop engine (2026-10-17)

`src/hop_engine.c`, bench firmware `-e hop`, no antenna, VTX on R3, gain 60,
256-sample snapshots, LO 10 MHz below the pilot. The retune and trigger
sequences come from Espressif's own libphy/librftest (see the file header).

- **Rate: 1061 Hz per channel on all of R1-R8, worst gap 1.1 ms** over 60 s
  (63,655 cycles): no lost frames, CRC errors, failed snapshots or gain
  re-forces. Per slot 111.6 us: retune 62.6, snapshot 4.7, meter 44.2.
- **Retune options.** The full libphy sequence minus its 300 us wait takes
  98 us (815 Hz per channel). Skipping `phy_ckgen_5g_cal` and
  `phy_freq_mem_change_5g_` gives 62 us with no measurable cost: the default.
- **Settling: none needed.** All 56 ordered R-band pairs (incl. the R8->R1
  wrap) are within 1 dB of the 2 ms reading from the first 3.2 us window;
  the VTX channel's level does not depend on the channel hopped from.
- **Snapshot:** RF-dump at 80 MS/s confirmed (4096 words in 51.4 us), lands
  at 0x40830000, gain field = the forced index. Spectrum mirrored as in
  Phase A. **Image rejection ~38 dB.** The VTX's 6.5 MHz audio subcarriers
  are visible.
- **Levels:** floor -48 to -43 dBFS, reading noise 0.8 dB SD; VTX on R3 at
  -7 to -12 dBFS on the bench, ~35 dB above the floor.
- **Close VTX, other channels.** Waving the VTX around until R3 clipped did
  not disturb the other slots: 3 readings above floor + 6 dB in ~445,000,
  none while R3 was clipping.

## Phase B3: RX5808-mode RSSI (2026-10-17)

- **Why not the video path.** Three routes to RSSI alongside video were
  tested:
  - *The writer's 10-bit ring.* The continuous MODEM_DIAG writer's position
    is in `DUMP_PTR_MODE` bits 0-13 (a 16384-word ring at 80 MS/s), but with
    its bank-owner setting nothing reaches the dump bank, and with the
    snapshot setting (owner 2) a CPU read of the bank is a load access fault
    (panic, USB dead until replug).
  - *The 4-bit video lanes* (`src/capture_ring.c`, C5VRX's PARLIO RX ring on
    our pad map) work, but at a fixed gain one 4-bit step is already
    ~-30 dBFS, above the noise floor: empty channel to VTX was 15.5 dB
    (ultrafine), 14.7 (fine), 8.6 (coarse).
  - So RX5808 mode uses the B2 snapshots, without video.
- **Readings: 4360/s, worst gap 0.5 ms**, each the mean of 4 snapshots.
  Noise 0.39 dB SD per reading on an empty channel, 0.08 dB on the VTX.
- **Range: 28-33 dB** from empty channels to the VTX on the bench.
- **Tune-to-valid: 0.35 ms**, 80 of 80 retunes.
- Lesson: polling the USB driver (and the GDMA registers) in a tight loop
  left the USB dead with no panic; busy loops poll the console once per ms.

## Phase C1: legacy bus (2026-10-17/18)

Bus slave `src/rx5808_bus.c` + `src/regs.c`, driven by a XIAO ESP32-C6 test
host (`tools/spi_host`) on 10-15 cm jumpers. 18/18 checks, three runs:

- **Writes at every host speed:** FPVRaceOne's exact timing (300 us per
  phase), 250 kHz (ESP32 scanners), and 32-clock frames at 0.25, 1 and 4 MHz
  with DATA changing on CLOCK falling (hardware-SPI hosts).
- **Back-to-back frames: 10/10 with SELECT high only 1.5 or 3 us between
  frames** (the FPV Scanner ESP32 case). Needed: decoding each frame in the
  SPI slave driver's interrupt callback and re-queueing its transaction there.
- **RotorHazard-style read-back** of 0x01; identify word 0xC5009.
- **Power:** 0xF2FFF (Chorus RF Laptimer) keeps it on, 0xFFFFF powers down,
  0x0F wakes it. **Shared bus:** frames with SELECT high are ignored.
- **Boot:** bus up 51 ms after power-on, before the radio.
- **RSSI pin through the RC filter:** level within 4 mV of the design at
  every duty (0-1.03 V), settled within 50 ms; the residual spread at the
  host is its ADC's own noise. (First attempt used 220 uF / 100 uF by
  mistake: right levels after 2 s, but ~1000x too slow.)
- **Unmodified FPVRaceOne end-to-end (2026-10-18):** it tunes the C5, its
  calibration graph shows floor ~20-40, VTX ~100-107 and hand-over-antenna
  dips to ~45-50, and laps register.
- Lessons:
  - Hardware-SPI hosts glitch SELECT on bench wiring; the C5's 2-cycle pin
    glitch filters on all three lines fix it. A 250 ns flex filter also
    worked but its state went bad during operation.
  - Create/enable the filters after the radio starts: created before Wi-Fi
    starts, they blocked the pins.
  - A frame stuck with SELECT low once stalled the radio task and the
    console; the wait is now bounded.
  - Test host: the C6's SPI peripheral did not drive these pins and plain
    GPIO writes top out at ~0.8 MHz, so fast frames use dedicated GPIO.

## Phase C2: extension (2026-10-18)

`src/receiver.c` (RX5808 + multi-channel mode on the snapshot engine),
fast readout in `src/rx5808_bus.c`. `spi_host.py c2`: 29/29; the C1 suite
still 18/18 on the same firmware.

- **In-place mode switch applied in 29-770 us**, confirmed by one INFO
  readout. **Multi-channel: 1035-1095 readings/s per slot** (8 slots); the
  VTX slot ~30 dB above the other seven.
- **Fast readout at 1, 2 and 4 MHz: every CRC good.** PEAKS (8 slots,
  60 bytes) 120 us at 4 MHz while hopping; INFO 32 us.
- **Legacy reads while hopping:** 20/20 RotorHazard-timed reads.
- Lessons:
  - `hop_tune` masked interrupts for ~60 us; in multi-channel mode that
    delayed the SPI interrupt past the host's 50 us request-to-readout gap:
    empty readouts, and the all-ones readout bits arrived as a "0x0F reset"
    write. Now only the scheduler is held during a retune, and all-ones
    frames are ignored.
  - A block must fit 64 bytes (SPI slave without DMA): PEAKS was repacked.
  - The readout frame must follow its request directly; a SELECT glitch in
    between uses up the readout.
  - The INFO readings/s window must restart on a mode or slot change.
  - Snapshots pause only during a legacy read's data bits (not its ~5 ms
    address phase, which cost a 5.8 ms reading gap in the first soak).

## Phase C3: RSSI curve (2026-10-18)

Tool: `tools/rssi_curve/rssi_curve.py` (pairs the C5's readings with
reference receivers over the same 100-200 ms; raw streams kept), chart:
`tools/rssi_curve/plot_curve.py`. Mach 3 VTX on R3 (5732 MHz); the
receivers side by side, RX5808s aimed at the VTX. References: an
FPVRaceOne (C6 + RX5808, 100 kΩ RSSI pulldown, its USB `rssi/start`
stream) and an FPVScanner's C6 + RX5808 reflashed with the `tools/spi_host`
test host (RSSI pin read directly in mV). The C5's own RSSI pin read by the
bench test host.

- **Without the C5's antenna it can't follow an RX5808.** At 600 mW and
  25 mW alike, both RX5808s ran from floor to saturation while the C5 was
  still within 2 dB of its noise floor: the C5 on a bare U.FL socket is
  30-40 dB less sensitive. (An early 600 mW run looked closer only because
  the FPVRaceOne wasn't pointed at the VTX: its RX5808 sits over a ground
  plane and is directional.)
- **With the antenna the two match:** C5 floor and RX5808 floors coincide,
  and the RX5808 is linear in dB: ~396 mV (module A) / ~345 mV (module B)
  with no signal, ~15 mV/dB, saturating at ~1.08 V.
- **Gain vs headroom** (VTX at a fixed spot, 3 passes): the signal falls
  ~1.0 dB per gain index; the floor falls almost as fast down to ~51, then
  flattens into the ADC floor (~-57.5 dBFS below index 38). SNR: 40.1 dB at
  60, 39.2 at 54 and 51, 37.3 at 48, 34.3 at 45. Default gain now **51**:
  < 1 dB of sensitivity for ~9 dB of clip margin.
- **Curve** (`src/rssi_out.*`): mV = 370 + 15 x (level - floor),
  level = dBFS + (60 - gain), clamped 370-1,200 mV; filter R1 2.2 kΩ ->
  1.5 kΩ for a 1.32 V full scale (rc check: within 10 mV, 0-1.31 V).
- **Confirmation run** (5,896 pairs): the C5 pin sits within the two
  modules' spread from the floor up (e.g. 10 dB: 0.55 V vs 0.56 / 0.39 V;
  40 dB: 1.01 V vs 1.01 / 0.82 V), and keeps rising ~4 dB past the RX5808s'
  saturation to 1.19 V. The headroom measured smaller than the gain sweep's
  9 dB; the C5 clips at 0 dBFS within ~5 cm at 25 mW.
- **FPVRaceOne counts:** its DMA ADC path is ~9.6 mV per count (113 counts
  = 1,092 mV at saturation), not the 4.02 mV its polled-path scale implies.
- Lessons: the bare C6 + RX5808 rig dropped its USB when the module was
  powered: FPVRaceOne's 100 µF + 10 µF across the module's 5 V are needed.
  A 1.5 Ω resistor in place of 1.5 kΩ (R1) looked exactly like a missing
  divider. FPVScanner hardware takes our test host unchanged (same pins);
  its Arduino firmware entered the ROM loader with esptool's normal USB
  reset (the wedge is specific to our IDF 6.0.2 firmware).
- Gain compensation moved to where readings leave the meter
  (`receiver.c` input_level(), `RX_REF_GAIN` 60), so PEAKS, INFO, the RSSI
  pin and the bench stream are all input-referred, as datasheet 10.2
  specifies. Check (VTX off): the floor reads -46.1 / -46.6 / -45.9 at gains
  60 / 57 / 54 (was a 5.4 dB spread), -44.3 at 51, -43.4 at 48.

## Flashing: why uploads avoid esptool's USB reset (2026-10-16)

With ESP-IDF 6.0.2 firmware running, esptool's DTR/RTS reset of the USB
Serial/JTAG port wedges the XIAO's USB (writes time out) until it is
unplugged:

| Firmware running when esptool resets | Result |
|---|---|
| ROM loader (BOOT held at plug-in) | works, every time |
| Plain scaffold | wedged |
| Scaffold, CPU switched to the crystal | wedged |
| Scaffold with the interrupt-driven USB driver (as esp-sdr) | wedged |

esp-sdr (its own pinned IDF commit) does not wedge, so the cause is somewhere
in IDF 6.0.2's startup; it wasn't pinned down further. C5VRX avoids the reset
the same way we do: the firmware answers console key `B` (reboot into the ROM
loader), and `tools/pio_upload.py` flashes with no reset and starts the new
firmware with a watchdog reset (on the C6 test host: an LP watchdog armed by
hand, since esptool has none for the C6).

**Stuck console after a reset (fixed):** after the watchdog reset the XIAO
re-enumerates; if the app starts while the host still holds the USB bus in
reset, the serial IN FIFO can stay full for good (7 of 9 boots after
padtest). A guard task in `src/dlboot.c` re-issues the FIFO flush and then
resets the serial IN FIFO when it stays full while the host polls.

## Video notes (for Phase D)

C5VRX demonstrates composite video from a XIAO C5: MODEM_DIAG I/Q looped out
through GPIO pads into PARLIO RX (40 MS/s, Q4/I4) and a circular GDMA ring; a
BitScrambler demodulates it; PARLIO TX clocks it out to a 6-bit resistor
ladder (240 R .. 8.2 k) summed onto one VIDEO node.

- **RSSI in video mode** must use the same 4-bit samples (PARLIO RX has at
  most 8 lanes): ~15 dB at a fixed gain (B3), so video mode needs gain
  ranging: RSSI = measured power - current gain from a measured gain table
  (the noise floor rises a steady 2 dB per gain index above ~30).
- **Pins.** 8 IQ loopback pads (GPIO2-5, 26-28, D2) + 6 ladder (D3-D7, D9)
  + 3 SPI + 1 RSSI PWM = 18 GPIOs, no spare.
- **Quality.** C5VRX is not yet reliable at range; 6-bit output; ±180°
  phase-step ambiguity. No audio.

## Known risks

- The I/Q path and the PHY routines are undocumented. An Espressif PHY
  library update or chip revision can break them: the IDF version is pinned
  (6.0.2, libphy SHA-256 as C5VRX verifies).
- Close-range overload at the gate (Phase A); the distance where clipping
  starts at race power, and the near/far case with two VTXs, are still to be
  measured.
- The receiver takes over the radio: no Wi-Fi on the receiver chip.
