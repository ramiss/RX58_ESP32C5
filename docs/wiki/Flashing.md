## Flashing

Each [release](https://github.com/ramiss/RX58_ESP32C5/releases) carries
`RX58_ESP32C5-merged.bin`: bootloader, partition table and app in one file,
flashed at offset 0 (a local build's `firmware.factory.bin` is the same
thing).

**Windows:** download the
[RX58_ESP32C5 Flasher](https://github.com/ramiss/RX58_ESP32C5-Flasher/releases/latest/download/RX58_ESP32C5-Flasher.exe),
a portable app that flashes any release straight from GitHub (or a local
`.bin`). A new board needs BOOT held while plugging it in the first time; a
running receiver is updated without touching it.

**Any platform, with [esptool](https://github.com/espressif/esptool) 5:**

1. Unplug the XIAO, hold **BOOT**, plug it in, release BOOT.
2. Flash:
   ```
   esptool --chip esp32c5 --port COM7 --before no-reset --after no-reset write-flash 0x0 RX58_ESP32C5-merged.bin
   ```
3. Unplug and replug (or press RESET) to start the firmware.

**Updating a running receiver:** send the single character `B` on its USB
serial port; it reboots into the ROM loader ("download_boot=requested"),
then flash as in step 2. `.\tools\pio.ps1 run -t upload` does all of this
for you.

> **Don't let esptool reset a running receiver.** With this firmware
> running, esptool's default DTR/RTS reset leaves the XIAO's USB dead until
> it is unplugged. Always pass `--before no-reset` (enter the loader with
> `B` or the BOOT button instead), and open the serial port with DTR and
> RTS low.

---
