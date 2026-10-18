// SPDX-License-Identifier: GPL-3.0-only
// RSSI as PWM on a pin, RC-filtered into an RX5808 host's analog RSSI input.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RSSI_OUT_GPIO 1              // XIAO D0

esp_err_t rssi_out_init(void);
// Readings at or below lo_dbfs give duty 0, at or above hi_dbfs full duty;
// linear in dB between (the real RX5808 curve is fitted in Phase C).
void rssi_out_range(int lo_dbfs, int hi_dbfs);
void rssi_out_set(uint16_t rssi16);  // 1/16 dB, 16 * (dBFS + 100)
uint32_t rssi_out_duty(void);        // last duty, 0-1023
void rssi_out_enable(bool on);       // off = duty 0 (receiver powered down)
void rssi_out_force(int32_t duty);   // bench: fixed duty 0-1023; -1 = follow RSSI again
