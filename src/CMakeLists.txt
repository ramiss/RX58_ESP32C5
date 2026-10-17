idf_component_register(
    SRCS "main.c" "dlboot.c" "rf_min.c" "padtest.c" "hop_engine.c" "hop_bench.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer spi_flash esp_wifi esp_phy nvs_flash esp_event esp_netif
                  esp_driver_parlio esp_driver_gpio esp_driver_usb_serial_jtag
                  esp_rom hal esp_hw_support esp_system
)
# The hop engine's meter runs ~8000 times a second: optimise it even in
# debug builds.
set_source_files_properties(hop_engine.c PROPERTIES COMPILE_OPTIONS "-O2")
