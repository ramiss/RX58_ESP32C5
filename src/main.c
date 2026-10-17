// ESP32_SDR_RX5808 -- drop-in RX5808 replacement on a XIAO ESP32-C5.
//
// Base scaffold: proves the toolchain, board, flash mode and USB console.
// The receiver (continuous I/Q capture -> fixed-gain block power -> RSSI
// output, plus the RX5808 3-wire SPI slave) is built on top of this.

#include <inttypes.h>
#include <stdio.h>

#include "dlboot.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef SDR_PADTEST
#include "padtest.h"
#endif

static const char *TAG = "rx5808";

void app_main(void)
{
#ifdef SDR_PADTEST
    // Phase B step 1, built by `.\tools\pio.ps1 run -e padtest`.
    padtest_main();
#endif
    // Console key B reboots to the ROM loader (used by every upload).
    dlboot_start_listener();

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);

    ESP_LOGI(TAG, "ESP32_SDR_RX5808 scaffold: %s rev v%d.%d, %d core(s), %" PRIu32 " MB flash",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores,
             flash_bytes / (1024 * 1024));

    for (;;) {
        ESP_LOGI(TAG, "alive %" PRId64 " ms", esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
