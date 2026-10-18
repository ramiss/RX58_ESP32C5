## Using it from a host

### Legacy hosts (no changes)

The receiver behaves like an RX5808 in SPI mode:

- **Tune:** write register 0x01 = `(N << 7) | A` with `tf = (MHz - 479) / 2`,
  `N = tf / 32`, `A = tf % 32` (R counter 0x00 = 8, the default).
- **Power down:** write 0x0A = 0xFFFFF (all ones). Any other value keeps it
  on, including the values hosts write to disable unused blocks.
- **Wake / reset:** write 0x0F (any data).
- **RSSI:** the analog pin, valid well within the 35 ms hosts already wait.
- **Read-back:** a read of any register returns what was written, so
  RotorHazard's tuning check passes. A real RX5808 doesn't answer reads.

Frames of 25 to 32 clocks are accepted at any speed up to 4 MHz;
back-to-back frames with only 1.5 µs between them are fine. Several
receivers can share DATA and CLOCK with separate SELECT lines.

### Extension-aware hosts (multi-channel)

Every power-on starts in RX5808 mode. A host that wants more:

1. **Detect:** read register 0x0E. **0xC5009** means an RX58_ESP32C5 (protocol
   0.9); a real RX5808 reads 0xFFFFF through a pull-up.
2. **Switch mode:** EXT write MODE = 1 (register 0x0E, data `0x00001`). The
   switch happens in place in under 1 ms; confirm it by reading the INFO
   block (mode byte = 1, "switching" flag clear).
3. **Set channels:** EXT writes SLOT1..SLOT8 = frequency in MHz (data
   `(slot << 16) | MHz`; 0 = slot off).
4. **Poll** every 5-10 ms: request the PEAKS block (EXT READ_SELECT = 1),
   wait 50 µs, then clock out 10 + 6 × slots + 2 bytes in the next frame,
   with DATA as an input. Check the CRC-16.
5. **Time laps:** for each slot, the peak happened `age_us` before the
   request frame's SELECT rose. Feed `(peak, t_request - age_us)` to the
   lap logic.

```c
// Sketch (host side). ext(sub, value) = write register 0x0E, (sub << 16) | value.
if (rx_read(0x0E) == 0xC5009) {
    ext(0x0, 1);                                   // MODE = multi-channel
    for (int s = 0; s < 8; ++s) ext(0x1 + s, raceband[s]);
    for (;;) {                                     // every 5-10 ms
        ext(0xB, 1);                               // READ_SELECT = PEAKS
        uint32_t t_req = micros();                 // when SELECT rose
        delay_us(50);
        read_block(buf, 10 + 6 * 8 + 2);           // next frame, DATA as input
        if (!crc_ok(buf)) { ext(0xB, 0x81); read_block(...); }   // re-read same snapshot
        for each slot k: lap_sample(k, peak[k], t_req - age_us[k]);
    }
}
```

RSSI values are unsigned 16-bit in 1/16 dB (= 16 × (dBFS + 100) in this
firmware); compare them between slots of one receiver, and calibrate
thresholds per receiver as with an RX5808.

Everything else (block layouts, timing, the EXT sub-registers, gain and
smoothing options, re-arm for retries, power-on defaults, what happens after
a receiver restart) is in the
**[datasheet](Datasheet)**.

---
