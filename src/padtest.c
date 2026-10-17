// SPDX-License-Identifier: GPL-3.0-only
//
// IQ loopback pad test (README "IQ loopback pads").
//
// An IQ lane drives a signal out of a pad and reads it straight back into
// PARLIO RX through the same pad at 40 MHz, so a usable pad must carry that
// edge rate with whatever the XIAO board hangs on it. Eight lanes per run:
// three references C5VRX already proved (GPIO3/4/5), the candidates
// GPIO2/26/27/28, and one header pin (D2/GPIO25 in set A, D3/GPIO7 in set B).
//
// 1. Pattern test: PARLIO TX drives a pseudo-random byte stream onto the
//    eight pads; PARLIO RX samples the same pads. Every bit is known, so
//    errors are exact. At 20 MHz each bit gets two 40 MHz samples: the
//    middle one must be error-free, and the edge one measures how late a
//    pad's edges are compared with the references (a loaded pad is slow).
//    The 40 MHz run is one sample per bit; its fixed TX/RX phase can make
//    even the references err, so lanes are judged against them.
// 2. Real-signal test: one live MODEM_DIAG bit is routed to all eight pads
//    at once and RX samples them with its free-running 40 MHz clock, as in
//    real use. Each lane must match the reference lane as well as the other
//    references do, and toggle as often.
//
// The pads must have nothing connected externally. Don't press BOOT
// (GPIO28) while it runs.

#include "padtest.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "driver/parlio_tx.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_attr.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rf_min.h"
#include "dlboot.h"
#include "soc/gpio_sig_map.h"

#define NLANES        8
#define NREF          3          // lanes 0..2 are the proven references
#define RX_HZ         40000000u
#define TX_BYTES      4096u
#define RX_BYTES      32768u
#define PREAMBLE      16u        // 8 bytes 0xFF, 8 bytes 0x00, then PRBS
#define TEST_GAIN     60u        // Phase A fixed gain
#define ALIGN_SPAN    3

typedef struct {
    const char *name;
    gpio_num_t pads[NLANES];
} lane_set_t;

static const lane_set_t SETS[] = {
    {"A: header lane D2/GPIO25", {GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_2,
                                  GPIO_NUM_26, GPIO_NUM_27, GPIO_NUM_28, GPIO_NUM_25}},
    {"B: header lane D3/GPIO7",  {GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_2,
                                  GPIO_NUM_26, GPIO_NUM_27, GPIO_NUM_28, GPIO_NUM_7}},
    // Negative control: the same pads, but lane 7 (GPIO25) has its output
    // switched off (BROKEN_LANE), so it reads a floating pad instead of its
    // own signal. It must FAIL; if it passes, the test cannot tell a bad
    // pad from a good one. (The first control drove GPIO6's 1 nF at 40 MHz
    // and wedged the chip twice: USB dead until a power cycle.)
    {"C: negative control (lane 7 output off)", {GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_2,
                                                 GPIO_NUM_26, GPIO_NUM_27, GPIO_NUM_28, GPIO_NUM_25}},
};
#define BROKEN_SET   2   // index of the negative-control set in SETS
#define BROKEN_LANE  7
#define NSETS (sizeof(SETS) / sizeof(SETS[0]))

// MODEM_DIAG bits for the real-signal test: Q4 (an LSB, toggles most),
// Q9 (sign) and I4 (C5VRX's ultrafine lane uses DIAG 4,5,6,9 / 14,15,16,19).
static const uint8_t DIAG_BITS[] = {4, 9, 14};
#define NDIAG (sizeof(DIAG_BITS) / sizeof(DIAG_BITS[0]))

static const char *pad_desc(gpio_num_t g)
{
    switch (g) {
    case GPIO_NUM_2:  return "GPIO2  TP7 (MTMS)";
    case GPIO_NUM_3:  return "GPIO3  TP6  ref";
    case GPIO_NUM_4:  return "GPIO4  TP5  ref";
    case GPIO_NUM_5:  return "GPIO5  TP4  ref";
    case GPIO_NUM_7:  return "GPIO7  D3 header";
    case GPIO_NUM_25: return "GPIO25 D2 header";
    case GPIO_NUM_26: return "GPIO26 batt-sense enable";
    case GPIO_NUM_27: return "GPIO27 yellow LED";
    case GPIO_NUM_28: return "GPIO28 BOOT button";
    default:          return "?";
    }
}

static DMA_ATTR __attribute__((aligned(64))) uint8_t s_tx[TX_BYTES];
static DMA_ATTR __attribute__((aligned(64))) uint8_t s_rx[RX_BYTES];

static parlio_rx_unit_handle_t s_rxu;
static parlio_rx_delimiter_handle_t s_delim;
static parlio_tx_unit_handle_t s_txu;
static bool s_rf_up;

// ---- results ---------------------------------------------------------------

typedef struct {
    uint64_t bits;                  // bits compared per lane
    uint64_t err[NLANES];           // middle sample (or the only sample)
    uint64_t edge[NLANES];          // second sample per bit (20 MHz only)
    uint32_t trials, lost;          // lost = no preamble seen
} pattern_res_t;

typedef struct {
    uint64_t samples;
    uint64_t mismatch[NLANES];      // vs lane 0
    uint64_t toggles[NLANES];
    uint32_t captures;
} diag_res_t;

typedef struct {
    pattern_res_t p20, p40;
    diag_res_t diag[NDIAG];
    bool done;
} set_res_t;

static set_res_t s_res[NSETS];
static bool s_rf_failed;

// ---- PARLIO + pad plumbing -------------------------------------------------

static esp_err_t rx_open(const gpio_num_t pads[NLANES])
{
    parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1,
        .max_recv_size = RX_BYTES,
        .dma_burst_size = 32,
        .data_width = NLANES,
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .exp_clk_freq_hz = RX_HZ,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .flags = {.free_clk = 1, .clk_gate_en = 0, .allow_pd = 0},
    };
    for (int k = 0; k < NLANES; ++k) cfg.data_gpio_nums[k] = pads[k];
    esp_err_t err = parlio_new_rx_unit(&cfg, &s_rxu);
    if (err != ESP_OK) return err;
    const parlio_rx_soft_delimiter_config_t d = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = RX_BYTES,
        .timeout_ticks = 0,
    };
    err = parlio_new_rx_soft_delimiter(&d, &s_delim);
    if (err != ESP_OK) return err;
    return parlio_rx_unit_enable(s_rxu, true);
}

static void rx_close(void)
{
    if (!s_rxu) return;
    (void)parlio_rx_unit_disable(s_rxu);
    (void)parlio_del_rx_delimiter(s_delim);
    (void)parlio_del_rx_unit(s_rxu);
    s_rxu = NULL;
    s_delim = NULL;
}

static esp_err_t tx_open(const gpio_num_t pads[NLANES], uint32_t hz)
{
    parlio_tx_unit_config_t cfg = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .output_clk_freq_hz = hz,
        .data_width = NLANES,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .trans_queue_depth = 1,
        .max_transfer_size = TX_BYTES,
        .dma_burst_size = 32,
        .shift_edge = PARLIO_SHIFT_EDGE_NEG,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
    };
    for (int k = 0; k < NLANES; ++k) cfg.data_gpio_nums[k] = pads[k];
    esp_err_t err = parlio_new_tx_unit(&cfg, &s_txu);
    if (err != ESP_OK) return err;
    return parlio_tx_unit_enable(s_txu);
}

static void tx_close(void)
{
    if (!s_txu) return;
    (void)parlio_tx_unit_disable(s_txu);
    (void)parlio_del_tx_unit(s_txu);
    s_txu = NULL;
}

// Make every pad an input+output loop: its output carries out_sig + k (or
// out_sig itself when same_sig), its input feeds PARLIO RX data line k.
// Done by hand after both drivers have claimed the pins, because each
// driver configures them for one direction only.
static esp_err_t pads_loop(const gpio_num_t pads[NLANES], uint32_t out_sig, bool same_sig,
                           int broken_lane)
{
    uint64_t mask = 0;
    for (int k = 0; k < NLANES; ++k) mask |= 1ULL << pads[k];
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    for (int k = 0; k < NLANES; ++k) {
        esp_rom_gpio_connect_out_signal(pads[k], same_sig ? out_sig : out_sig + k, false, false);
        esp_rom_gpio_connect_in_signal(pads[k], PARL_RX_DATA0_IDX + k, false);
    }
    if (broken_lane >= 0) {
        // Negative control: output off, pulled low, so the lane reads a
        // constant instead of its own signal.
        gpio_set_direction(pads[broken_lane], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pads[broken_lane], GPIO_PULLDOWN_ONLY);
    }
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    return ESP_OK;
}

static int broken_lane_of(const lane_set_t *set)
{
    return set == &SETS[BROKEN_SET] ? BROKEN_LANE : -1;
}

// One bounded RX capture into s_rx; optionally transmits s_tx meanwhile.
static esp_err_t capture(bool with_tx)
{
    memset(s_rx, 0xA5, sizeof(s_rx));
    const parlio_receive_config_t rc = {.delimiter = s_delim, .flags = {0}};
    esp_err_t err = parlio_rx_unit_receive(s_rxu, s_rx, sizeof(s_rx), &rc);
    if (err != ESP_OK) return err;
    err = parlio_rx_soft_delimiter_start_stop(s_rxu, s_delim, true);
    if (err != ESP_OK) return err;
    if (with_tx) {
        const parlio_transmit_config_t tc = {.idle_value = 0};
        err = parlio_tx_unit_transmit(s_txu, s_tx, TX_BYTES * 8u, &tc);
        if (err == ESP_OK) err = parlio_tx_unit_wait_all_done(s_txu, 200);
    }
    esp_err_t rerr = parlio_rx_unit_wait_all_done(s_rxu, 200);
    (void)parlio_rx_soft_delimiter_start_stop(s_rxu, s_delim, false);
    return err != ESP_OK ? err : rerr;
}

// ---- pattern test ----------------------------------------------------------

static uint32_t s_lfsr = 0xACE1u;

static uint8_t prbs_byte(void)
{
    // 32-bit Galois LFSR (taps 32,22,2,1); one byte per call.
    uint8_t b = 0;
    for (int i = 0; i < 8; ++i) {
        uint32_t lsb = s_lfsr & 1u;
        s_lfsr >>= 1;
        if (lsb) s_lfsr ^= 0x80200003u;
        b = (uint8_t)((b << 1) | lsb);
    }
    return b;
}

static void fill_pattern(void)
{
    for (unsigned i = 0; i < 8; ++i) s_tx[i] = 0xFF;
    for (unsigned i = 8; i < PREAMBLE; ++i) s_tx[i] = 0x00;
    for (unsigned i = PREAMBLE; i < TX_BYTES; ++i) s_tx[i] = prbs_byte();
}

// Per-lane errors comparing TX byte m with RX sample o + ratio*m.
static bool count_errors(long o, unsigned ratio, uint32_t err[NLANES])
{
    memset(err, 0, sizeof(uint32_t) * NLANES);
    if (o < 0 || o + (long)ratio * (TX_BYTES - 1) >= (long)RX_BYTES) return false;
    for (unsigned m = PREAMBLE; m < TX_BYTES; ++m) {
        uint8_t x = s_rx[o + ratio * m] ^ s_tx[m];
        while (x) {
            int k = __builtin_ctz(x);
            ++err[k];
            x &= (uint8_t)(x - 1);
        }
    }
    return true;
}

static uint32_t ref_sum(const uint32_t err[NLANES])
{
    uint32_t s = 0;
    for (int k = 0; k < NREF; ++k) s += err[k];
    return s;
}

static void pattern_trial(unsigned ratio, pattern_res_t *r)
{
    fill_pattern();
    ++r->trials;
    if (capture(true) != ESP_OK) { ++r->lost; return; }
    // Preamble: the first sample where all three references read 1.
    long s = -1;
    for (long n = 0; n < (long)RX_BYTES; ++n)
        if ((s_rx[n] & 0x07u) == 0x07u) { s = n; break; }
    if (s < 0) { ++r->lost; return; }
    uint32_t best[NLANES], cur[NLANES];
    long best_o = -1;
    uint32_t best_ref = UINT32_MAX;
    for (long o = s - ALIGN_SPAN; o <= s + ALIGN_SPAN; ++o) {
        if (!count_errors(o, ratio, cur)) continue;
        if (ref_sum(cur) < best_ref) { best_ref = ref_sum(cur); best_o = o; memcpy(best, cur, sizeof(best)); }
    }
    if (best_o < 0) { ++r->lost; return; }
    r->bits += TX_BYTES - PREAMBLE;
    for (int k = 0; k < NLANES; ++k) r->err[k] += best[k];
    if (ratio == 2) {
        // The other sample of the same bit is o-1 or o+1; the one belonging
        // to a neighbouring bit errs ~50% on the references.
        uint32_t a[NLANES], b[NLANES];
        bool ha = count_errors(best_o - 1, ratio, a), hb = count_errors(best_o + 1, ratio, b);
        const uint32_t *e = (ha && (!hb || ref_sum(a) <= ref_sum(b))) ? a : (hb ? b : NULL);
        if (e) for (int k = 0; k < NLANES; ++k) r->edge[k] += e[k];
    }
}

static void run_pattern(const lane_set_t *set, uint32_t tx_hz, unsigned ratio,
                        uint32_t seconds, pattern_res_t *r)
{
    memset(r, 0, sizeof(*r));
    esp_err_t err = rx_open(set->pads);
    if (err == ESP_OK) err = tx_open(set->pads, tx_hz);
    if (err == ESP_OK) err = pads_loop(set->pads, PARL_TX_DATA0_IDX, false, broken_lane_of(set));
    if (err != ESP_OK) {
        printf("  pattern %" PRIu32 " MHz: setup failed (%s)\n", tx_hz / 1000000u, esp_err_to_name(err));
        tx_close();
        rx_close();
        return;
    }
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    while (esp_timer_get_time() < end) {
        pattern_trial(ratio, r);
        if ((r->trials & 31u) == 0) vTaskDelay(1);
    }
    tx_close();
    rx_close();
}

// ---- real-signal (MODEM_DIAG) test -----------------------------------------

static void run_diag(const lane_set_t *set, uint8_t diag, uint32_t seconds, diag_res_t *r)
{
    memset(r, 0, sizeof(*r));
    esp_err_t err = rx_open(set->pads);
    if (err == ESP_OK) err = pads_loop(set->pads, MODEM_DIAG0_IDX + diag, true, broken_lane_of(set));
    if (err != ESP_OK) {
        printf("  DIAG%u: setup failed (%s)\n", diag, esp_err_to_name(err));
        rx_close();
        return;
    }
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    while (esp_timer_get_time() < end) {
        if (capture(false) != ESP_OK) continue;
        ++r->captures;
        uint8_t prev = s_rx[0];
        for (unsigned n = 0; n < RX_BYTES; ++n) {
            uint8_t v = s_rx[n];
            // Lane 0 bit replicated across all lanes; XOR gives mismatches.
            uint8_t ref = (v & 1u) ? 0xFFu : 0x00u;
            uint8_t x = v ^ ref, t = v ^ prev;
            while (x) { int k = __builtin_ctz(x); ++r->mismatch[k]; x &= (uint8_t)(x - 1); }
            while (t) { int k = __builtin_ctz(t); ++r->toggles[k]; t &= (uint8_t)(t - 1); }
            prev = v;
        }
        r->samples += RX_BYTES;
        vTaskDelay(1);
    }
    rx_close();
}

// ---- report ----------------------------------------------------------------

static double rate(uint64_t n, uint64_t d) { return d ? (double)n / (double)d : 0.0; }

static double ref_max_rate(const uint64_t v[NLANES], uint64_t d, int first)
{
    double m = 0;
    for (int k = first; k < NREF; ++k) if (rate(v[k], d) > m) m = rate(v[k], d);
    return m;
}

static void print_set(unsigned si)
{
    const lane_set_t *set = &SETS[si];
    const set_res_t *res = &s_res[si];
    if (!res->done) return;
    printf("\n=== Set %s ===\n", set->name);

    const pattern_res_t *p = &res->p20;
    printf("Pattern 20 MHz (2 samples/bit), %" PRIu64 " bits/lane, %" PRIu32 " trials, %" PRIu32 " lost\n",
           p->bits, p->trials, p->lost);
    printf("  lane pad                       mid errors   edge-sample err rate\n");
    for (int k = 0; k < NLANES; ++k)
        printf("  %d    %-24s %10" PRIu64 "   %9.5f\n", k, pad_desc(set->pads[k]), p->err[k], rate(p->edge[k], p->bits));
    p = &res->p40;
    printf("Pattern 40 MHz (1 sample/bit), %" PRIu64 " bits/lane, %" PRIu32 " trials, %" PRIu32 " lost\n",
           p->bits, p->trials, p->lost);
    for (int k = 0; k < NLANES; ++k)
        printf("  %d    %-24s err rate %9.6f\n", k, pad_desc(set->pads[k]), rate(p->err[k], p->bits));
    if (s_rf_failed) {
        printf("Real-signal test: SKIPPED (RF bring-up failed)\n");
    } else {
        for (unsigned d = 0; d < NDIAG; ++d) {
            const diag_res_t *g = &res->diag[d];
            printf("Real signal MODEM_DIAG%u, %" PRIu64 " samples/lane (%" PRIu32 " captures)\n",
                   DIAG_BITS[d], g->samples, g->captures);
            printf("  lane pad                       mismatch vs lane0   toggle rate\n");
            for (int k = 0; k < NLANES; ++k)
                printf("  %d    %-24s %12.6f        %8.4f\n", k, pad_desc(set->pads[k]),
                       rate(g->mismatch[k], g->samples), rate(g->toggles[k], g->samples));
        }
    }

    printf("Verdict (references = lanes 0-2):\n");
    double e20 = ref_max_rate(res->p20.edge, res->p20.bits, 0);
    double e40 = ref_max_rate(res->p40.err, res->p40.bits, 0);
    for (int k = NREF; k < NLANES; ++k) {
        const char *why = NULL;
        bool warn = false;
        if (res->p20.bits == 0) why = "pattern test produced no data";
        else if (res->p20.err[k]) why = "bit errors at 20 MHz";
        if (!why && !s_rf_failed) {
            for (unsigned d = 0; d < NDIAG && !why; ++d) {
                const diag_res_t *g = &res->diag[d];
                if (!g->samples) { why = "no real-signal data"; break; }
                double mm = rate(g->mismatch[k], g->samples);
                double mref = ref_max_rate(g->mismatch, g->samples, 1);
                double tr = rate(g->toggles[k], g->samples), t0 = rate(g->toggles[0], g->samples);
                if (mm > 2.0 * mref + 1e-4) why = "real signal differs from the references";
                else if (t0 > 0 && (tr < 0.95 * t0 || tr > 1.05 * t0)) why = "real signal toggles differently";
            }
        }
        if (!why) {
            if (rate(res->p20.edge[k], res->p20.bits) > 2.0 * e20 + 1e-4) warn = true;
            if (rate(res->p40.err[k], res->p40.bits) > 2.0 * e40 + 1e-5) warn = true;
        }
        const bool control = (k == broken_lane_of(set));
        printf("  %-24s %s%s%s\n", pad_desc(set->pads[k]),
               why ? "FAIL: " : warn ? "WARN: slower edges than the references" : "PASS",
               why ? why : "",
               !control ? "" : why ? "   <- negative control: FAIL expected, test is sensitive"
                                   : "   <- negative control PASSED: the test is NOT sensitive!");
    }
}

static void print_summary(void)
{
    printf("\n######## IQ loopback pad test results ########\n");
    for (unsigned si = 0; si < NSETS; ++si) print_set(si);
    printf("\nCommands: r = rerun, l = long soak (60 s per step), s = show results, "
           "B = reboot to download mode (for flashing)\n");
    fflush(stdout);
}

static void run_all(uint32_t pattern_s, uint32_t diag_s)
{
    if (!s_rf_up && !s_rf_failed) {
        printf("Starting RF front end (gain %u)...\n", (unsigned)TEST_GAIN);
        esp_err_t err = rf_min_start(TEST_GAIN);
        if (err == ESP_OK) s_rf_up = true;
        else { s_rf_failed = true; printf("RF bring-up FAILED: %s\n", esp_err_to_name(err)); }
    }
    for (unsigned si = 0; si < NSETS; ++si) {
        printf("Set %s: pattern 20 MHz...\n", SETS[si].name); fflush(stdout);
        run_pattern(&SETS[si], 20000000u, 2, pattern_s, &s_res[si].p20);
        printf("Set %s: pattern 40 MHz...\n", SETS[si].name); fflush(stdout);
        run_pattern(&SETS[si], 40000000u, 1, pattern_s, &s_res[si].p40);
        if (s_rf_up) {
            for (unsigned d = 0; d < NDIAG; ++d) {
                printf("Set %s: real signal DIAG%u...\n", SETS[si].name, DIAG_BITS[d]); fflush(stdout);
                run_diag(&SETS[si], DIAG_BITS[d], diag_s, &s_res[si].diag[d]);
            }
        }
        s_res[si].done = true;
    }
    print_summary();
}

void padtest_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&usb) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    dlboot_start_usb_guard();
    setvbuf(stdout, NULL, _IONBF, 0);

    for (int i = 5; i > 0; --i) {
        printf("IQ loopback pad test starts in %d s (nothing connected to the pads; don't press BOOT)\n", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    run_all(10, 3);

    int64_t next_print = esp_timer_get_time() + 30000000;
    for (;;) {
        uint8_t c = 0;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(200)) == 1) {
            if (c == 'r') run_all(10, 3);
            else if (c == 'l') run_all(60, 20);
            else if (c == 's') print_summary();
            else if (c == 'B') rf_min_reboot_to_download();
            next_print = esp_timer_get_time() + 30000000;
        }
        if (esp_timer_get_time() > next_print) {
            print_summary();
            next_print = esp_timer_get_time() + 30000000;
        }
    }
}
