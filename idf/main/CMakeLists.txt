idf_component_register(SRCS "main.c" "diag.c" "usb_desc.c" "selfcheck.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_tinyusb esp_netif esp_http_server nvs_flash lwip esp_timer driver espcoredump spi_flash esp_partition usb)
