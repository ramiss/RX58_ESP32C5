// SPDX-License-Identifier: GPL-3.0-only
// RX5808 3-wire bus slave: DATA D10 (GPIO10), SELECT D8 (GPIO8), CLOCK D1
// (GPIO0). docs/RX5808-C5_SPI_Interface.md sections 2-5.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BUS_PIN_DATA 10
#define BUS_PIN_SEL  8
#define BUS_PIN_CLK  0

typedef struct {
    uint32_t frames;         // SELECT cycles the SPI slave completed
    uint32_t writes;         // valid write frames (>= 25 bits, R/W = 1)
    uint32_t reads;          // R/W = 0 frames (answered when slow enough)
    uint32_t short_frames;   // fewer than 25 bits: discarded
    uint32_t overruns;       // a transaction could not be re-queued
    uint32_t floating;       // all-ones frames (undriven bus): ignored
    uint32_t requests;       // READ_SELECT writes
    uint32_t readouts;       // readout frames served
} bus_stats_t;

typedef struct {
    uint32_t t_us, bits, addr, data;
    bool write;
} bus_frame_t;

// Bring the slave up (no radio needed: call before Wi-Fi starts).
esp_err_t bus_init(void);

// Fast readout (datasheet 8): the builder fills buf (BUS_BLOCK_MAX bytes)
// with block n and returns its length; 0 = no such block. It runs in the
// SPI interrupt when a READ_SELECT request arrives.
#define BUS_BLOCK_MAX 64
typedef size_t (*bus_block_builder_t)(unsigned block, uint8_t *buf);
void bus_set_block_builder(bus_block_builder_t fn);
// Frames go into the register file from the SPI driver's interrupt; kept
// as a hook for the radio task (no-op).
void bus_poll(void);
// True while a slow (possible read) frame is in progress: the radio task
// should not mask interrupts then, because read bits must follow CLOCK
// within 5 us.
bool bus_slow_frame_active(void);
void bus_stats(bus_stats_t *out, bool reset);
// Input glitch filters on (bit set) or off: 1 = SELECT (250 ns flex),
// 2 = CLOCK, 4 = DATA (2-cycle pin filters). Returns the new mask. Call
// bus_filters(7) after the radio has started: hardware-SPI hosts (DATA
// changing on CLOCK falling) need the SELECT filter, and filters enabled
// before Wi-Fi starts block the pins.
unsigned bus_filters(unsigned mask);
// Most recent frames, newest last; returns how many were copied.
unsigned bus_log(bus_frame_t *out, unsigned max);
