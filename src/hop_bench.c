// SPDX-License-Identifier: GPL-3.0-only
//
// Phase B2 bench firmware: hop engine + USB console + binary stream.
//
// Commands (ASCII, one per line; replies are text lines "OK ..." / "ERR ..."):
//   F m1 [m2 .. m8]   slot pilot frequencies in MHz (0 = slot off)
//   G g               forced RX gain index
//   K mask            skip parts of the retune: 1 = ckgen_5g_cal, 2 = freq_mem_change_5g
//   L side            LO side: -1 = LO 10 MHz below the pilot, 1 = above
//   M 0|1             captures are spectrally mirrored (1, Phase A finding)
//   D us              extra settle time between hop and snapshot
//   N n               snapshot length in samples (64-2048, multiple of 8)
//   R / X             start / stop the measurement stream
//   T                 timing statistics (text; resets them)
//   ?                 configuration
//   C lo n [us]       raw capture: tune the LO to lo MHz, wait us (default
//                     1000), capture n words (<= 16384), send them as frames
//   P f1 f2 reps [us] settling curve: settle on f1, hop to f2, (wait us),
//                     one 16384-word capture, band power of each 256-word
//                     window (3.2 us) vs a reading 2 ms later
//   B                 (single byte, anywhere) reboot to the ROM loader
//
// Binary frames: 0xA5, type, len, payload[len], crc8(type, len, payload)
// (poly 0x07, init 0). Multi-byte fields little-endian.
//   'M' measurement, one per cycle while streaming:
//       u16 seq, u32 t0_us, u8 slot_mask, u8 flags (1 = gain re-forced,
//       2 = snapshot failed, 4 = frames dropped before this one), then per
//       active slot: u16 rssi (1/16 dB, 16 * (dBFS + 100)), u16 t_us (after
//       t0), u8 clipped samples
//   'S' statistics, once per second while streaming:
//       u32 cycles, u32 dropped frames, u32 snapshot failures, u32 gain
//       re-forces, then for tune, capture, meter, slot, cycle: u16 min,
//       avg, max in 0.1 us
//   'H' raw capture header: u32 n, u32 lo_mhz, u32 capture cycles, u32 cpu
//       ticks/us, u32 DUMP_PTR_MODE, u32 DUMP_CTRL, u32 gain register,
//       u32 index of the first word still holding the sentinel (n + 64 if none)
//   'W' raw capture words: u16 index, then up to 60 u32 words

#include "hop_bench.h"
#include "hop_engine.h"
#include "bench_link.h"
#include "dlboot.h"
#include "rf_min.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG32(a)        (*(volatile uint32_t *)(uintptr_t)(a))
#define RAW_CHUNK_WORDS 60
#define SETTLE_WORDS    HOP_RAW_MAX
#define SETTLE_WIN      256
#define SETTLE_NWIN     (SETTLE_WORDS / SETTLE_WIN)

static const hop_config_t DEFAULT_CFG = {
    .mhz = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},   // RaceBand R1-R8
    .gain = 60,                                                // Phase A fixed gain
    // Both 5 GHz retune steps skipped: 62 us hops, settled from the first
    // 3.2 us window on all 56 R-band pairs, level within 0.7 dB of the full
    // sequence (bench 2026-10-17, README "Phase B2 results").
    .skip = HOP_SKIP_CKGEN | HOP_SKIP_FREQMEM,
    .lo_side = -1,
    .mirrored = 1,
    .settle_us = 0,
    .samples = 256,
};

static bool s_rf_up;
static bool s_streaming;
static uint16_t s_seq;
static char s_line[128];
static unsigned s_line_len;

// Stream frames drop when the USB buffer is full; raw captures block.
#define send_frame bench_send_frame
#define put16      bench_put16
#define put32      bench_put32

// u16 in 0.1 us, saturating.
static uint32_t tenth_us(uint64_t cycles, uint32_t tpu)
{
    uint64_t v = cycles * 10u / tpu;
    return v > 65535u ? 65535u : (uint32_t)v;
}

static void send_stats(void)
{
    hop_stats_t st;
    hop_stats_get(&st, true);
    const uint32_t tpu = esp_rom_get_cpu_ticks_per_us();
    uint8_t buf[16 + 5 * 6], *p = buf;
    put32(&p, st.cycles);
    put32(&p, bench_dropped());
    put32(&p, st.timeouts);
    put32(&p, st.gain_rewrites);
    const hop_stat_t *v[5] = {&st.tune, &st.capture, &st.meter, &st.slot, &st.cycle};
    for (int i = 0; i < 5; ++i) {
        put16(&p, tenth_us(v[i]->min, tpu));
        put16(&p, tenth_us(v[i]->n ? v[i]->sum / v[i]->n : 0, tpu));
        put16(&p, tenth_us(v[i]->max, tpu));
    }
    send_frame('S', buf, (uint8_t)(p - buf), 0);
}

static void print_stats(void)
{
    hop_stats_t st;
    hop_stats_get(&st, true);
    const uint32_t tpu = esp_rom_get_cpu_ticks_per_us();
    printf("OK stats cycles=%lu fail=%lu gain_reforce=%lu dropped=%lu (us min/avg/max)\n",
           (unsigned long)st.cycles, (unsigned long)st.timeouts, (unsigned long)st.gain_rewrites,
           (unsigned long)bench_dropped());
    const char *names[5] = {"tune", "capture", "meter", "slot", "cycle"};
    const hop_stat_t *v[5] = {&st.tune, &st.capture, &st.meter, &st.slot, &st.cycle};
    for (int i = 0; i < 5; ++i) {
        uint64_t avg = v[i]->n ? v[i]->sum / v[i]->n : 0;
        printf("  %-8s %7.1f %7.1f %7.1f\n", names[i], (double)v[i]->min / tpu, (double)avg / tpu,
               (double)v[i]->max / tpu);
    }
}

static void print_config(void)
{
    const hop_config_t *c = hop_config();
    printf("OK cfg slots=");
    for (int s = 0; s < HOP_MAX_SLOTS; ++s) printf("%s%u", s ? "," : "", c->mhz[s]);
    printf(" gain=%u skip=%u lo_side=%d mirrored=%u settle_us=%u samples=%u stream=%d rf=%d\n",
           c->gain, c->skip, c->lo_side, c->mirrored, c->settle_us, c->samples, s_streaming, s_rf_up);
}

static void send_measurement(const hop_reading_t *r, unsigned n, uint8_t mask, uint32_t t0, bool reforced,
                             bool failed)
{
    uint8_t buf[8 + HOP_MAX_SLOTS * 5], *p = buf;
    put16(&p, s_seq++);
    put32(&p, t0);
    *p++ = mask;
    *p++ = (uint8_t)((reforced ? 1 : 0) | (failed ? 2 : 0) | (bench_take_drop_flag() ? 4 : 0));
    for (unsigned i = 0; i < n; ++i) {
        put16(&p, r[i].rssi);
        put16(&p, r[i].t_us);
        *p++ = r[i].clip;
    }
    send_frame('M', buf, (uint8_t)(p - buf), 0);
}

static void raw_capture(uint32_t lo, uint32_t n, uint32_t settle_us)
{
    if (n < 8 || n > HOP_RAW_MAX) { printf("ERR n must be 8-%u\n", HOP_RAW_MAX); return; }
    volatile uint32_t *ram = hop_dump_ram();
    const uint32_t fill = n + 64 > HOP_RAW_MAX ? HOP_RAW_MAX : n + 64;
    (void)hop_gain_check();
    hop_tune(lo);
    esp_rom_delay_us(settle_us);
    for (uint32_t i = 0; i < fill; ++i) ram[i] = 0xffffffffu;
    uint32_t cyc = 0;
    bool ok = hop_capture(n, &cyc);
    uint32_t first_sentinel = n + 64;
    for (uint32_t i = 0; i < fill; ++i)
        if (ram[i] == 0xffffffffu) { first_sentinel = i; break; }
    uint8_t hdr[32], *p = hdr;
    put32(&p, n);
    put32(&p, lo);
    put32(&p, cyc);
    put32(&p, esp_rom_get_cpu_ticks_per_us());
    put32(&p, REG32(0x600a9008u));
    put32(&p, REG32(0x600a9004u));
    put32(&p, REG32(0x600a702cu));
    put32(&p, first_sentinel);
    printf("OK raw lo=%lu n=%lu ok=%d\n", (unsigned long)lo, (unsigned long)n, ok);
    if (!send_frame('H', hdr, sizeof hdr, pdMS_TO_TICKS(1000))) return;
    for (uint32_t i = 0; i < n; i += RAW_CHUNK_WORDS) {
        uint32_t k = n - i < RAW_CHUNK_WORDS ? n - i : RAW_CHUNK_WORDS;
        uint8_t buf[2 + RAW_CHUNK_WORDS * 4], *q = buf;
        put16(&q, i);
        for (uint32_t j = 0; j < k; ++j) put32(&q, ram[i + j]);
        if (!send_frame('W', buf, (uint8_t)(q - buf), pdMS_TO_TICKS(1000))) return;
    }
}

static void settle_curve(uint32_t f1, uint32_t f2, uint32_t reps, uint32_t delay_us)
{
    static int32_t sum[SETTLE_NWIN];
    memset(sum, 0, sizeof sum);
    int32_t ref_sum = 0;
    uint32_t ok_reps = 0, clip_max = 0;
    const uint32_t lo1 = hop_lo_mhz((uint16_t)f1), lo2 = hop_lo_mhz((uint16_t)f2);
    const uint32_t tpu = esp_rom_get_cpu_ticks_per_us();
    uint32_t lat_sum = 0;
    for (uint32_t r = 0; r < reps; ++r) {
        (void)hop_gain_check();
        hop_tune(lo1);
        esp_rom_delay_us(1000);
        const uint32_t t0 = esp_cpu_get_cycle_count();
        hop_tune(lo2);
        if (delay_us) esp_rom_delay_us(delay_us);
        const uint32_t t1 = esp_cpu_get_cycle_count();
        if (!hop_capture(SETTLE_WORDS, NULL)) continue;
        lat_sum += (t1 - t0) / tpu;
        for (unsigned w = 0; w < SETTLE_NWIN; ++w) {
            uint8_t clip;
            sum[w] += hop_measure(w * SETTLE_WIN, SETTLE_WIN, &clip);
            if (clip > clip_max) clip_max = clip;
        }
        esp_rom_delay_us(2000);
        if (!hop_capture(SETTLE_WIN, NULL)) continue;
        ref_sum += hop_measure(0, SETTLE_WIN, NULL);
        ok_reps++;
        if ((r & 15) == 15) vTaskDelay(1);
    }
    if (!ok_reps) { printf("ERR settle: no good captures\n"); return; }
    // Window w spans [w*3.2, (w+1)*3.2) us after the snapshot started, which
    // is hop_us (tune + delay) after the hop began.
    printf("OK settle f1=%lu f2=%lu reps=%lu hop_us=%lu ref=%.2f clip_max=%lu dB:",
           (unsigned long)f1, (unsigned long)f2, (unsigned long)ok_reps, (unsigned long)(lat_sum / ok_reps),
           (double)ref_sum / ok_reps / 16.0 - 100.0, (unsigned long)clip_max);
    for (unsigned w = 0; w < SETTLE_NWIN; ++w)
        printf(" %.2f", (double)(sum[w] - ref_sum) / ok_reps / 16.0);
    printf("\n");
}

// Phase B3 question: while the continuous MODEM_DIAG writer runs (as video
// needs), can the CPU read its 10-bit ring in the dump bank, and which
// register tracks the write position? Prints the dump-engine registers that
// move, whether the bank changes under the CPU, and a pointer trace.
// Answer (2026-10-17): DUMP_PTR_MODE bits 0-13 are the write position (a
// 16384-word ring at 80 MS/s), but with the continuous setup's owner field
// (0) nothing lands in the bank, and with owner 2 a CPU read of the bank is
// a load access fault (panic, USB dead until replug). So RX5808-mode RSSI
// cannot read the writer's 10-bit ring; only owner 0 is allowed here.
#define PROBE_REGS 64
static void writer_probe(uint32_t lo, uint32_t owner)
{
    static uint32_t r1[PROBE_REGS], r2[PROBE_REGS], trace_v[8][32], trace_t[32];
    volatile uint32_t *ram = hop_dump_ram();
    const uint32_t tpu = esp_rom_get_cpu_ticks_per_us();
    s_streaming = false;
    hop_tune(lo);
    for (uint32_t i = 0; i < HOP_RAW_MAX; ++i) ram[i] = 0xffffffffu;
    rf_min_writer_start();
    // Bank owner field (bits 8-11): the continuous setup leaves 0; one-shot
    // snapshots use 2.
    REG32(0x60095004u) = (REG32(0x60095004u) & ~0x00000f00u) | ((owner & 0xfu) << 8);
    esp_rom_delay_us(2000);
    const uint32_t sram_usage = REG32(0x60095004u);

    for (int k = 0; k < PROBE_REGS; ++k) r1[k] = REG32(0x600a9000u + 4u * k);
    esp_rom_delay_us(13);
    for (int k = 0; k < PROBE_REGS; ++k) r2[k] = REG32(0x600a9000u + 4u * k);
    int moving[8], nm = 0;
    for (int k = 0; k < PROBE_REGS && nm < 8; ++k)
        if (r1[k] != r2[k]) moving[nm++] = k;
    for (int t = 0; t < 32; ++t) {
        trace_t[t] = esp_cpu_get_cycle_count();
        for (int m = 0; m < nm; ++m) trace_v[m][t] = REG32(0x600a9000u + 4u * moving[m]);
        esp_rom_delay_us(3);
    }

    // The bank under the CPU: sentinels left, real-looking words, changes.
    uint32_t sentinel = 0, gain_ok = 0, changed = 0;
    const uint32_t gain = hop_config()->gain;
    static uint32_t snap[1024];
    for (uint32_t i = 0; i < 1024; ++i) snap[i] = ram[i * 16];
    esp_rom_delay_us(50);
    for (uint32_t i = 0; i < HOP_RAW_MAX; ++i) {
        uint32_t w = ram[i];
        sentinel += w == 0xffffffffu;
        gain_ok += ((w >> 20) & 0x7f) == gain;
    }
    for (uint32_t i = 0; i < 1024; ++i) changed += ram[i * 16] != snap[i];
    const uint32_t ram_word0 = ram[0], ram_word_mid = ram[8000];
    rf_min_writer_stop();
    hop_capture_setup();

    printf("OK probe lo=%lu HP_SRAM_USAGE=0x%08lx bank: sentinels %lu/%u, gain==%lu %lu, "
           "changed in 50us %lu/1024, w[0]=0x%08lx w[8000]=0x%08lx\n",
           (unsigned long)lo, (unsigned long)sram_usage, (unsigned long)sentinel, HOP_RAW_MAX,
           (unsigned long)gain, (unsigned long)gain_ok, (unsigned long)changed,
           (unsigned long)ram_word0, (unsigned long)ram_word_mid);
    for (int m = 0; m < nm; ++m) {
        printf("  reg 0x%08lx moves: 0x%08lx -> 0x%08lx; trace (us, value):",
               (unsigned long)(0x600a9000u + 4u * moving[m]), (unsigned long)r1[moving[m]],
               (unsigned long)r2[moving[m]]);
        for (int t = 0; t < 32; t += 4)
            printf(" %.1f:0x%lx", (double)(trace_t[t] - trace_t[0]) / tpu, (unsigned long)trace_v[m][t]);
        printf("\n");
    }
    if (!nm) printf("  no register in 0x600a9000-0x600a90fc moved\n");
}

static void handle_line(char *line)
{
    char *p = line;
    while (*p == ' ') ++p;
    if (!*p) return;
    const char cmd = *p++;
    long a[HOP_MAX_SLOTS];
    int na = 0;
    while (na < HOP_MAX_SLOTS) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        a[na++] = v;
        p = end;
    }
    hop_config_t c = *hop_config();
    switch (cmd) {
    case 'F':
        if (na < 1) { printf("ERR F needs 1-8 MHz values\n"); return; }
        for (int s = 0; s < HOP_MAX_SLOTS; ++s) {
            long v = s < na ? a[s] : 0;
            if (v && (v < 5000 || v > 6000)) { printf("ERR %ld MHz out of range\n", v); return; }
            c.mhz[s] = (uint16_t)v;
        }
        break;
    case 'G': if (na != 1 || a[0] < 0 || a[0] > 127) { printf("ERR G 0-127\n"); return; } c.gain = (uint8_t)a[0]; break;
    case 'K': if (na != 1 || a[0] < 0 || a[0] > 3) { printf("ERR K 0-3\n"); return; } c.skip = (uint8_t)a[0]; break;
    case 'L': if (na != 1 || (a[0] != 1 && a[0] != -1)) { printf("ERR L -1|1\n"); return; } c.lo_side = (int8_t)a[0]; break;
    case 'M': if (na != 1 || a[0] < 0 || a[0] > 1) { printf("ERR M 0|1\n"); return; } c.mirrored = (uint8_t)a[0]; break;
    case 'D': if (na != 1 || a[0] < 0 || a[0] > 5000) { printf("ERR D 0-5000\n"); return; } c.settle_us = (uint16_t)a[0]; break;
    case 'N':
        if (na != 1 || a[0] < HOP_MIN_SAMPLES || a[0] > HOP_MAX_SAMPLES || (a[0] & 7)) {
            printf("ERR N %d-%d, multiple of 8\n", HOP_MIN_SAMPLES, HOP_MAX_SAMPLES);
            return;
        }
        c.samples = (uint16_t)a[0];
        break;
    case 'R': s_streaming = true; printf("OK stream on\n"); return;
    case 'X': s_streaming = false; printf("OK stream off\n"); return;
    case 'T': print_stats(); return;
    case '?': print_config(); return;
    case 'C':
        if (!s_rf_up) { printf("ERR radio not running\n"); return; }
        if (na < 2) { printf("ERR C lo_mhz n [settle_us]\n"); return; }
        raw_capture((uint32_t)a[0], (uint32_t)a[1], na > 2 ? (uint32_t)a[2] : 1000u);
        return;
    case 'P':
        if (!s_rf_up) { printf("ERR radio not running\n"); return; }
        if (na < 3 || a[2] < 1) { printf("ERR P f1 f2 reps [delay_us]\n"); return; }
        settle_curve((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], na > 3 ? (uint32_t)a[3] : 0u);
        return;
    case 'Q':
        if (!s_rf_up) { printf("ERR radio not running\n"); return; }
        if (na > 1 && a[1] != 0) { printf("ERR owner != 0 faults the CPU (see writer_probe)\n"); return; }
        writer_probe(na > 0 ? (uint32_t)a[0] : hop_lo_mhz(5732), 0u);
        return;
    default: printf("ERR unknown command '%c'\n", cmd); return;
    }
    hop_set_config(&c);
    print_config();
}

static void poll_console(void)
{
    uint8_t buf[64];
    int n = usb_serial_jtag_read_bytes(buf, sizeof buf, 0);
    for (int i = 0; i < n; ++i) {
        const char ch = (char)buf[i];
        if (ch == DLBOOT_KEY) rf_min_reboot_to_download();
        if (ch == '\r' || ch == '\n') {
            s_line[s_line_len] = 0;
            handle_line(s_line);
            s_line_len = 0;
        } else if (s_line_len < sizeof s_line - 1) {
            s_line[s_line_len++] = ch;
        }
    }
}

static void bench_task(void *arg)
{
    (void)arg;
    hop_reading_t r[HOP_MAX_SLOTS];
    int64_t next_stats = esp_timer_get_time() + 1000000;
    hop_stats_t st;
    for (;;) {
        poll_console();
        if (!s_rf_up) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        hop_stats_get(&st, false);
        const uint32_t rew0 = st.gain_rewrites, fail0 = st.timeouts;
        uint8_t mask;
        uint32_t t0;
        unsigned n = hop_cycle(r, &mask, &t0);
        if (!n) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (s_streaming) {
            hop_stats_get(&st, false);
            send_measurement(r, n, mask, t0, st.gain_rewrites != rew0, st.timeouts != fail0);
            if (esp_timer_get_time() >= next_stats) {
                send_stats();
                next_stats = esp_timer_get_time() + 1000000;
            }
        }
    }
}

void hop_bench_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.tx_buffer_size = 16384;
    usb.rx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&usb) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    dlboot_start_usb_guard();
    setvbuf(stdout, NULL, _IONBF, 0);

    // The engine task never blocks while slots are active, so the idle task
    // starves; take it off the task watchdog.
#if CONFIG_ESP_TASK_WDT_INIT
    const esp_task_wdt_config_t twdt = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
#ifdef CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#endif
    };
    (void)esp_task_wdt_reconfigure(&twdt);
#endif

    printf("ESP32_SDR_RX5808 hop bench (Phase B2). '?' = config, R = stream, T = timing\n");
    esp_err_t err = hop_init(&DEFAULT_CFG);
    s_rf_up = err == ESP_OK;
    if (s_rf_up) print_config();
    else printf("ERR radio start failed: %s\n", esp_err_to_name(err));

    // Priority 1: everything else (Wi-Fi, USB guard) preempts the engine.
    xTaskCreate(bench_task, "hopbench", 8192, NULL, 1, NULL);
    for (;;) vTaskDelay(portMAX_DELAY);
}
