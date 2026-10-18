# PlatformIO upload hooks (extra_scripts = post:tools/pio_upload.py).
#
# esptool's USB Serial/JTAG reset (DTR/RTS) wedges the XIAO C5's USB when
# ESP-IDF 6.0.2 firmware is running: writes time out until it is unplugged
# (2026-10-16; README "Flashing"). From the ROM loader the same reset is
# fine, and so is a watchdog reset. So uploads never use the DTR/RTS reset
# (platformio.ini: board_upload.before_reset / after_reset = no-reset):
#
#  before: send the console key 'B' (src/dlboot.c). The firmware stops the
#          radio, sets LP_AON_FORCE_DOWNLOAD_BOOT and restarts into the ROM
#          loader. If nothing acknowledges, the chip may already be in the
#          loader (BOOT held at plug-in), which is checked with esptool.
#  after:  clear LP_AON_FORCE_DOWNLOAD_BOOT (it survives resets) and start
#          the new firmware with a watchdog reset. (A DTR/RTS "hard reset"
#          from the loader leaves this XIAO in the loader.)
#
# LP_AON_SYS_CFG_REG = 0x600B1034, FORCE_DOWNLOAD_BOOT = bits 29-30 on the
# C5, bit 30 on the C6 (ESP-IDF soc/esp32c5|c6/register/soc/lp_aon_reg.h).

import subprocess
import time

import serial

Import("env")  # noqa: F821  (SCons)

LP_AON_SYS_CFG = "0x600B1034"
# FORCE_DOWNLOAD_BOOT: bits 29-30 on the C5, bit 30 alone on the C6.
FORCE_DOWNLOAD_MASK = {"esp32c5": "0x60000000", "esp32c6": "0x40000000"}
DLBOOT_KEY = b"B"            # src/dlboot.h DLBOOT_KEY
ACK = b"download_boot=requested"


def _port(env):
    if not env.subst("$UPLOAD_PORT"):
        env.AutodetectUploadPort()
    return env.subst("$UPLOAD_PORT")


def _esptool(env, port, *args):
    # The board's MCU: esp32c5 for the receiver; an ESP32-C6 project can use
    # the same reset-free upload.
    chip = env.BoardConfig().get("build.mcu", "esp32c5")
    cmd = [env.subst("$PYTHONEXE"), "-m", "esptool", "--chip", chip, "--port", port,
           "--before", "no-reset"] + list(args)
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, r.stdout + r.stderr


def _request_download(port):
    """Send the download key to the running firmware. True if acknowledged."""
    for _ in range(3):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 0.05, 0.5
        s.dtr = False   # asserting DTR/RTS on open would trigger the USB reset
        s.rts = False
        try:
            s.open()
        except serial.SerialException:
            time.sleep(0.5)
            continue
        seen = b""
        try:
            s.reset_input_buffer()
            s.write(DLBOOT_KEY)
            deadline = time.time() + 1.0
            while time.time() < deadline and ACK not in seen:
                seen = (seen + s.read(512))[-4096:]
        except serial.SerialTimeoutException:
            pass        # nobody reads the port: the ROM loader or a wedged chip
        except serial.SerialException:
            seen += ACK  # the port vanished mid-read: the restart happened
        finally:
            try:
                s.close()
            except serial.SerialException:
                pass
        if ACK in seen:
            return True
        time.sleep(0.3)
    return False


def enter_download(source, target, env):
    port = _port(env)
    if not port:
        print("pre-upload: no upload port found")
        return
    if _request_download(port):
        print("pre-upload: firmware rebooted into the ROM loader")
        time.sleep(1.5)
        return
    rc, _ = _esptool(env, port, "--after", "no-reset", "read-mac")
    if rc == 0:
        print("pre-upload: chip is already in the ROM loader")
    else:
        print("pre-upload: neither the firmware nor the ROM loader answers.\n"
              "  Unplug the XIAO, hold BOOT, plug it in, release BOOT, then upload again.")


def start_firmware(source, target, env):
    port = _port(env)
    chip = env.BoardConfig().get("build.mcu", "esp32c5")
    rc, out = _esptool(env, port, "--after", "no-reset", "write-mem", LP_AON_SYS_CFG, "0",
                       FORCE_DOWNLOAD_MASK[chip])
    if rc != 0:
        print("post-upload: could not clear the force-download flag; unplug and replug the XIAO")
        return
    if chip == "esp32c6":
        # esptool has no watchdog reset for the C6 (it falls back to RTS,
        # which stays in the loader, as does a system software reset: the
        # strapping pins are only re-read at a chip reset). Arm the LP RTC
        # watchdog by hand, as esptool does on the C3/C5 (bench 2026-10-17:
        # the port survives and the app starts).
        for reg, val in (("0x600B1C18", "0x50D83AA1"),   # LP_WDT_RWDT_WPROTECT: unlock
                         ("0x600B1C04", "2000"),         # CONFIG1: stage-0 timeout
                         ("0x600B1C00", "0xD0000102")):  # CONFIG0: enable, system reset
            _esptool(env, port, "--after", "no-reset", "write-mem", reg, val)
        time.sleep(1.0)
        print("post-upload: new firmware started (LP watchdog reset)")
        return
    rc, out = _esptool(env, port, "--after", "watchdog-reset", "read-mac")
    # esptool can report an error after a successful watchdog reset because
    # the port disappears while it closes it.
    if rc == 0 or "Hard resetting with a watchdog" in out:
        print("post-upload: new firmware started (watchdog reset)")
    else:
        print("post-upload: reset failed; unplug and replug the XIAO")


env.AddPreAction("upload", enter_download)  # noqa: F821
env.AddPostAction("upload", start_firmware)  # noqa: F821
