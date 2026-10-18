// SPDX-License-Identifier: GPL-3.0-only
//
// RX5808 3-wire bus slave.
//
// Writes, at any host speed (1 kbit/s bit-banged to 4 MHz hardware SPI):
// the GPSPI2 slave with SELECT as chip select, SPI mode 0, LSB first. Each
// completed frame is decoded in the driver's post-transaction callback (its
// interrupt) and its transaction re-queued right there, so the queue never
// runs dry: re-queueing from a task lost the 9th and 10th of 10
// back-to-back frames (2026-10-17). A frame of fewer than 25 bits is
// discarded; bits after the 25th are ignored (32-clock hosts).
//
// Reads (datasheet section 5): the SPI slave cannot drive DATA mid-frame,
// so a GPIO interrupt handler follows CLOCK. It samples A0-A3 and R/W on
// rising edges; a bit faster than 30 us means a write-speed frame and the
// handler switches itself off for the rest of it. After a slow R/W = 0 it
// drives D0 (at once if 0, else when the host releases DATA and the line
// rises), then each next bit on each CLOCK falling edge, and releases DATA
// when SELECT rises.
//
// Fast readout (datasheet section 8): a READ_SELECT write builds the block
// into the shared transmit buffer from the SPI driver's callback, before the
// driver loads the next transaction (which copies that buffer into the SPI
// hardware). The very next frame is the readout: at its SELECT fall the
// DATA pad is switched to the SPI slave's output (FSPIQ) and driven; at
// SELECT rise it goes back to an input. The readout frame's own received
// bits (our own data) are discarded. Without DMA a frame carries at most 64
// bytes, so every block fits in 64 (PEAKS for 8 slots is 60).

#include "rx5808_bus.h"
#include "regs.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/gpio_filter.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "esp_private/spi_slave_internal.h"
#include "esp_cpu.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"

#define NTRANS        8
#define TRANS_BYTES   BUS_BLOCK_MAX   // 64: the most the slave moves without DMA
#define SLOW_BIT_US   30              // datasheet: >= 40 us per address bit for reads
#define LOG_LEN       16

typedef enum { ST_IDLE, ST_ADDR, ST_READ, ST_OFF } rd_state_t;

static spi_slave_transaction_t s_tr[NTRANS];
static gpio_glitch_filter_handle_t s_filter[3];   // SELECT (flex), CLOCK, DATA (pin)
static unsigned s_filter_mask;
static uint8_t s_rx[NTRANS][TRANS_BYTES] __attribute__((aligned(4)));
// Shared transmit buffer: the latest block snapshot (0xFF past its end).
static DRAM_ATTR uint8_t s_tx[TRANS_BYTES] __attribute__((aligned(4)));
static bus_block_builder_t s_builder;
static volatile bool s_armed;           // the next frame is a readout
static volatile bool s_in_readout;      // a readout frame is in progress
static volatile unsigned s_readout_pending;  // readout frames whose transaction is not done yet
static bool s_have_snapshot;
static bus_stats_t s_st;
static bus_frame_t s_log[LOG_LEN];
static unsigned s_log_n;

// Read handler state (ISR only, except s_state which the task reads).
static volatile rd_state_t s_state;
static unsigned s_bit, s_out;
static uint32_t s_word, s_value, s_t_last, s_slow_cycles;
static bool s_skip_fall, s_driving;

static inline void clk_irq(gpio_int_type_t type)
{
    gpio_ll_set_intr_type(&GPIO, BUS_PIN_CLK, type);
    gpio_ll_clear_intr_status(&GPIO, 1u << BUS_PIN_CLK);
    if (type == GPIO_INTR_DISABLE) gpio_ll_intr_disable(&GPIO, BUS_PIN_CLK);
    else gpio_ll_intr_enable_on_core(&GPIO, 0, BUS_PIN_CLK);
}

static inline void data_irq(bool on)
{
    gpio_ll_set_intr_type(&GPIO, BUS_PIN_DATA, on ? GPIO_INTR_POSEDGE : GPIO_INTR_DISABLE);
    gpio_ll_clear_intr_status(&GPIO, 1u << BUS_PIN_DATA);
    if (on) gpio_ll_intr_enable_on_core(&GPIO, 0, BUS_PIN_DATA);
    else gpio_ll_intr_disable(&GPIO, BUS_PIN_DATA);
}

static inline void drive(int level)
{
    gpio_ll_set_level(&GPIO, BUS_PIN_DATA, level);
    if (!s_driving) {
        gpio_ll_output_enable(&GPIO, BUS_PIN_DATA);
        s_driving = true;
    }
}

static inline void release(void)
{
    gpio_ll_output_disable(&GPIO, BUS_PIN_DATA);
    s_driving = false;
}

static IRAM_ATTR void readout_begin(void)
{
    esp_rom_gpio_connect_out_signal(BUS_PIN_DATA, FSPIQ_OUT_IDX, false, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, BUS_PIN_DATA, false, false);   // we, not the SPI block, enable it
    gpio_ll_output_enable(&GPIO, BUS_PIN_DATA);
    s_in_readout = true;
}

static IRAM_ATTR void readout_end(void)
{
    gpio_ll_output_disable(&GPIO, BUS_PIN_DATA);
    esp_rom_gpio_connect_out_signal(BUS_PIN_DATA, SIG_GPIO_OUT_IDX, false, false);
    s_in_readout = false;
}

static IRAM_ATTR void sel_isr(void *arg)
{
    (void)arg;
    if (gpio_ll_get_level(&GPIO, BUS_PIN_SEL)) {          // SELECT rose: frame over
        if (s_in_readout) readout_end();
        release();
        data_irq(false);
        clk_irq(GPIO_INTR_DISABLE);
        s_state = ST_IDLE;
    } else if (s_armed) {                                  // SELECT fell: the readout frame
        s_armed = false;
        s_readout_pending++;
        s_state = ST_OFF;                                  // no legacy-read decoding on our own data
        clk_irq(GPIO_INTR_DISABLE);
        readout_begin();
    } else {                                               // SELECT fell: new frame
        release();
        s_bit = 0;
        s_word = 0;
        s_t_last = esp_cpu_get_cycle_count();
        s_state = ST_ADDR;
        clk_irq(GPIO_INTR_POSEDGE);
    }
}

static IRAM_ATTR void clk_isr(void *arg)
{
    (void)arg;
    const uint32_t now = esp_cpu_get_cycle_count();
    if (s_state == ST_ADDR) {
        if (s_bit > 0 && now - s_t_last < s_slow_cycles) {  // write-speed frame
            clk_irq(GPIO_INTR_DISABLE);
            s_state = ST_OFF;
            return;
        }
        s_t_last = now;
        s_word |= (uint32_t)gpio_ll_get_level(&GPIO, BUS_PIN_DATA) << s_bit;
        if (++s_bit < 5) return;
        if (s_word & 0x10u) {                                // a write: the SPI slave has it
            clk_irq(GPIO_INTR_DISABLE);
            s_state = ST_OFF;
            return;
        }
        s_value = regs_read(s_word & 0xfu);
        s_out = 0;
        s_skip_fall = true;                                  // R/W's own falling edge comes next
        s_state = ST_READ;
        clk_irq(GPIO_INTR_NEGEDGE);
        // D0 = 0 can be driven now: the host holds the line low after R/W = 0.
        if (!(s_value & 1u)) drive(0);
        else data_irq(true);                                 // wait for the release
    } else if (s_state == ST_READ) {
        s_t_last = now;
        if (s_skip_fall) { s_skip_fall = false; return; }
        if (++s_out >= 20) {
            release();
            clk_irq(GPIO_INTR_DISABLE);
            s_state = ST_OFF;
            return;
        }
        data_irq(false);
        drive((int)((s_value >> s_out) & 1u));
    }
}

static IRAM_ATTR void data_isr(void *arg)
{
    (void)arg;
    // The host released DATA and the pull-up raised it: D0 = 1 now.
    data_irq(false);
    if (s_state == ST_READ && s_out == 0) drive(1);
}

static IRAM_ATTR void log_frame(uint32_t bits, uint32_t word)
{
    bus_frame_t *f = &s_log[s_log_n++ % LOG_LEN];
    f->t_us = (uint32_t)esp_timer_get_time();
    f->bits = bits;
    f->addr = word & 0xfu;
    f->write = (word >> 4) & 1u;
    f->data = (word >> 5) & 0xfffffu;
}

// READ_SELECT (from regs_write() in frame_done(), i.e. this interrupt):
// snapshot block n into s_tx, or re-arm the last snapshot.
static IRAM_ATTR void read_select(uint32_t v)
{
    s_st.requests++;
    if (v & READ_REARM) {
        if (s_have_snapshot) s_armed = true;
        return;
    }
    size_t n = s_builder ? s_builder(v & 0x7fu, s_tx) : 0;
    if (n > TRANS_BYTES) n = TRANS_BYTES;
    for (size_t i = n; i < TRANS_BYTES; ++i) s_tx[i] = 0xff;   // past the end reads as 1s
    s_have_snapshot = true;
    s_armed = true;
}

// Driver interrupt: one frame done. Decode, store, re-queue.
static IRAM_ATTR void frame_done(spi_slave_transaction_t *t)
{
    s_st.frames++;
    if (s_readout_pending) {
        // A readout frame: what came in on DATA was our own data.
        s_readout_pending--;
        s_st.readouts++;
        t->trans_len = 0;
        if (spi_slave_queue_trans_isr(SPI2_HOST, t) != ESP_OK) s_st.overruns++;
        return;
    }
    const uint32_t bits = t->trans_len;
    const uint8_t *b = t->rx_buffer;
    const uint32_t word = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    log_frame(bits, word);
    // All ones (25+ bits) is what an undriven bus reads through the host's
    // pull-up, e.g. a readout frame the slave did not recognise; as a write
    // it would be 0x0F = reset. Hosts reset with 0x0F = 0. Ignore it.
    if (bits >= 25 && (word & 0x1ffffffu) == 0x1ffffffu) { s_st.floating++; goto requeue; }
    if (bits < 25) s_st.short_frames++;
    else if (word & 0x10u) { s_st.writes++; regs_write(word & 0xfu, (word >> 5) & 0xfffffu); }
    else s_st.reads++;   // R/W = 0: answered by the GPIO handlers if it was slow
requeue:
    t->trans_len = 0;
    if (spi_slave_queue_trans_isr(SPI2_HOST, t) != ESP_OK) s_st.overruns++;
}

esp_err_t bus_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = BUS_PIN_DATA, .miso_io_num = -1, .sclk_io_num = BUS_PIN_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
    };
    const spi_slave_interface_config_t slv = {
        .spics_io_num = BUS_PIN_SEL,
        .flags = SPI_SLAVE_BIT_LSBFIRST | SPI_SLAVE_NO_RETURN_RESULT,
        .queue_size = NTRANS,
        .mode = 0,
        .post_trans_cb = frame_done,
    };
    memset(s_tx, 0xff, sizeof s_tx);
    regs_set_select_cb(read_select);
    esp_err_t err = spi_slave_initialize(SPI2_HOST, &bus, &slv, SPI_DMA_DISABLED);
    if (err != ESP_OK) return err;
    for (int i = 0; i < NTRANS; ++i) {
        memset(&s_tr[i], 0, sizeof s_tr[i]);
        s_tr[i].length = TRANS_BYTES * 8;
        s_tr[i].rx_buffer = s_rx[i];
        s_tr[i].tx_buffer = s_tx;
        if ((err = spi_slave_queue_trans(SPI2_HOST, &s_tr[i], 0)) != ESP_OK) return err;
    }

    // DATA is the SPI slave's input; as an output it is a plain GPIO, off
    // unless a read frame is being answered.
    esp_rom_gpio_connect_out_signal(BUS_PIN_DATA, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_output_disable(&GPIO, BUS_PIN_DATA);
    s_slow_cycles = SLOW_BIT_US * esp_rom_get_cpu_ticks_per_us();

    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    gpio_isr_handler_add(BUS_PIN_SEL, sel_isr, NULL);
    gpio_isr_handler_add(BUS_PIN_CLK, clk_isr, NULL);
    gpio_isr_handler_add(BUS_PIN_DATA, data_isr, NULL);
    clk_irq(GPIO_INTR_DISABLE);
    data_irq(false);
    gpio_set_intr_type(BUS_PIN_SEL, GPIO_INTR_ANYEDGE);
    gpio_intr_enable(BUS_PIN_SEL);
    // The input glitch filters come later, from bus_filters() once the
    // radio is up.
    return ESP_OK;
}

// Frames are handled in frame_done(); nothing is left for the task.
void bus_poll(void) {}

void bus_set_block_builder(bus_block_builder_t fn) { s_builder = fn; }

// Input glitch filters. A host whose DATA changes on the CLOCK falling edge
// (hardware SPI) made SELECT glitch at the C5 on bench jumpers: one 32-clock
// frame arrived as nine 1-4 bit frames (2026-10-17). SELECT never
// legitimately pulses shorter than 10 us, so a 250 ns flex filter costs
// nothing; CLOCK and DATA get the 2-cycle pin filter (4 MHz safe).
// Created on first use, after the radio has started: filters created
// before Wi-Fi started blocked the pins when enabled (only an off/on cycle
// afterwards restored them).
static void create_filters(void)
{
    // All three are fixed 2-cycle pin filters. A 250 ns flex filter on
    // SELECT also fixed the hardware-SPI frames, but its state went bad
    // during operation (every frame "short" until it was turned off and on
    // again, 2026-10-17).
    static const gpio_num_t pins[3] = {BUS_PIN_SEL, BUS_PIN_CLK, BUS_PIN_DATA};
    for (int i = 0; i < 3; ++i) {
        const gpio_pin_glitch_filter_config_t pf = {.clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT, .gpio_num = pins[i]};
        (void)gpio_new_pin_glitch_filter(&pf, &s_filter[i]);
    }
}

unsigned bus_filters(unsigned mask)
{
    static bool created;
    if (!created) { create_filters(); created = true; }
    for (int i = 0; i < 3; ++i) {
        if (!s_filter[i]) continue;
        const bool want = mask & (1u << i), have = s_filter_mask & (1u << i);
        if (want && !have) gpio_glitch_filter_enable(s_filter[i]);
        if (!want && have) gpio_glitch_filter_disable(s_filter[i]);
    }
    s_filter_mask = mask & 7u;
    return s_filter_mask;
}

// A frame counts as active only while its CLOCK keeps moving: SELECT stuck
// low (or an open SELECT wire) must not stall the radio task, which once
// spun here forever and took the console with it (2026-10-17).
#define STALL_US 5000

// Only the data bits of a legacy read are timing-critical (each bit within
// 5 us of CLOCK falling): the radio pauses snapshots for those (~0.4 ms at
// RotorHazard timing), not for the ~5 ms address phase, which once cost a
// 5.8 ms gap in the readings (soak, 2026-10-18).
bool bus_slow_frame_active(void)
{
    if (s_state != ST_READ) return false;
    return esp_cpu_get_cycle_count() - s_t_last < STALL_US * esp_rom_get_cpu_ticks_per_us();
}

void bus_stats(bus_stats_t *out, bool reset)
{
    *out = s_st;
    if (reset) memset(&s_st, 0, sizeof s_st);
}

unsigned bus_log(bus_frame_t *out, unsigned max)
{
    unsigned n = s_log_n < LOG_LEN ? s_log_n : LOG_LEN;
    if (n > max) n = max;
    for (unsigned i = 0; i < n; ++i) out[i] = s_log[(s_log_n - n + i) % LOG_LEN];
    return n;
}
