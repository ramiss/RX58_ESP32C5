// SPDX-License-Identifier: GPL-3.0-only
//
// Continuous Q4/I4 capture ring.
//
// Adapted from C5VRX by Twotoz and the C5VRX contributors
// (https://github.com/Twotoz/C5VRX, GPL-3.0-only, commit 14e190d):
// route_modem_iq() and the lane table from main/rf.c; prepare_rx(),
// start_rx(), find_dma(), clear_eof(), quiet_interrupts() and the
// newest-descriptor access from main/transport.c. Changes: our pad map
// (README "IQ loopback pads"), RX only (the BitScrambler/PARLIO TX video
// half comes in Phase D), only the RX channel's interrupts are touched, and
// in-place access with a completion counter instead of copies.
//
// MODEM_DIAG I/Q bits go out through 8 pads and straight back in through
// the same pads into PARLIO RX at 40 MS/s. With suc_eof cleared on every
// descriptor and interrupts off, the GDMA ring never stops (C5VRX;
// espressif/esp-idf#19091), so no descriptor boundary is a stream gap.

#include "capture_ring.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "esp_attr.h"
#include "esp_rom_gpio.h"
#include "hal/dma_types.h"
#include "soc/ahb_dma_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/parl_io_struct.h"

#define MAX_NODES       16
#define PERI_SEL_PARLIO 9

// PARLIO data bits 0-3 = Q, 4-7 = I. All eight pads passed the 9-min
// loopback soak (README "IQ loopback pads"); GPIO25 is header pin D2.
static const gpio_num_t s_pins[8] = {
    GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5,      // Q
    GPIO_NUM_26, GPIO_NUM_27, GPIO_NUM_28, GPIO_NUM_25,  // I
};

// MODEM_DIAG bit per data bit (C5VRX rf.c): Q is DIAG 0-9, I is DIAG 10-19,
// lowest selected bit first, the sign bit (9 / 19) last.
static const uint8_t s_lane_diag[RING_LANES][8] = {
    {6u, 7u, 8u, 9u, 16u, 17u, 18u, 19u},   // coarse    {9,8,7,6}
    {5u, 6u, 7u, 9u, 15u, 16u, 17u, 19u},   // fine      {9,7,6,5}
    {4u, 5u, 6u, 9u, 14u, 15u, 16u, 19u},   // ultrafine {9,6,5,4}
};
static const unsigned s_lane_step[RING_LANES] = {64u, 32u, 16u};

DMA_ATTR __attribute__((aligned(64))) static uint8_t s_ring[RING_BYTES];

typedef struct {
    const dma_descriptor_t *dscr;
    const uint8_t *buf;
    size_t len;
} node_t;

static parlio_rx_unit_handle_t s_rx;
static parlio_rx_delimiter_handle_t s_delim;
static int s_ch = -1;
static node_t s_nodes[MAX_NODES];
static int s_nnodes;
static ring_lane_t s_lane;
static int s_last_active = -1;
static uint32_t s_done;          // descriptors completed so far
static uint32_t s_faults;

static esp_err_t route(ring_lane_t lane)
{
    uint64_t mask = 0;
    for (unsigned k = 0; k < 8; ++k) mask |= 1ULL << s_pins[k];
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    for (unsigned k = 0; k < 8; ++k)
        esp_rom_gpio_connect_out_signal(s_pins[k], MODEM_DIAG0_IDX + s_lane_diag[lane][k], false, false);
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    s_lane = lane;
    return ESP_OK;
}

esp_err_t ring_set_lane(ring_lane_t lane)
{
    if (lane >= RING_LANES) return ESP_ERR_INVALID_ARG;
    for (unsigned k = 0; k < 8; ++k)
        esp_rom_gpio_connect_out_signal(s_pins[k], MODEM_DIAG0_IDX + s_lane_diag[lane][k], false, false);
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    s_lane = lane;
    return ESP_OK;
}

ring_lane_t ring_lane(void) { return s_lane; }
unsigned ring_lane_step(ring_lane_t lane) { return lane < RING_LANES ? s_lane_step[lane] : 0; }

static int find_rx_dma(void)
{
    for (int k = 0; k < 3; ++k)
        if (AHB_DMA.channel[k].in.in_peri_sel.peri_in_sel_chn == PERI_SEL_PARLIO) return k;
    return -1;
}

// Clear suc_eof on every descriptor so the ring never ends; record them.
static int clear_eof(int ch)
{
    uint32_t first = AHB_DMA.channel[ch].in.in_dscr_bf0.val;
    if (first < 0x40800000u || first >= 0x40860000u) return 0;
    dma_descriptor_t *d = (dma_descriptor_t *)(uintptr_t)first;
    int n = 0;
    while (d && n < MAX_NODES) {
        d->dw0.suc_eof = 0;
        s_nodes[n].dscr = d;
        s_nodes[n].buf = (const uint8_t *)d->buffer;
        s_nodes[n].len = d->dw0.size;
        d = d->next;
        ++n;
        if ((uintptr_t)d == first) break;
    }
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    return n;
}

esp_err_t ring_start(ring_lane_t lane)
{
    if (lane >= RING_LANES) return ESP_ERR_INVALID_ARG;
    esp_err_t err = route(lane);
    if (err != ESP_OK) return err;
    memset(s_ring, 0, sizeof s_ring);
    const parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1u,
        .max_recv_size = sizeof s_ring,
        .dma_burst_size = 32u,
        .data_width = 8u,
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .exp_clk_freq_hz = RING_RATE_HZ,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .data_gpio_nums = {s_pins[0], s_pins[1], s_pins[2], s_pins[3],
                           s_pins[4], s_pins[5], s_pins[6], s_pins[7]},
        .flags = {.free_clk = true, .clk_gate_en = false, .allow_pd = false},
    };
    if ((err = parlio_new_rx_unit(&cfg, &s_rx)) != ESP_OK) return err;
    const parlio_rx_soft_delimiter_config_t delim = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = sizeof s_ring,
        .timeout_ticks = 0u,
    };
    if ((err = parlio_new_rx_soft_delimiter(&delim, &s_delim)) != ESP_OK) return err;
    if ((err = parlio_rx_unit_enable(s_rx, false)) != ESP_OK) return err;
    if ((err = parlio_rx_soft_delimiter_start_stop(s_rx, s_delim, true)) != ESP_OK) return err;
    const parlio_receive_config_t rcv = {
        .delimiter = s_delim,
        .flags = {.partial_rx_en = true, .indirect_mount = false},
    };
    if ((err = parlio_rx_unit_receive(s_rx, s_ring, sizeof s_ring, &rcv)) != ESP_OK) return err;

    s_ch = find_rx_dma();
    if (s_ch < 0) return ESP_ERR_NOT_FOUND;
    AHB_DMA.in_intr[s_ch].ena.val = 0;
    PARL_IO.int_ena.val = 0;
    // External-enable EOF selection: soft-delimiter RX never stalls on EOF.
    PARL_IO.rx_genrl_cfg.rx_eof_gen_sel = 1;
    s_nnodes = clear_eof(s_ch);
    if (s_nnodes < 3) return ESP_ERR_INVALID_STATE;
    PARL_IO.int_clr.val = UINT32_MAX;
    AHB_DMA.in_intr[s_ch].clr.val = UINT32_MAX;
    return ESP_OK;
}

static int active_index(void)
{
    uint32_t a = AHB_DMA.channel[s_ch].in.in_dscr_bf0.val;
    for (int k = 0; k < s_nnodes; ++k)
        if ((uintptr_t)s_nodes[k].dscr == a) return k;
    return -1;
}

// Advance the completion counter. Must run at least once per ring period
// (819 us) to count every descriptor.
static int update(void)
{
    if (PARL_IO.int_raw.rx_fifo_wovf_int_raw) { ++s_faults; PARL_IO.int_clr.rx_fifo_wovf_int_clr = 1; }
    uint32_t raw = AHB_DMA.in_intr[s_ch].raw.val & 0xfcu;   // descriptor / bus errors
    if (raw) { ++s_faults; AHB_DMA.in_intr[s_ch].clr.val = raw; }
    int a = active_index();
    if (a < 0) return -1;
    if (s_last_active >= 0) s_done += (uint32_t)((a - s_last_active + s_nnodes) % s_nnodes);
    s_last_active = a;
    return a;
}

bool ring_newest(const uint8_t **samples, size_t *len, uint32_t *seq)
{
    if (s_ch < 0) return false;
    int a = update();
    if (a < 0 || s_done == 0) return false;
    int idx = (a - 1 + s_nnodes) % s_nnodes;
    *samples = s_nodes[idx].buf;
    *len = s_nodes[idx].len;
    *seq = s_done;
    return true;
}

bool ring_still_valid(uint32_t seq)
{
    if (s_ch < 0 || update() < 0) return false;
    // The descriptor that completed as number seq is rewritten once RX has
    // gone round the other nnodes - 1 descriptors.
    return s_done - seq < (uint32_t)(s_nnodes - 1);
}

uint32_t ring_faults(void) { return s_faults; }
