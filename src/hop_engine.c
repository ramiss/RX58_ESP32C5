// SPDX-License-Identifier: GPL-3.0-only
//
// Multi-channel hop engine (Phase B2).
//
// Visits up to 8 channels at ~1 kHz each: retune the RF PLL without
// libphy's fixed 300 us wait, take a short RF-dump snapshot with the LO
// offset 10 MHz from the pilot, and measure band power with a periodic
// window x mixer table. The raw I/Q capture path was first published by
// esp-sdr (github.com/ESPARGOS/esp-sdr); the C5 RF bring-up follows C5VRX
// (see src/rf_min.c).
//
// - Retune: the sequence is read from Espressif's own ESP-IDF 6.0.2 C5
//   libphy.a (phy_set_rf_freq_offset -> phy_set_rfpll_freq), minus the
//   ets_delay_us(300) that ends phy_set_rfpll_freq. Arguments are the ones
//   phy_chip_set_chan passes: phy_param[49] (crystal select) and the signed
//   frequency correction at phy_param[30].
// - Snapshot: the register sequence of librftest.a's adctrig() in its
//   software-trigger mode (source 0), reduced to the per-capture part.
// - Meter: our own design, see hop_measure().
//
// All register addresses and private symbols are for ESP-IDF 6.0.2 with
// its pinned libphy (see platformio.ini); re-verify after any IDF change.

#include "hop_engine.h"
#include "rf_min.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG32(a)         (*(volatile uint32_t *)(uintptr_t)(a))
#define FENCE()          __asm__ __volatile__("fence iorw, iorw" ::: "memory")

// RF-dump engine (librftest adctrig; also src/rf_min.c).
#define HP_SRAM_USAGE    0x60095004u
#define SRAM_OWNER_MASK  0x00010f00u
#define SRAM_OWNER_DUMP  0x00010200u   // bank owner field = 2, bit 16 = modem access
#define DUMP_CTRL        0x600a9004u
#define CTRL_COUNT_MASK  0x0001ffffu
#define CTRL_DUMP_FIRST  0x00020000u   // pre-trigger circular mode (off here)
#define CTRL_DONE        0x00040000u
#define CTRL_TRIGGER     0x00080000u
#define CTRL_MODE_BITS   0x00300000u
#define CTRL_ENABLE      0x80000000u
#define DUMP_PTR_MODE    0x600a9008u
#define PTR_TRIG_SEL     0x001e0000u   // bits 17-20: trigger source, 0xF = software
#define PTR_DECIM        0x00e00000u   // bits 21-23
#define DUMP_FORMAT      0x600a9018u
#define FE_PATH          0x600a20b4u
#define FE_ENABLE        0x600a0800u
#define MODEM_CLOCK      0x600a9c04u
#define DUMP_RAM         ((volatile uint32_t *)0x40830000u)
#define SENTINEL         0xffffffffu   // gain field 127: never a real sample

// Forced RX gain (phy_force_rx_gain): index in bits 24-31, force in bit 23.
#define RX_GAIN_FORCE    0x600a702cu
#define GAIN_FORCE_BIT   0x00800000u

// phy_param fields, from the libphy 6.0.2 disassembly.
#define PP_FREQ_CORR     30            // int16: frequency correction
#define PP_IS_5G         42            // uint8
#define PP_XTAL_SEL      49            // uint8
#define PP_PLL_MHZ       714           // uint16: written by phy_set_rfpll_freq

extern uint8_t phy_param[];
extern void phy_rfpll_set_freq(uint32_t mhz, uint32_t xtal_sel, int32_t corr, uint8_t sdm[4]);
extern void phy_write_rfpll_sdm(uint8_t sdm[4]);
extern void phy_set_freq_i2c_new(uint32_t mhz);
extern void phy_restart_cal(void);
extern void phy_i2c_sdm_en(uint32_t is_5g);
extern void phy_ckgen_5g_cal(uint32_t mhz);
extern void phy_freq_mem_change_5g_(uint8_t sdm[4]);
extern void phy_force_rx_gain(bool enable, uint8_t gain_idx);

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static hop_config_t s_cfg;
static hop_stats_t s_stats;

// Meter: 8-tap periodic Hann window times a mixer at +-fs/8, Q14.
static int16_t s_cr[8], s_ci[8];
// round(256 * log2(1 + i / 256))
static uint8_t s_log2_frac[256];

static inline uint32_t ccount(void) { return esp_cpu_get_cycle_count(); }

static void stat_add(hop_stat_t *s, uint32_t v)
{
    if (s->n == 0 || v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->sum += v;
    s->n++;
}

// The pilot sits at +-10 MHz in the capture. Which bin it lands in depends
// on the LO side and on the C5's spectral mirroring.
static void build_meter(void)
{
    int bin = s_cfg.lo_side < 0 ? 1 : -1;      // LO below the pilot: pilot at +fs/8
    if (s_cfg.mirrored) bin = -bin;
    for (int j = 0; j < 8; ++j) {
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * j / 8.0);
        double ph = -2.0 * M_PI * bin * j / 8.0;
        s_cr[j] = (int16_t)lround(16384.0 * w * cos(ph));
        s_ci[j] = (int16_t)lround(16384.0 * w * sin(ph));
    }
}

void hop_capture_setup(void)
{
    REG32(DUMP_CTRL) &= ~CTRL_ENABLE;
    FENCE();
    REG32(MODEM_CLOCK) = UINT32_MAX;
    REG32(FE_ENABLE) |= 4u;
    REG32(FE_PATH) &= ~1u;
    // Sample format fields as adctrig (and C5VRX) program them.
    uint32_t v = REG32(DUMP_FORMAT);
    v = (v & 0xff03ffffu) | 0x006c0000u;
    v = (v & 0xfffc0fffu) | 0x0001a000u;
    v = (v & 0xfffff03fu) | 0x00000640u;
    v = (v & 0xffffffc0u) | 0x18u;
    REG32(DUMP_FORMAT) = v | 0x01000000u;
    REG32(DUMP_PTR_MODE) = (REG32(DUMP_PTR_MODE) & ~(PTR_DECIM | PTR_TRIG_SEL)) | PTR_TRIG_SEL;
    REG32(DUMP_CTRL) &= ~(CTRL_DUMP_FIRST | CTRL_MODE_BITS | CTRL_TRIGGER);
    FENCE();
}

esp_err_t hop_init(const hop_config_t *cfg)
{
    for (int i = 0; i < 256; ++i)
        s_log2_frac[i] = (uint8_t)lround(256.0 * log2(1.0 + i / 256.0));
    hop_set_config(cfg);
    esp_err_t err = rf_min_start_front(s_cfg.gain);
    if (err != ESP_OK) return err;
    hop_capture_setup();
    return ESP_OK;
}

void hop_set_config(const hop_config_t *cfg)
{
    s_cfg = *cfg;
    if (s_cfg.samples < HOP_MIN_SAMPLES) s_cfg.samples = HOP_MIN_SAMPLES;
    if (s_cfg.samples > HOP_MAX_SAMPLES) s_cfg.samples = HOP_MAX_SAMPLES;
    s_cfg.samples &= ~7u;
    build_meter();
}

const hop_config_t *hop_config(void) { return &s_cfg; }

uint32_t hop_lo_mhz(uint16_t pilot_mhz)
{
    return s_cfg.lo_side < 0 ? pilot_mhz - HOP_IF_MHZ : pilot_mhz + HOP_IF_MHZ;
}

// phy_set_rfpll_freq without its trailing ets_delay_us(300), then the two
// 5 GHz steps of phy_set_rf_freq_offset unless skipped.
// Only the scheduler is held (no other task may touch the PHY's I2C
// mid-sequence); interrupts stay on. Masking them for this ~60 us delayed
// the RX5808 bus's SPI interrupt past a host's 50 us request-to-readout
// gap, so readouts came back empty and the next frame was lost
// (2026-10-18).
void hop_tune(uint32_t lo_mhz)
{
    uint8_t sdm[4];
    int16_t corr;
    uint16_t m = (uint16_t)lo_mhz;
    memcpy(&corr, &phy_param[PP_FREQ_CORR], sizeof corr);
    vTaskSuspendAll();
    memcpy(&phy_param[PP_PLL_MHZ], &m, sizeof m);
    phy_rfpll_set_freq(lo_mhz, phy_param[PP_XTAL_SEL], corr, sdm);
    phy_write_rfpll_sdm(sdm);
    phy_set_freq_i2c_new(lo_mhz);
    phy_restart_cal();
    phy_i2c_sdm_en(phy_param[PP_IS_5G]);
    if (phy_param[PP_IS_5G] && !(s_cfg.skip & HOP_SKIP_CKGEN)) phy_ckgen_5g_cal(lo_mhz);
    if (lo_mhz > 2482 && !(s_cfg.skip & HOP_SKIP_FREQMEM)) phy_freq_mem_change_5g_(sdm);
    (void)xTaskResumeAll();
}

bool hop_capture(uint32_t n, uint32_t *cycles)
{
    if (n < 8 || n > HOP_RAW_MAX) return false;
    const uint32_t tpu = esp_rom_get_cpu_ticks_per_us();
    const uint32_t limit = (n / HOP_SAMPLE_MHZ + 100u) * tpu;
    DUMP_RAM[n - 1] = SENTINEL;
    FENCE();
    portENTER_CRITICAL(&s_mux);
    uint32_t ctrl = (REG32(DUMP_CTRL) & ~(CTRL_COUNT_MASK | CTRL_ENABLE | CTRL_TRIGGER)) | n;
    REG32(HP_SRAM_USAGE) = (REG32(HP_SRAM_USAGE) & ~SRAM_OWNER_MASK) | SRAM_OWNER_DUMP;
    REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    FENCE();
    const uint32_t t0 = ccount();
    REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE | CTRL_TRIGGER;
    REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    bool done;
    uint32_t dt;
    do {
        done = REG32(DUMP_CTRL) & CTRL_DONE;
        dt = ccount() - t0;
    } while (!done && dt < limit);
    REG32(DUMP_CTRL) = ctrl;
    REG32(HP_SRAM_USAGE) &= ~SRAM_OWNER_MASK;
    FENCE();
    portEXIT_CRITICAL(&s_mux);
    if (cycles) *cycles = dt;
    return done && DUMP_RAM[n - 1] != SENTINEL;
}

volatile uint32_t *hop_dump_ram(void) { return DUMP_RAM; }

bool hop_gain_check(void)
{
    uint32_t v = REG32(RX_GAIN_FORCE);
    if ((v >> 24) == s_cfg.gain && (v & GAIN_FORCE_BIT)) return false;
    phy_force_rx_gain(true, s_cfg.gain);
    return true;
}

// 160 * log10(p) = 1/16 dB, from log2 with an 8-bit mantissa table.
static int32_t db16(uint64_t p)
{
    if (p == 0) return 0;
    int e = 63 - __builtin_clzll(p);
    uint32_t f = e >= 8 ? (uint32_t)(p >> (e - 8)) & 0xffu : (uint32_t)(p << (8 - e)) & 0xffu;
    uint32_t log2_q8 = ((uint32_t)e << 8) + s_log2_frac[f];
    return (int32_t)((log2_q8 * 12330u) >> 16);    // x 160 * log10(2) / 256
}

// A full-scale tone (amplitude 512) exactly in the bin gives
// |X| = 512 * sum(w) * 16384 = 2^25 per block, so |X|^2 = 2^50 = 0 dBFS.
// db16(2^50) = 2408; rssi = 16 * (dBFS + 100) = db16 - 2408 + 1600.
#define RSSI_OFFSET  808

static inline int32_t sext10(uint32_t v) { return (int32_t)(v << 22) >> 22; }

// Band power around the pilot. Pass 1: DC (mean I/Q) and clip count. Pass
// 2: each block of 8 samples, DC removed, dotted with the window x mixer
// table: one 8-point DFT bin at the pilot (Hann, ~15 MHz noise bandwidth,
// exact nulls at DC+-fs/8 and on the image at the opposite bin). Result:
// mean |X|^2 over the blocks.
// In IRAM and read through a plain pointer (the bank is static once the
// snapshot is done; hop_capture() ends with a fence): ~3x faster than a
// volatile loop running from flash.
IRAM_ATTR uint16_t hop_measure(uint32_t first, uint32_t n, uint8_t *clip)
{
    const uint32_t *x = (const uint32_t *)DUMP_RAM + first;
    int32_t si = 0, sq = 0;
    uint32_t clips = 0;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t w = x[k];
        int32_t i = sext10(w), q = sext10(w >> 10);
        si += i;
        sq += q;
        clips += (i >= 511 || i <= -512 || q >= 511 || q <= -512);
    }
    const int32_t half = (int32_t)n / 2;
    const int32_t di = (si >= 0 ? si + half : si - half) / (int32_t)n;
    const int32_t dq = (sq >= 0 ? sq + half : sq - half) / (int32_t)n;
    uint64_t acc = 0;
    for (uint32_t b = 0; b < n; b += 8) {
        int32_t xr = 0, xi = 0;
        for (int j = 0; j < 8; ++j) {
            uint32_t w = x[b + j];
            int32_t i = sext10(w) - di, q = sext10(w >> 10) - dq;
            xr += i * s_cr[j] - q * s_ci[j];
            xi += i * s_ci[j] + q * s_cr[j];
        }
        acc += (uint64_t)((int64_t)xr * xr) + (uint64_t)((int64_t)xi * xi);
    }
    if (clip) *clip = clips > 255 ? 255 : (uint8_t)clips;
    int32_t r = db16(acc / (n / 8)) - RSSI_OFFSET;
    return r < 1 ? 1 : (r > 65535 ? 65535 : (uint16_t)r);
}

unsigned hop_cycle(hop_reading_t out[HOP_MAX_SLOTS], uint8_t *mask, uint32_t *t0_us)
{
    const uint32_t tpu = esp_rom_get_cpu_ticks_per_us();
    const uint32_t c0 = ccount();
    *t0_us = (uint32_t)esp_timer_get_time();
    *mask = 0;
    unsigned k = 0;
    for (unsigned s = 0; s < HOP_MAX_SLOTS; ++s) {
        if (!s_cfg.mhz[s]) continue;
        *mask |= 1u << s;
        if (hop_gain_check()) s_stats.gain_rewrites++;
        const uint32_t a = ccount();
        hop_tune(hop_lo_mhz(s_cfg.mhz[s]));
        const uint32_t b = ccount();
        if (s_cfg.settle_us) esp_rom_delay_us(s_cfg.settle_us);
        const bool ok = hop_capture(s_cfg.samples, NULL);
        const uint32_t c = ccount();
        out[k].t_us = (uint16_t)((c - c0) / tpu);
        out[k].clip = 0;
        out[k].rssi = ok ? hop_measure(0, s_cfg.samples, &out[k].clip) : 0;
        if (!ok) s_stats.timeouts++;
        const uint32_t d = ccount();
        stat_add(&s_stats.tune, b - a);
        stat_add(&s_stats.capture, c - b);
        stat_add(&s_stats.meter, d - c);
        stat_add(&s_stats.slot, d - a);
        k++;
    }
    if (k) {
        stat_add(&s_stats.cycle, ccount() - c0);
        s_stats.cycles++;
    }
    return k;
}

void hop_stats_get(hop_stats_t *out, bool reset)
{
    *out = s_stats;
    if (reset) memset(&s_stats, 0, sizeof s_stats);
}
