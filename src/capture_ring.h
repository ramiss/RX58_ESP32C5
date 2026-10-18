// SPDX-License-Identifier: GPL-3.0-only
// Continuous Q4/I4 capture: MODEM_DIAG -> GPIO loopback lanes -> PARLIO RX
// 40 MS/s -> endless GDMA ring (adapted from C5VRX, see capture_ring.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RING_RATE_HZ   40000000u
#define RING_BYTES     32768u        // one sample per byte: Q in bits 0-3, I in 4-7

// Which 4 ADC bits each lane carries (sign + 3 magnitude bits).
typedef enum {
    RING_LANE_COARSE = 0,            // bits {9,8,7,6}: 64 ADC codes per step, full range
    RING_LANE_FINE = 1,              // bits {9,7,6,5}: 32 codes, +-256 window (+6 dB)
    RING_LANE_ULTRAFINE = 2,         // bits {9,6,5,4}: 16 codes, +-128 window (+12 dB)
    RING_LANES
} ring_lane_t;

// Route the lanes and start PARLIO RX into the ring. Call after the radio's
// continuous writer runs (rf_min_start).
esp_err_t ring_start(ring_lane_t lane);
esp_err_t ring_set_lane(ring_lane_t lane);
ring_lane_t ring_lane(void);
unsigned ring_lane_step(ring_lane_t lane);   // ADC codes per 4-bit step

// The newest completed GDMA descriptor: its samples and a sequence number
// that changes whenever a newer one completes. False before the ring runs.
bool ring_newest(const uint8_t **samples, size_t *len, uint32_t *seq);
// True if the descriptor returned with seq is still intact (RX has not
// wrapped back into it). Check after using the samples in place.
bool ring_still_valid(uint32_t seq);

uint32_t ring_faults(void);           // PARLIO FIFO overflow + GDMA error count
