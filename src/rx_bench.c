// SPDX-License-Identifier: GPL-3.0-only
//
// Bench firmware for the receiver (src/receiver.c): RX5808 mode and
// multi-channel mode on 10-bit snapshots, the RX5808 bus with the EXT
// register and fast readout, PWM RSSI on D0, a USB console and binary
// stream. Built by `.\tools\pio.ps1 run -e rx`; host side: tools/phase_b
// (rx, rx-tune, stream) and tools/spi_host (bus tests).
//
// Console commands (ASCII lines; replies "OK ..." / "ERR ..."). The
// receiver settings go through the register file exactly as a bus host's
// frames do:
//   F mhz      tune (register 0x01; the RX5808 2 MHz step applies)
//   G g        gain (EXT GAIN; 0 = default 60)
//   Y m        mode (EXT MODE): 0 RX5808, 1 multi-channel
//   S n mhz    slot n (1-8) frequency (EXT SLOTn; 0 = off)
//   N n        RX5808 mode: snapshots per reading (1-32)
//   P lo hi    PWM range in dBFS (duty 0 at lo, full at hi)
//   O duty     hold the PWM at duty 0-1023 (RC filter check); -1 = follow RSSI
//   R / X      start / stop the stream
//   T          statistics (resets them)
//   L          RX5808 bus: counters and the last frames received
//   Z mask     bus glitch filters (1 SEL, 2 CLK, 4 DATA)
//   U 0|1      bus pull-ups (wiring check)
//   I          bus pin levels (wiring check)
//   ?          configuration
//   B          (single byte, anywhere) reboot to the ROM loader
//
// Frames (bench_link.h framing), one per step while streaming:
//   'Q' (RX5808 mode): u16 seq, u32 t_us (end of the reading), u16 mhz,
//       u8 gain, u16 rssi (1/16 dB, 16 * (dBFS + 100), after smoothing),
//       u16 min, u16 max (snapshot spread), u8 clipped samples, u8 snapshots,
//       u8 flags (1 = a snapshot failed, 2 = gain re-forced, 4 = frames
//       dropped before this one, 8 = first reading after a retune), u32 us
//       since the last retune
//   'M' (multi-channel mode, as src/hop_bench.c): u16 seq, u32 t0_us,
//       u8 slot mask, u8 flags (4 = frames dropped before), per slot in the
//       mask: u16 rssi, u16 t_us after t0, u8 clipped samples

#include "rx_bench.h"
#include "bench_link.h"
#include "dlboot.h"
#include "hop_engine.h"
#include "receiver.h"
#include "regs.h"
#include "rf_min.h"
#include "rssi_out.h"
#include "rx5808_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_up, s_streaming;
static uint16_t s_seq;
static char s_line[96];
static unsigned s_line_len;

typedef struct {
    uint32_t steps, readings, fails, reforced, gap_max_us, cpu_max;
    uint64_t cpu_sum;
    int64_t last_t;
    uint16_t last_rssi;
} rx_stats_t;
static rx_stats_t s_st;

static void ext(uint32_t sub, uint32_t value) { regs_write(REG_EXT, (sub << 16) | (value & 0xffffu)); }

static void print_config(void)
{
    ext_state_t e;
    regs_ext(&e);
    printf("OK cfg mode=%u mhz=%u reg01_mhz=%lu gain=%u per_reading=%u slots=", receiver_mode(), receiver_mhz(),
           (unsigned long)regs_mhz(), e.gain ? e.gain : RX_DEFAULT_GAIN, receiver_per_reading());
    for (int s = 0; s < REGS_SLOTS; ++s) printf("%s%u", s ? "," : "", e.slot_mhz[s]);
    printf(" pin_slot=%u smoothing=%u stream=%d rf=%d powered_down=%d\n", e.pin_slot, e.options & OPT_SMOOTHING,
           s_streaming, s_up, receiver_powered_down());
}

static void print_bus(void)
{
    bus_stats_t st;
    bus_stats(&st, false);
    printf("OK bus frames=%lu writes=%lu reads=%lu short=%lu floating=%lu overruns=%lu requests=%lu readouts=%lu "
           "reg01=0x%05lx -> %lu MHz power=0x%05lx\n",
           (unsigned long)st.frames, (unsigned long)st.writes, (unsigned long)st.reads,
           (unsigned long)st.short_frames, (unsigned long)st.floating, (unsigned long)st.overruns,
           (unsigned long)st.requests,
           (unsigned long)st.readouts, (unsigned long)regs_read(REG_SYNTH_B), (unsigned long)regs_mhz(),
           (unsigned long)regs_read(REG_POWER));
    bus_frame_t f[16];
    unsigned n = bus_log(f, 16);
    for (unsigned i = 0; i < n; ++i)
        printf("  t=%lu bits=%lu %s addr=0x%lx data=0x%05lx\n", (unsigned long)f[i].t_us, (unsigned long)f[i].bits,
               f[i].write ? "W" : "R", (unsigned long)f[i].addr, (unsigned long)f[i].data);
}

static void print_stats(void)
{
    const uint32_t tpu = esp_rom_get_cpu_ticks_per_us();
    printf("OK stats mode=%u steps=%lu readings=%lu fails=%lu gain_reforced=%lu dropped=%lu max_gap_us=%lu "
           "cpu_us/step avg=%.1f max=%.1f last=%.2f dBFS duty=%lu rate=%lu/s/slot switches=%lu last_switch_us=%lu\n",
           receiver_mode(), (unsigned long)s_st.steps, (unsigned long)s_st.readings, (unsigned long)s_st.fails,
           (unsigned long)s_st.reforced, (unsigned long)bench_dropped(), (unsigned long)s_st.gap_max_us,
           s_st.steps ? (double)s_st.cpu_sum / s_st.steps / tpu : 0.0, (double)s_st.cpu_max / tpu,
           s_st.last_rssi / 16.0 - 100.0, (unsigned long)rssi_out_duty(), (unsigned long)receiver_rate(),
           (unsigned long)receiver_switches(), (unsigned long)receiver_last_switch_us());
    memset(&s_st, 0, sizeof s_st);
}

static void handle_line(char *line)
{
    char *p = line;
    while (*p == ' ') ++p;
    if (!*p) return;
    const char cmd = *p++;
    long a[2];
    int na = 0;
    while (na < 2) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        a[na++] = v;
        p = end;
    }
    switch (cmd) {
    case 'F':
        if (na != 1 || a[0] < 5000 || a[0] > 6000) { printf("ERR F 5000-6000\n"); return; }
        regs_write(REG_SYNTH_B, regs_encode_mhz((uint32_t)a[0]));
        printf("OK tune reg01=0x%05lx -> %lu MHz\n", (unsigned long)regs_read(REG_SYNTH_B), (unsigned long)regs_mhz());
        return;
    case 'G':
        if (na != 1 || a[0] < 0 || a[0] > 83) { printf("ERR G 0-83 (0 = default)\n"); return; }
        ext(EXT_GAIN, (uint32_t)a[0]);
        break;
    case 'Y':
        if (na != 1 || a[0] < 0 || a[0] > 1) { printf("ERR Y 0|1\n"); return; }
        ext(EXT_MODE, (uint32_t)a[0]);
        break;
    case 'S':
        if (na != 2 || a[0] < 1 || a[0] > 8 || (a[1] && (a[1] < REGS_MIN_MHZ || a[1] > REGS_MAX_MHZ))) {
            printf("ERR S slot(1-8) mhz(0 or %u-%u)\n", REGS_MIN_MHZ, REGS_MAX_MHZ);
            return;
        }
        ext(EXT_SLOT1 + (uint32_t)a[0] - 1, (uint32_t)a[1]);
        break;
    case 'N':
        if (na != 1 || a[0] < 1 || a[0] > 32) { printf("ERR N 1-32\n"); return; }
        receiver_set_per_reading((unsigned)a[0]);
        break;
    case 'P':
        if (na != 2 || a[1] <= a[0]) { printf("ERR P lo_dbfs hi_dbfs\n"); return; }
        rssi_out_range((int)a[0], (int)a[1]);
        printf("OK pwm lo=%ld hi=%ld dBFS\n", a[0], a[1]);
        return;
    case 'O':
        // Bench: hold the RSSI PWM at a fixed duty (checks the RC filter).
        if (na != 1 || a[0] < -1 || a[0] > 1023) { printf("ERR O duty 0-1023, -1 = follow RSSI\n"); return; }
        rssi_out_force((int32_t)a[0]);
        printf("OK pwm duty %s %ld\n", a[0] < 0 ? "follows RSSI" : "forced", a[0]);
        return;
    case 'R': s_streaming = true; printf("OK stream on\n"); return;
    case 'X': s_streaming = false; printf("OK stream off\n"); return;
    case 'T': print_stats(); return;
    case 'L': print_bus(); return;
    case 'Z':
        if (na != 1 || a[0] < 0 || a[0] > 7) { printf("ERR Z 0-7 (1 SEL, 2 CLK, 4 DATA)\n"); return; }
        printf("OK filters %u\n", bus_filters((unsigned)a[0]));
        return;
    case 'U':
        // Wiring check: internal pull-ups on (1) or off (0) for all three
        // bus pins. A pin that reads 1 while the host drives it low is open.
        if (na != 1) { printf("ERR U 0|1\n"); return; }
        for (int pin = 0; pin < 3; ++pin) {
            const int g = pin == 0 ? BUS_PIN_DATA : pin == 1 ? BUS_PIN_SEL : BUS_PIN_CLK;
            if (a[0]) gpio_pullup_en(g); else gpio_pullup_dis(g);
        }
        printf("OK pullups %ld\n", a[0]);
        return;
    case 'I':
        // Wiring check: the bus pins' input levels.
        printf("OK pins data(D10)=%d sel(D8)=%d clk(D1)=%d\n", gpio_get_level(BUS_PIN_DATA),
               gpio_get_level(BUS_PIN_SEL), gpio_get_level(BUS_PIN_CLK));
        return;
    case '?': break;
    default: printf("ERR unknown command '%c'\n", cmd); return;
    }
    print_config();
}

static void poll_console(void)
{
    uint8_t buf[32];
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

static void stream(const rx_result_t *r)
{
    if (r->mode == MODE_RX5808) {
        if (!r->n) return;
        uint8_t buf[22], *q = buf;
        bench_put16(&q, s_seq++);
        bench_put32(&q, r->t0_us + r->r[0].t_us);
        bench_put16(&q, r->mhz);
        *q++ = hop_config()->gain;
        bench_put16(&q, r->r[0].rssi);
        bench_put16(&q, r->r[0].mn);
        bench_put16(&q, r->r[0].mx);
        *q++ = r->r[0].clip;
        *q++ = r->snaps;
        *q++ = (uint8_t)((r->failed ? 1 : 0) | (r->reforced ? 2 : 0) | (bench_take_drop_flag() ? 4 : 0) |
                         (r->retuned ? 8 : 0));
        bench_put32(&q, r->since_tune_us);
        bench_send_frame('Q', buf, (uint8_t)(q - buf), 0);
    } else {
        uint8_t buf[8 + REGS_SLOTS * 5], *q = buf;
        uint8_t mask = 0;
        for (unsigned i = 0; i < r->n; ++i) mask |= 1u << r->r[i].slot;
        bench_put16(&q, s_seq++);
        bench_put32(&q, r->t0_us);
        *q++ = mask;
        *q++ = (uint8_t)(bench_take_drop_flag() ? 4 : 0);
        for (unsigned i = 0; i < r->n; ++i) {
            bench_put16(&q, r->r[i].rssi);
            bench_put16(&q, r->r[i].t_us);
            *q++ = r->r[i].clip;
        }
        bench_send_frame('M', buf, (uint8_t)(q - buf), 0);
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    int64_t next_console = 0;
    rx_result_t r;
    for (;;) {
        // Console once per ms: polling the USB driver in a tight loop left
        // the USB dead (2026-10-17).
        const int64_t now = esp_timer_get_time();
        if (now >= next_console) {
            poll_console();
            next_console = now + 1000;
        }
        if (!s_up) { vTaskDelay(1); continue; }
        const uint32_t c0 = esp_cpu_get_cycle_count();
        if (!receiver_step(&r)) { vTaskDelay(1); continue; }
        const uint32_t dc = esp_cpu_get_cycle_count() - c0;
        const int64_t t = esp_timer_get_time();
        s_st.steps++;
        s_st.readings += r.n;
        s_st.fails += r.failed;
        s_st.reforced += r.reforced;
        s_st.cpu_sum += dc;
        if (dc > s_st.cpu_max) s_st.cpu_max = dc;
        if (s_st.last_t && t - s_st.last_t > s_st.gap_max_us) s_st.gap_max_us = (uint32_t)(t - s_st.last_t);
        s_st.last_t = t;
        if (r.n) s_st.last_rssi = r.r[0].rssi;
        if (s_streaming) stream(&r);
    }
}

void rx_bench_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.tx_buffer_size = 16384;
    usb.rx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&usb) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    dlboot_start_usb_guard();
    setvbuf(stdout, NULL, _IONBF, 0);
#if CONFIG_ESP_TASK_WDT_INIT
    // The RX task never blocks, so the idle task starves.
    const esp_task_wdt_config_t twdt = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
#ifdef CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#endif
    };
    (void)esp_task_wdt_reconfigure(&twdt);
#endif

    printf("ESP32_SDR_RX5808 receiver bench (C2). '?' = config, R = stream, T = stats, L = bus\n");
    // The bus first (datasheet 11: frames accepted within ~150 ms of power-on;
    // hosts tune once at their own boot). Frames that arrive while the radio
    // starts are kept in the register file and applied once it is up.
    esp_err_t bus_err = bus_init();
    printf(bus_err == ESP_OK ? "OK bus up at %lld ms\n" : "ERR bus init failed at %lld ms\n",
           esp_timer_get_time() / 1000);
    const hop_config_t hc = {.gain = RX_DEFAULT_GAIN, .skip = HOP_SKIP_CKGEN | HOP_SKIP_FREQMEM,
                             .lo_side = -1, .mirrored = 1, .samples = 256};
    esp_err_t err = hop_init(&hc);
    if (err == ESP_OK) err = rssi_out_init();
    if (err == ESP_OK) err = receiver_init();
    s_up = err == ESP_OK;
    if (bus_err == ESP_OK) printf("OK bus filters %u\n", bus_filters(7u));
    if (s_up) print_config();
    else printf("ERR start failed: %s\n", esp_err_to_name(err));
    // Priority 1: Wi-Fi and the USB guard preempt it.
    xTaskCreate(rx_task, "rxbench", 6144, NULL, 1, NULL);
    for (;;) vTaskDelay(portMAX_DELAY);
}
