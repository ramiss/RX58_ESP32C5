// SPDX-License-Identifier: GPL-3.0-only
// Multi-channel hop engine (Phase B2): fast PLL hop -> short RF-dump
// snapshot -> band power, round-robin over up to 8 channels.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define HOP_MAX_SLOTS    8
#define HOP_MIN_SAMPLES  64
#define HOP_MAX_SAMPLES  2048      // per stream snapshot
#define HOP_RAW_MAX      16384     // whole dump bank (words)
#define HOP_SAMPLE_MHZ   80        // RF-dump sample rate
#define HOP_IF_MHZ       10        // LO offset; = HOP_SAMPLE_MHZ / 8 (meter table period)

// hop_config_t.skip: leave out parts of libphy's retune sequence.
#define HOP_SKIP_CKGEN   0x01      // phy_ckgen_5g_cal
#define HOP_SKIP_FREQMEM 0x02      // phy_freq_mem_change_5g_

typedef struct {
    uint16_t mhz[HOP_MAX_SLOTS];   // pilot frequency per slot, 0 = slot off
    uint8_t gain;                  // forced RX gain index
    uint8_t skip;                  // HOP_SKIP_*
    int8_t lo_side;                // -1: LO = f - 10 MHz, +1: LO = f + 10 MHz
    uint8_t mirrored;              // 1: captures are spectrally mirrored (Phase A)
    uint16_t settle_us;            // extra wait between hop and snapshot
    uint16_t samples;              // snapshot length, multiple of 8
} hop_config_t;

typedef struct {
    uint16_t rssi;                 // 1/16 dB, = 16 * (dBFS + 100); 0 = no reading
    uint16_t t_us;                 // end of snapshot, us after the cycle start
    uint8_t clip;                  // samples on a 10-bit rail (saturates at 255)
} hop_reading_t;

typedef struct {
    uint32_t n, min, max;
    uint64_t sum;                  // CPU cycles
} hop_stat_t;

typedef struct {
    hop_stat_t tune, capture, meter, slot, cycle;
    uint32_t cycles, timeouts, gain_rewrites;
} hop_stats_t;

// Start the radio as a receive-only front end and set up the RF-dump
// engine for software-triggered snapshots.
esp_err_t hop_init(const hop_config_t *cfg);
void hop_set_config(const hop_config_t *cfg);
const hop_config_t *hop_config(void);

// One pass over the active slots. Returns how many readings were written to
// out (in slot order); *mask gets the active-slot bitmap, *t0_us the cycle
// start (esp_timer, low 32 bits).
unsigned hop_cycle(hop_reading_t out[HOP_MAX_SLOTS], uint8_t *mask, uint32_t *t0_us);

// Primitives for bench tests.
uint32_t hop_lo_mhz(uint16_t pilot_mhz);
void hop_tune(uint32_t lo_mhz);
// Snapshot of n words into the dump bank. False on timeout or when the last
// word was not written. *cycles (optional) = trigger to done.
bool hop_capture(uint32_t n, uint32_t *cycles);
// Band power of words [first, first + n) of the dump bank (n multiple of 8).
uint16_t hop_measure(uint32_t first, uint32_t n, uint8_t *clip);
volatile uint32_t *hop_dump_ram(void);
bool hop_gain_check(void);         // re-force the gain if it moved; true if rewritten

void hop_stats_get(hop_stats_t *out, bool reset);
