// SPDX-License-Identifier: GPL-3.0-only
// Binary frames on the USB console for the bench firmwares (tools/phase_b):
// 0xA5, type, len, payload[len], crc8(type, len, payload), poly 0x07, init 0.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

// wait_ticks 0: drop the frame if the USB TX buffer is full (live streams);
// otherwise block up to wait_ticks. Dropped frames are counted.
bool bench_send_frame(uint8_t type, const void *payload, uint8_t len, TickType_t wait_ticks);
uint32_t bench_dropped(void);
// True once after any drop (for a "frames were dropped before this" flag).
bool bench_take_drop_flag(void);

static inline void bench_put16(uint8_t **p, uint32_t v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); *p += 2; }
static inline void bench_put32(uint8_t **p, uint32_t v) { bench_put16(p, v); bench_put16(p, v >> 16); }
