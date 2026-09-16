idf_component_register(SRCS "main.c" "diag.c" "usb_desc.c" "selfcheck.c"
                       "display.cpp" "screen.c" "shtc3.c" "buttons.c" "ui.c"
                       "probe.c" "timesync.c" "ping_inet.c" "settings.c" "msc.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_tinyusb esp_netif esp_http_server nvs_flash lwip esp_timer driver espcoredump spi_flash esp_partition usb epaper_driver_bsp)
