idf_component_register(
    SRCS "main.c" "dlboot.c" "rf_min.c" "padtest.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer spi_flash esp_wifi esp_phy nvs_flash esp_event esp_netif
                  esp_driver_parlio esp_driver_gpio esp_driver_usb_serial_jtag
                  esp_rom hal esp_hw_support esp_system
)
