// SPDX-License-Identifier: GPL-3.0-only
// Phase B step 1: which XIAO ESP32-C5 pads work as 40 MHz IQ loopback lanes.
#pragma once

// Runs the pad test, prints the results on the USB console, then serves
// console commands (r = rerun, l = long soak, s = summary, B = download mode).
// Never returns.
void padtest_main(void);
