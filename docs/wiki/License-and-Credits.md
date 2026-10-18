## License

The firmware is **GPL-3.0-only** (it builds on C5VRX and esp-sdr). It runs on
its own chip and talks to hosts over wires, so hosts under other licenses
(FPVRaceOne is MIT) are unaffected. The
[interface datasheet](Datasheet) is **CC BY 4.0**, so
anyone can implement the host side.

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
release.

[esp-sdr](https://github.com/ESPARGOS/esp-sdr) by ESPARGOS first published
the ESP32 radios' raw I/Q capture path; the first feasibility tests used its
stock firmware. The multi-channel hop engine (`src/hop_engine.c`) takes its
PLL retune and RF-dump trigger sequences from Espressif's own ESP-IDF
libraries.
