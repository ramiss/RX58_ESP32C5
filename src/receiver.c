// SPDX-License-Identifier: GPL-3.0-only
//
// The receiver: one firmware, three modes (datasheet 7.4). RX5808 mode and
// multi-channel mode both run on the hop engine's 10-bit RF-dump snapshots
// (src/hop_engine.c), so a switch between them is a change of what the next
// step measures: in place, no restart. Video mode (Phase D) is not built
// yet; INFO reports no video capability and a MODE = 2 request is ignored.
//
// RX5808 mode: one channel (register 0x01), no retune between readings;
//   each reading is the mean of N snapshots (default 4, ~4400 readings/s).
// Multi-channel mode: hop_cycle() over the active slots (~1 kHz each with 8).
//
// Every reading goes through the optional median-of-3 (OPTIONS bit 0) into
// the slot's peak-hold; the RSSI-pin slot drives the PWM. A READ_SELECT
// request snapshots a block in the SPI interrupt (receiver_block()).

#include "receiver.h"
#include "hop_engine.h"
#include "rssi_out.h"
#include "rx5808_bus.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define CAP_VIDEO       0x01
#define CAP_MULTI       0x02
#define CAP_FAST_READ   0x04
#define CAP_READ_BACK   0x08
#define INFO_POWERED_DOWN 0x01
#define INFO_SWITCHING    0x02
#define INFO_OVERLOAD     0x04

static hop_config_t s_cfg = {
    .gain = RX_DEFAULT_GAIN,
    .skip = HOP_SKIP_CKGEN | HOP_SKIP_FREQMEM,   // README "Phase B2 results"
    .lo_side = -1,
    .mirrored = 1,
    .samples = 256,
};

static volatile uint8_t s_mode = MODE_RX5808;
static volatile bool s_powered_down;
static volatile uint16_t s_mhz;                  // RX5808 mode: tuned
static volatile uint8_t s_active_mask = 0x01;    // PEAKS / INFO slot mask
static volatile uint8_t s_pin_slot = 1;
static unsigned s_per_reading = 4;
static bool s_retune;                            // RX5808 mode: next step retunes
static int64_t s_tune_us;
static bool s_retuned;
static uint32_t s_switches, s_last_switch_us;
static volatile uint32_t s_rate;
static uint32_t s_rate_count;
static int64_t s_rate_t0;

// Per-slot peak-hold (section 9.2), shared with the SPI interrupt.
typedef struct {
    uint16_t peak, latest;
    int64_t peak_t;
    bool valid, clipped;
} peak_t;
static peak_t s_pk[REGS_SLOTS];
static volatile bool s_missed = true;            // readings missed since the last PEAKS (power-on)
static volatile bool s_overload;                 // clipping since the last INFO
static uint16_t s_pk_seq;
static portMUX_TYPE s_pk_mux = portMUX_INITIALIZER_UNLOCKED;

// Median-of-3 history per slot.
static uint16_t s_hist[REGS_SLOTS][3];
static uint8_t s_hist_n[REGS_SLOTS];
static bool s_smoothing = true;

static void reset_slot_state(void)
{
    portENTER_CRITICAL(&s_pk_mux);
    memset(s_pk, 0, sizeof s_pk);
    s_missed = true;
    portEXIT_CRITICAL(&s_pk_mux);
    memset(s_hist_n, 0, sizeof s_hist_n);
}

static uint16_t smooth(unsigned slot, uint16_t v)
{
    if (!s_smoothing) return v;
    uint16_t *h = s_hist[slot];
    h[0] = h[1];
    h[1] = h[2];
    h[2] = v;
    if (s_hist_n[slot] < 3) { s_hist_n[slot]++; return v; }
    const uint16_t a = h[0], b = h[1], c = h[2];
    return a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
}

static void record(unsigned slot, uint16_t rssi, uint8_t clip, int64_t t)
{
    portENTER_CRITICAL(&s_pk_mux);
    peak_t *p = &s_pk[slot];
    // age_us counts from the FIRST reading equal to the peak: replace only
    // on a strictly higher value.
    if (!p->valid || rssi > p->peak) { p->peak = rssi; p->peak_t = t; }
    p->latest = rssi;
    p->valid = true;
    if (clip) { p->clipped = true; s_overload = true; }
    portEXIT_CRITICAL(&s_pk_mux);
    if (slot == (unsigned)s_pin_slot - 1u || s_mode == MODE_RX5808) rssi_out_set(rssi);
    s_rate_count++;
}

static void tune(uint16_t mhz)
{
    hop_tune(hop_lo_mhz(mhz));
    s_tune_us = esp_timer_get_time();
    s_mhz = mhz;
    s_retuned = true;
}

// Bus changes -> receiver state. Runs at the start of every step.
static void apply_changes(void)
{
    bus_poll();
    if (!regs_take_changes() && !s_retune) return;
    ext_state_t e;
    regs_ext(&e);

    const bool down = regs_powered_down();
    if (down != s_powered_down) {
        s_powered_down = down;
        rssi_out_enable(!down);
    }
    s_smoothing = e.options & OPT_SMOOTHING;
    s_pin_slot = e.pin_slot;
    const uint8_t gain = e.gain ? e.gain : RX_DEFAULT_GAIN;

    // Video mode is not built yet: a MODE = 2 request leaves the mode as is.
    const uint8_t want = e.mode == MODE_VIDEO ? s_mode : e.mode;
    if (want != s_mode) {
        s_mode = want;
        reset_slot_state();
        s_retune = true;                         // RX5808 mode must retune after hopping
        s_switches++;
        s_last_switch_us = (uint32_t)(esp_timer_get_time() - regs_mode_write_us());
    }

    hop_config_t c = s_cfg;
    c.gain = gain;
    uint8_t mask = 0;
    if (s_mode == MODE_MULTI) {
        for (int s = 0; s < REGS_SLOTS; ++s) {
            c.mhz[s] = e.slot_mhz[s];
            if (c.mhz[s]) mask |= 1u << s;
        }
    } else {
        memset(c.mhz, 0, sizeof c.mhz);
        mask = 0x01;
    }
    if (mask != s_active_mask) reset_slot_state();
    s_active_mask = mask;
    s_cfg = c;
    hop_set_config(&s_cfg);

    if (s_mode == MODE_RX5808) {
        const uint32_t mhz = regs_mhz();
        if (mhz && (mhz != s_mhz || s_retune)) tune((uint16_t)mhz);
        if (!s_mhz) tune(5865u);
    }
    // Multi-channel mode retunes every slot each cycle anyway.
    s_retune = false;
}

static bool step_rx5808(rx_result_t *out)
{
    const bool reforced = hop_gain_check();
    uint32_t sum = 0, n = 0, clip = 0;
    uint16_t mn = 0xffff, mx = 0;
    bool failed = false;
    for (unsigned k = 0; k < s_per_reading; ++k) {
        // A snapshot masks interrupts for ~5 us; a slow bus frame may be a
        // read whose bits must follow CLOCK within 5 us, so wait it out
        // (bounded: bus_slow_frame_active() gives up on a stalled frame).
        while (bus_slow_frame_active()) {}
        if (!hop_capture(s_cfg.samples, NULL)) { failed = true; continue; }
        uint8_t c;
        const uint16_t r = hop_measure(0, s_cfg.samples, &c);
        sum += r;
        n++;
        clip += c;
        if (r < mn) mn = r;
        if (r > mx) mx = r;
    }
    const int64_t now = esp_timer_get_time();
    out->mode = MODE_RX5808;
    out->failed = failed;
    out->reforced = reforced;
    out->retuned = s_retuned;
    s_retuned = false;
    out->mhz = s_mhz;
    out->since_tune_us = (uint32_t)(now - s_tune_us);
    out->snaps = (uint8_t)n;
    out->n = 0;
    if (!n) return true;
    const uint16_t raw = (uint16_t)((sum + n / 2) / n);
    const uint16_t v = smooth(0, raw);
    record(0, v, clip > 0, now);
    out->n = 1;
    out->r[0].slot = 0;
    out->r[0].rssi = v;
    out->r[0].raw = raw;
    out->r[0].t_us = (uint16_t)(now - (int64_t)out->t0_us);
    out->r[0].clip = (uint8_t)(clip > 255 ? 255 : clip);
    out->r[0].mn = mn;
    out->r[0].mx = mx;
    return true;
}

static bool step_multi(rx_result_t *out)
{
    // The cycle masks interrupts in short bursts (retune ~60 us, snapshot
    // ~5 us), which would delay a legacy read's data bits. Start it with the
    // bus idle. A read that begins mid-cycle can still meet one burst.
    // TODO(C2): measure legacy reads in multi-channel mode; if a retune burst
    // lands on a data bit, check bus_slow_frame_active() per slot.
    while (bus_slow_frame_active()) {}
    hop_reading_t r[REGS_SLOTS];
    uint8_t mask;
    uint32_t t0;
    const unsigned n = hop_cycle(r, &mask, &t0);
    // hop_cycle() reports 32-bit times; peak ages need the full clock.
    const int64_t now = esp_timer_get_time();
    const uint32_t now32 = (uint32_t)now;
    out->mode = MODE_MULTI;
    out->t0_us = t0;
    out->n = 0;
    if (!n) return false;
    unsigned k = 0;
    for (unsigned s = 0; s < REGS_SLOTS && k < n; ++s) {
        if (!(mask >> s & 1u)) continue;
        if (r[k].rssi) {
            const uint16_t v = smooth(s, r[k].rssi);
            const int64_t t = now - (int64_t)(uint32_t)(now32 - (t0 + r[k].t_us));
            record(s, v, r[k].clip, t);
            out->r[out->n].slot = (uint8_t)s;
            out->r[out->n].rssi = v;
            out->r[out->n].raw = r[k].rssi;
            out->r[out->n].t_us = r[k].t_us;
            out->r[out->n].clip = r[k].clip;
            out->n++;
        }
        ++k;
    }
    return true;
}

bool receiver_step(rx_result_t *out)
{
    apply_changes();
    out->t0_us = (uint32_t)esp_timer_get_time();
    const bool ok = s_mode == MODE_MULTI ? step_multi(out) : step_rx5808(out);
    const int64_t now = esp_timer_get_time();
    if (now - s_rate_t0 >= 1000000) {
        unsigned slots = __builtin_popcount(s_active_mask);
        s_rate = slots ? s_rate_count / slots : 0;
        s_rate_count = 0;
        s_rate_t0 = now;
    }
    return ok;
}

// ---- Fast-readout blocks (section 9), built in the SPI interrupt --------

static IRAM_ATTR uint16_t crc16(const uint8_t *p, size_t n)
{
    // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.
    uint16_t c = 0xffff;
    while (n--) {
        c ^= (uint16_t)(*p++) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static IRAM_ATTR void put16(uint8_t *b, size_t *i, uint32_t v) { b[(*i)++] = (uint8_t)v; b[(*i)++] = (uint8_t)(v >> 8); }
static IRAM_ATTR void put32(uint8_t *b, size_t *i, uint32_t v) { put16(b, i, v); put16(b, i, v >> 16); }

static IRAM_ATTR size_t finish(uint8_t *b, size_t i)
{
    const uint16_t c = crc16(b, i);
    put16(b, &i, c);
    return i;
}

static IRAM_ATTR size_t block_info(uint8_t *b)
{
    ext_state_t e;
    regs_ext(&e);
    size_t i = 0;
    b[i++] = 0xC5;
    b[i++] = (REGS_IDENTIFY >> 8) & 0xfu;            // protocol major
    b[i++] = REGS_IDENTIFY & 0xffu;                  // protocol minor
    b[i++] = CAP_MULTI | CAP_FAST_READ | CAP_READ_BACK;
    put16(b, &i, RX_FW_VERSION);
    b[i++] = s_mode;
    const bool switching = e.mode != s_mode && e.mode != MODE_VIDEO;
    b[i++] = (uint8_t)((s_powered_down ? INFO_POWERED_DOWN : 0) | (switching || s_retune ? INFO_SWITCHING : 0) |
                       (s_overload ? INFO_OVERLOAD : 0));
    s_overload = false;
    const uint16_t f = s_mode == MODE_MULTI ? e.slot_mhz[(s_pin_slot - 1u) & 7u] : s_mhz;
    put16(b, &i, f);
    b[i++] = s_active_mask;
    b[i++] = s_cfg.gain;
    put16(b, &i, s_rate > 65535 ? 65535 : s_rate);
    return finish(b, i);
}

// PEAKS (section 9.2, 64-byte layout): seq u16, snap_us u32, slot mask,
// valid mask, clipped mask, flags; per active slot: peak, age_us, latest
// (u16 each); CRC-16. 8 slots = 60 bytes. Starts a new peak interval.
static IRAM_ATTR size_t block_peaks(uint8_t *b)
{
    size_t i = 0;
    const int64_t snap = esp_timer_get_time();
    const uint8_t mask = s_active_mask;
    put16(b, &i, ++s_pk_seq);
    put32(b, &i, (uint32_t)snap);
    const size_t at_masks = i;
    b[i++] = mask;
    b[i++] = 0;                                      // valid, filled below
    b[i++] = 0;                                      // clipped
    uint8_t valid = 0, clipped = 0;
    portENTER_CRITICAL_ISR(&s_pk_mux);
    b[i++] = s_missed ? 0x01 : 0x00;
    s_missed = false;
    for (unsigned s = 0; s < REGS_SLOTS; ++s) {
        if (!(mask >> s & 1u)) continue;
        peak_t *p = &s_pk[s];
        int64_t age = snap - p->peak_t;
        if (age < 0) age = 0;
        if (p->valid) valid |= 1u << s;
        if (p->clipped) clipped |= 1u << s;
        put16(b, &i, p->valid ? p->peak : 0);
        put16(b, &i, p->valid && age < 0xffff ? (uint32_t)age : 0xffffu);
        put16(b, &i, p->latest);
        p->valid = false;
        p->clipped = false;
        p->peak = 0;
    }
    portEXIT_CRITICAL_ISR(&s_pk_mux);
    b[at_masks + 1] = valid;
    b[at_masks + 2] = clipped;
    return finish(b, i);
}

static IRAM_ATTR size_t block_regs(uint8_t *b)
{
    size_t i = 0;
    for (uint32_t a = 0; a < 16; ++a) {
        const uint32_t v = regs_read(a);
        b[i++] = (uint8_t)v;
        b[i++] = (uint8_t)(v >> 8);
        b[i++] = (uint8_t)(v >> 16);
    }
    return finish(b, i);
}

static IRAM_ATTR size_t build_block(unsigned block, uint8_t *buf)
{
    switch (block) {
    case 0: return block_info(buf);
    case 1: return block_peaks(buf);
    case 2: return block_regs(buf);
    default: return 0;                               // reads as 0xFF, CRC will not match
    }
}

esp_err_t receiver_init(void)
{
    bus_set_block_builder(build_block);
    (void)regs_take_changes();
    s_powered_down = regs_powered_down();
    rssi_out_enable(!s_powered_down);
    s_retune = true;
    apply_changes();
    s_rate_t0 = esp_timer_get_time();
    return ESP_OK;
}

void receiver_set_per_reading(unsigned snapshots)
{
    if (snapshots >= 1 && snapshots <= 32) s_per_reading = snapshots;
}

unsigned receiver_per_reading(void) { return s_per_reading; }
uint8_t receiver_mode(void) { return s_mode; }
bool receiver_powered_down(void) { return s_powered_down; }
uint16_t receiver_mhz(void) { return s_mhz; }
uint32_t receiver_rate(void) { return s_rate; }
uint32_t receiver_switches(void) { return s_switches; }
uint32_t receiver_last_switch_us(void) { return s_last_switch_us; }
