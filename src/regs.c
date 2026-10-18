// SPDX-License-Identifier: GPL-3.0-only
#include "regs.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// regs_write() runs in the SPI slave's interrupt, so everything it touches
// lives in RAM.

// RTC6715 datasheet defaults (interface datasheet, section 4).
static const DRAM_ATTR uint32_t DEFAULTS[16] = {
    0x00008, 0x02A05, 0xDB643, 0x03980, 0x1E1CE, 0x7E1D2, 0x82408, 0x82408,
    0x03F40, 0xB2018, 0x10C13, 0, 0, 0, 0, 0,
};

static uint32_t s_regs[16] = {
    0x00008, 0x02A05, 0xDB643, 0x03980, 0x1E1CE, 0x7E1D2, 0x82408, 0x82408,
    0x03F40, 0xB2018, 0x10C13, 0, 0, 0, 0, 0,
};
// Power-on EXT state (section 7.4): RX5808 mode, slot 1 = register 0x01's
// default frequency (5865 MHz), other slots off, smoothing on.
static ext_state_t s_ext = {
    .mode = MODE_RX5808, .slot_mhz = {5865}, .pin_slot = 1, .gain = 0, .options = OPT_SMOOTHING,
};
static volatile bool s_changed;
static volatile int64_t s_mode_write_us;
static regs_select_cb_t s_select_cb;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static IRAM_ATTR uint32_t decode_mhz(void)
{
    const uint32_t r = s_regs[REG_SYNTH_A] & 0x7fffu, b = s_regs[REG_SYNTH_B];
    if (r == 0) return 0;
    const uint32_t n = (b >> 7) & 0x1fffu, a = b & 0x7fu;
    // Section 4.1: f = 2 * (N * 32 + A) * (8 / R) + 479.
    const uint32_t mhz = 2u * (n * 32u + a) * 8u / r + 479u;
    return mhz >= REGS_MIN_MHZ && mhz <= REGS_MAX_MHZ ? mhz : 0;
}

IRAM_ATTR void regs_reset(void)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    // Section 4.3: defaults, out of power-down, frequency kept until 0x01
    // is written again. The mode and the EXT settings are not touched (7.4).
    const uint32_t synth_a = s_regs[REG_SYNTH_A], synth_b = s_regs[REG_SYNTH_B];
    for (int i = 0; i < 16; ++i) s_regs[i] = DEFAULTS[i];
    s_regs[REG_SYNTH_A] = synth_a;
    s_regs[REG_SYNTH_B] = synth_b;
    s_changed = true;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

// EXT write (section 7.1): D19-D16 sub-register, D15-D0 value.
static IRAM_ATTR void ext_write(uint32_t data)
{
    const uint32_t sub = (data >> 16) & 0xfu, v = data & 0xffffu;
    if (sub == EXT_READ_SELECT) {
        if (s_select_cb) s_select_cb(v);
        return;
    }
    bool changed = false;
    portENTER_CRITICAL_SAFE(&s_mux);
    if (sub == EXT_MODE && v <= MODE_VIDEO) {
        changed = s_ext.mode != v;
        s_ext.mode = (uint8_t)v;
        if (changed) s_mode_write_us = esp_timer_get_time();
    } else if (sub >= EXT_SLOT1 && sub < EXT_SLOT1 + REGS_SLOTS &&
               (v == 0 || (v >= REGS_MIN_MHZ && v <= REGS_MAX_MHZ))) {
        s_ext.slot_mhz[sub - EXT_SLOT1] = (uint16_t)v;
        changed = true;
    } else if (sub == EXT_RSSI_PIN_SLOT && v >= 1 && v <= REGS_SLOTS) {
        s_ext.pin_slot = (uint8_t)v;
        changed = true;
    } else if (sub == EXT_GAIN && v <= 83) {
        s_ext.gain = (uint8_t)v;
        changed = true;
    } else if (sub == EXT_OPTIONS) {
        s_ext.options = (uint8_t)(v & OPT_SMOOTHING);
        changed = true;
    }
    if (changed) s_changed = true;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

IRAM_ATTR bool regs_write(uint32_t addr, uint32_t data)
{
    addr &= 0xfu;
    data &= 0xfffffu;
    if (addr == REG_STATE) { regs_reset(); return true; }
    if (addr == REG_EXT) { ext_write(data); return true; }
    portENTER_CRITICAL_SAFE(&s_mux);
    const uint32_t old = s_regs[addr];
    s_regs[addr] = data;
    const bool matters = (addr == REG_SYNTH_A || addr == REG_SYNTH_B || addr == REG_POWER) && old != data;
    // A tuning write also sets the RSSI-pin slot (section 7.2), so slot 1
    // follows register 0x01 in both modes by default.
    if ((addr == REG_SYNTH_A || addr == REG_SYNTH_B)) {
        const uint32_t mhz = decode_mhz();
        if (mhz) s_ext.slot_mhz[s_ext.pin_slot - 1] = (uint16_t)mhz;
    }
    if (matters) s_changed = true;
    portEXIT_CRITICAL_SAFE(&s_mux);
    return matters;
}

IRAM_ATTR uint32_t regs_read(uint32_t addr)
{
    addr &= 0xfu;
    if (addr == REG_EXT) return REGS_IDENTIFY;
    if (addr == REG_STATE) return 0;
    return s_regs[addr];
}

void regs_set_select_cb(regs_select_cb_t cb) { s_select_cb = cb; }

int64_t regs_mode_write_us(void) { return s_mode_write_us; }

IRAM_ATTR uint32_t regs_mhz(void) { return decode_mhz(); }

bool regs_powered_down(void) { return s_regs[REG_POWER] == 0xfffffu; }

IRAM_ATTR void regs_ext(ext_state_t *out)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    *out = s_ext;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

bool regs_take_changes(void)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    const bool c = s_changed;
    s_changed = false;
    portEXIT_CRITICAL_SAFE(&s_mux);
    return c;
}

uint32_t regs_encode_mhz(uint32_t mhz)
{
    const uint32_t tf = (mhz - 479u) / 2u;
    return ((tf / 32u) << 7) | (tf % 32u);
}
