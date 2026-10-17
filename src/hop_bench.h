// SPDX-License-Identifier: GPL-3.0-only
// Phase B2 bench firmware: the hop engine with a USB console and a binary
// measurement stream (tools/phase_b). Built by `.\tools\pio.ps1 run -e hop`.
#pragma once

void hop_bench_main(void) __attribute__((noreturn));
