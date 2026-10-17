// SPDX-License-Identifier: GPL-3.0-only
//
// Reboot into the ROM download loader on request, so flashing never needs
// esptool's USB reset, and keep the USB console from sticking after the
// re-enumeration that follows a reset (README "Flashing").

#pragma once

// The console key that requests a download reboot. tools/pio_upload.py
// sends it before every upload.
#define DLBOOT_KEY 'B'

// Print "download_boot=requested", set the force-download flag and restart.
// Callers that started the radio stop it first (rf_min_reboot_to_download).
void dlboot_reboot(void) __attribute__((noreturn));

// For firmware without its own console (stdout on IDF's driverless USB
// console): a task that polls the receive FIFO, calls dlboot_reboot() on
// DLBOOT_KEY (other input is discarded) and runs the stuck-output guard.
void dlboot_start_listener(void);

// For firmware with its own console (USB driver installed, which must
// handle DLBOOT_KEY itself): only the stuck-output guard.
void dlboot_start_usb_guard(void);
