// SPDX-License-Identifier: GPL-3.0-only
// The receiver: RX5808 mode and multi-channel mode on the hop engine's
// 10-bit snapshots, switched in place over the bus (EXT MODE), with per-slot
// peak-hold and the fast-readout blocks (docs/RX5808-C5_SPI_Interface.md
// sections 7-10).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "regs.h"

#define RX_FW_VERSION      0x0001     // 0.1 (INFO bytes 4-5)
#define RX_DEFAULT_GAIN    60u

// What one receiver_step() produced, for the bench stream.
typedef struct {
    uint8_t mode;                     // MODE_RX5808 or MODE_MULTI
    uint8_t n;                        // readings in r[]
    uint32_t t0_us;                   // step start (esp_timer, low 32 bits)
    struct {
        uint8_t slot;                 // 0-7
        uint16_t rssi;                // 1/16 dB, after smoothing
        uint16_t raw;                 // before smoothing
        uint16_t t_us;                // after t0
        uint8_t clip;
        uint16_t mn, mx;              // RX5808 mode: snapshot spread in the reading
    } r[REGS_SLOTS];
    uint8_t snaps;                    // RX5808 mode: snapshots in the reading
    bool failed, reforced, retuned;   // a snapshot failed, gain re-forced, first after a retune
    uint16_t mhz;                     // RX5808 mode: tuned frequency
    uint32_t since_tune_us;
} rx_result_t;

// After hop_init() and bus_init(): take the power-on state from the
// register file, tune, and register the fast-readout block builder.
esp_err_t receiver_init(void);
// Apply bus changes (mode, frequency, slots, gain, power), then take one
// reading (RX5808 mode) or one hop cycle (multi-channel mode). False if
// there was nothing to measure (e.g. no active slot).
bool receiver_step(rx_result_t *out);

void receiver_set_per_reading(unsigned snapshots);    // RX5808 mode, 1-32
unsigned receiver_per_reading(void);
uint8_t receiver_mode(void);
bool receiver_powered_down(void);
uint16_t receiver_mhz(void);                           // RX5808 mode: tuned frequency
// Readings per second per active slot over the last second.
uint32_t receiver_rate(void);
// Mode switches done and the last one's latency (MODE write -> applied).
uint32_t receiver_switches(void);
uint32_t receiver_last_switch_us(void);
