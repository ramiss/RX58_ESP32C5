// SPDX-License-Identifier: GPL-3.0-only
//
// Minimal receive-only RF bring-up.
//
// Adapted from C5VRX by Twotoz and the C5VRX contributors
// (https://github.com/Twotoz/C5VRX, main/rf.c @ 14e190d, GPL-3.0-only):
// rf_start(), lock_rx_only(), enable_continuous_modem() and
// rf_reboot_to_download(). Changes: no PHY lock, filter, DC or gain-table
// handling, no lane routing (the caller routes MODEM_DIAG), configurable
// forced gain. Register addresses and private symbols are as C5VRX uses
// them with ESP-IDF 6.0.2 (pinned libphy).

#include "rf_min.h"
#include "dlboot.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define REG32(a)         (*(volatile uint32_t *)(uintptr_t)(a))
#define MAC_TXQ0_CONF    0x600a4d6cu
#define MAC_TXQ_STRIDE   0x10u
#define MAC_TXQ_ENABLE   0x80000000u
#define MAC_TXQ_COUNT    5u
// Continuous modem front end: keeps the ADC streaming into MODEM_DIAG
// without 802.11 packets (C5VRX autonomous dump-first writer).
#define DUMP_CTRL        0x600a9004u
#define DUMP_PTR_MODE    0x600a9008u
#define DUMP_FORMAT      0x600a9018u
#define FE_PATH          0x600a20b4u
#define FE_ENABLE        0x600a0800u
#define SOURCE_CTRL      0x600a08ccu
#define SOURCE_MUX       0x600a70b8u
#define MODEM_CLOCK      0x600a9c04u
#define CTRL_ENABLE      0x80000000u
#define CTRL_DUMP_FIRST  0x00020000u
#define TX_START_SELECT  0x00060000u
#define SELECTOR_MASK    0x01fe0000u
#define HP_SRAM_USAGE    0x60095004u

// The autonomous dump writer started below writes its 16384-word I/Q ring
// into SRAM at 0x40830000 for as long as it runs. That bank is kept out of
// the heap in src/dlboot.c, which every firmware links.

extern int lmac_stop_hw_txq(void);
extern void phy_wifi_fbw_sel(uint32_t val);
extern void phy_force_rx_gain(bool enable, uint8_t gain_idx);
extern void phy_disable_agc(void);
extern void phy_rfagc_disable(void);

static esp_err_t lock_rx_only(void)
{
    (void)lmac_stop_hw_txq();
    for (unsigned q = 0; q < MAC_TXQ_COUNT; ++q)
        REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) &= ~MAC_TXQ_ENABLE;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    for (unsigned q = 0; q < MAC_TXQ_COUNT; ++q)
        if (REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) & MAC_TXQ_ENABLE) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static void enable_continuous_modem(void)
{
    REG32(HP_SRAM_USAGE) = (REG32(HP_SRAM_USAGE) & 0xfffef0ffu) | 0x00010000u;
    REG32(SOURCE_CTRL) &= 0xff87ffffu;
    REG32(SOURCE_MUX) = (REG32(SOURCE_MUX) & 0xfffffff8u) | 1u;
    REG32(MODEM_CLOCK) = UINT32_MAX;
    REG32(FE_ENABLE) |= 4u;
    REG32(FE_PATH) &= ~1u;
    uint32_t v = REG32(DUMP_FORMAT);
    v = (v & 0xff03ffffu) | 0x006c0000u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xfffc0fffu) | 0x0001a000u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xfffff03fu) | 0x00000640u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xffffffc0u) | 0x18u;
    REG32(DUMP_FORMAT) = v | 0x01000000u;
    // Pre-trigger circular mode on TX_START: the MAC queues are disabled,
    // so it never fires and the writer streams continuously.
    REG32(DUMP_PTR_MODE) = (REG32(DUMP_PTR_MODE) & ~SELECTOR_MASK) | TX_START_SELECT;
    uint32_t ctrl = REG32(DUMP_CTRL);
    ctrl &= ~(CTRL_ENABLE | 0x00080000u | 0x00040000u);
    ctrl |= CTRL_DUMP_FIRST;
    ctrl = (ctrl & ~0x0001ffffu) | 16384u;
    REG32(DUMP_CTRL) = ctrl;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

esp_err_t rf_min_start(uint8_t gain)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    // sta_disconnected_pm must be off: Wi-Fi power management would gate
    // the RF/PHY clocks that keep MODEM_DIAG streaming.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.sta_disconnected_pm = false;
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    if ((err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)) != ESP_OK) return err;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) return err;
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N,
    };
    if ((err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols)) != ESP_OK) return err;
    wifi_bandwidths_t bandwidths = {.ghz_2g = WIFI_BW20, .ghz_5g = WIFI_BW40};
    if ((err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bandwidths)) != ESP_OK) return err;
    if ((err = esp_wifi_set_channel(173, WIFI_SECOND_CHAN_NONE)) != ESP_OK) return err;
    // Promiscuous RX keeps the receive path alive; a zero filter keeps the
    // LMAC from buffering packets or raising interrupts.
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) return err;
    wifi_promiscuous_filter_t filter = {.filter_mask = 0};
    (void)esp_wifi_set_promiscuous_filter(&filter);
    if ((err = lock_rx_only()) != ESP_OK) return err;
    enable_continuous_modem();
    phy_disable_agc();
    phy_rfagc_disable();
    phy_wifi_fbw_sel(1u);   // BW40 analog filter
    phy_force_rx_gain(true, gain);
    return ESP_OK;
}

void rf_min_reboot_to_download(void)
{
    // Stop the dump writer first: left running, it keeps writing SRAM into
    // the ROM loader and the loader's USB stops answering (C5VRX finding).
    REG32(DUMP_CTRL) &= ~CTRL_ENABLE;
    REG32(HP_SRAM_USAGE) &= ~0x00010f00u;
    (void)esp_wifi_stop();
    dlboot_reboot();
}
