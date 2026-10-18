// SPDX-License-Identifier: GPL-3.0-only
#include "bench_link.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"

#define FRAME_SYNC 0xA5

static uint32_t s_dropped;
static bool s_drop_flag;

static uint8_t crc8(const uint8_t *p, unsigned n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int b = 0; b < 8; ++b) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

bool bench_send_frame(uint8_t type, const void *payload, uint8_t len, TickType_t wait_ticks)
{
    uint8_t buf[3 + 255 + 1];
    buf[0] = FRAME_SYNC;
    buf[1] = type;
    buf[2] = len;
    memcpy(&buf[3], payload, len);
    buf[3 + len] = crc8(&buf[1], len + 2u);
    if (usb_serial_jtag_write_bytes(buf, len + 4u, wait_ticks) == 0) {
        s_dropped++;
        s_drop_flag = true;
        return false;
    }
    return true;
}

uint32_t bench_dropped(void) { return s_dropped; }

bool bench_take_drop_flag(void)
{
    bool f = s_drop_flag;
    s_drop_flag = false;
    return f;
}
