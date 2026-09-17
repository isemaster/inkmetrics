/* Заглушка вместо ESP-IDF: display.h тянет esp_err.h ради display_init(). */
#pragma once

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL (-1)
