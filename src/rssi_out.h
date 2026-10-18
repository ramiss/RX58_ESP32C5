// SPDX-License-Identifier: GPL-3.0-only
// RSSI as PWM on a pin, RC-filtered into an RX5808 host's analog RSSI input.
//
// The voltage follows a real RX5808's RSSI curve (measured 2026-10-18 against
// two RX5808 modules, an FPVRaceOne and an FPVScanner, side by side with the
// C5 on its antenna, 25 mW VTX): linear in dB, ~370 mV with no signal,
// ~15 mV/dB, ~1.08 V where the RX5808 saturates.  The C5 resolves further
// (Option 1 "headroom"): the same slope continues to RSSI_OUT_MAX_MV where
// the C5 itself clips.
//
// Readings arrive referred to the receiver input (receiver.h RX_REF_GAIN), so
// a host's EXT GAIN does not shift the curve.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RSSI_OUT_GPIO 1              // XIAO D0

#define RSSI_OUT_FLOOR_DBFS   (-46)  // input level (dBFS at gain 60) of the RX5808s' floor
#define RSSI_OUT_FLOOR_MV      370   // RX5808 no-signal level (modules: 345-396 mV)
#define RSSI_OUT_MV_PER_DB      15   // RX5808 slope (log detector)
#define RSSI_OUT_MAX_MV       1200   // C5 clip at the default gain (RX5808 saturates ~1080)
// Filter full scale at 100% duty: 3.3 V x 1k / (1.5k + 1k) (README "RSSI filter").
#define RSSI_OUT_FULL_SCALE_MV 1320

esp_err_t rssi_out_init(void);
void rssi_out_set(uint16_t rssi16);     // 1/16 dB, input-referred 16 * (dBFS + 100)
uint32_t rssi_out_duty(void);           // last duty, 0-1023
uint32_t rssi_out_mv(void);             // last target voltage, mV
void rssi_out_full_scale(uint32_t mv);  // bench: filter full scale (calibration)
void rssi_out_enable(bool on);          // off = duty 0 (receiver powered down)
void rssi_out_force(int32_t duty);      // bench: fixed duty 0-1023; -1 = follow RSSI again
