// SPDX-License-Identifier: GPL-3.0-only
//
// Why this exists: with ESP-IDF 6.0.2 firmware running, esptool's USB
// Serial/JTAG reset (DTR/RTS) leaves the XIAO C5's USB dead until it is
// unplugged (found 2026-10-16: the plain scaffold, a crystal-clocked CPU and
// the interrupt-driven USB driver all wedge; a reset from the ROM loader
// does not). A software restart with the force-download flag set always
// reaches a working ROM loader, so uploads use that instead
// (tools/pio_upload.py), as C5VRX does.

#include "dlboot.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "heap_memory_layout.h"
#include "soc/lp_aon_reg.h"
#include "soc/usb_serial_jtag_reg.h"

// The radio's dump writer (src/rf_min.c) fills SRAM 0x40830000-0x4083FFFF
// (C5VRX hardware finding; 0x40840000 is untouched). Keep the heap out of
// that bank in every firmware, not only those that start the writer. This
// file is linked by every firmware (the B handler), so the reservation
// lives here.
SOC_RESERVE_MEMORY_REGION(0x40830000, 0x40840000, modem_dump_ring);

// Stuck console output: after the watchdog reset that starts new firmware,
// the XIAO re-enumerates. If the app starts while the host still holds the
// bus in reset (USB_SERIAL_JTAG_BUS_RESET_ST = 0 at boot), the serial IN
// FIFO can stay full forever although the host keeps polling it: output is
// silent, input still works (seen in 7 of 9 boots after padtest, register
// snapshots 2026-10-16). Recovery: if the FIFO stays full for 100 ms while
// the host sends SOFs, re-issue WR_DONE; after 200 ms also reset the serial
// IN async FIFO. With the port closed this only discards output nobody reads.
// A FIFO that is full but moving (the IN endpoint's pointers change between
// polls) is a busy stream, not a stuck one, and is left alone.
#define POLL_MS      20
#define FLUSH_POLLS  5
#define RESET_POLLS  10

void dlboot_reboot(void)
{
    printf("download_boot=requested\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    // Survives the restart; tools/pio_upload.py clears it after flashing.
    REG_SET_FIELD(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT, 1u);
    esp_restart();
}

static void guard_task(void *arg)
{
    const bool poll_key = arg != NULL;
    unsigned stuck = 0;
    uint32_t last_frame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
    uint32_t last_ep = REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG);
    for (;;) {
        if (poll_key) {
            uint8_t buf[16];
            while (usb_serial_jtag_ll_rxfifo_data_available()) {
                int n = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
                for (int i = 0; i < n; ++i)
                    if (buf[i] == DLBOOT_KEY) dlboot_reboot();
            }
        }
        uint32_t frame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
        bool host_alive = frame != last_frame;
        last_frame = frame;
        uint32_t ep = REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG);
        bool moving = ep != last_ep;
        last_ep = ep;
        bool in_free = REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE;
        stuck = (!in_free && host_alive && !moving) ? stuck + 1 : 0;
        if (stuck == FLUSH_POLLS) usb_serial_jtag_ll_txfifo_flush();
        if (stuck >= RESET_POLLS) {
            const uint32_t in_reset = USB_SERIAL_JTAG_SERIAL_IN_AFIFO_RESET_WR | USB_SERIAL_JTAG_SERIAL_IN_AFIFO_RESET_RD;
            REG_SET_BIT(USB_SERIAL_JTAG_SER_AFIFO_CONFIG_REG, in_reset);
            REG_CLR_BIT(USB_SERIAL_JTAG_SER_AFIFO_CONFIG_REG, in_reset);
            stuck = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void dlboot_start_listener(void)
{
    xTaskCreate(guard_task, "dlboot", 2048, (void *)1, 5, NULL);
}

void dlboot_start_usb_guard(void)
{
    xTaskCreate(guard_task, "usbguard", 2048, NULL, 5, NULL);
}
