// SPDX-License-Identifier: GPL-3.0-only
// RX58_ESP32C5 -- drop-in RX5808 replacement on a XIAO ESP32-C5.
//
// The default build (`.\tools\pio.ps1 run`, env xiao_c5) is the receiver:
// src/receiver.c behind the RX5808 bus (src/rx5808_bus.c), with a USB
// console and bench stream (src/rx_bench.c). The other envs are bench
// firmwares from the development phases: `-e hop` (multi-channel hop
// engine, src/hop_bench.c) and `-e padtest` (IQ loopback pad test).

#if defined(SDR_PADTEST)
#include "padtest.h"
#elif defined(SDR_HOPBENCH)
#include "hop_bench.h"
#else
#include "rx_bench.h"
#endif

void app_main(void)
{
#if defined(SDR_PADTEST)
    padtest_main();
#elif defined(SDR_HOPBENCH)
    hop_bench_main();
#else
    rx_bench_main();
#endif
}
