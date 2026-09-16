/* Описатели USB-устройства (класс ECM/RNDIS): см. usb_desc.c */
#pragma once

#include <stdint.h>

#include "tusb.h"

/* Описатель устройства (VID/PID, строки). */
extern const tusb_desc_device_t usb_desc_device;

/* Единственная конфигурация — RNDIS (её понимает Windows без своего драйвера). */
extern const uint8_t usb_desc_fs_config[];

/* Строки: 0 — язык, 1 — производитель, 2 — продукт, 3 — серийный, 4 — интерфейс. */
extern const char *usb_desc_strings[];
extern const int usb_desc_string_count;
