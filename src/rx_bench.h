// SPDX-License-Identifier: GPL-3.0-only
// Phase B3 bench firmware: RX5808 mode without video (one channel, RSSI from
// 10-bit snapshots, PWM). Built by `.\tools\pio.ps1 run -e rx`.
#pragma once

void rx_bench_main(void) __attribute__((noreturn));
