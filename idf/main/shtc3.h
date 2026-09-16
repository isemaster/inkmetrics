/*
 * SHTC3 (датчик температуры и влажности на плате, I2C 0x70) — для IDF.
 * Команды и формулы взяты из Arduino-версии (firmware/inkmetrics) — там же проверены.
 */
#ifndef SHTC3_H
#define SHTC3_H

#include <stdbool.h>
#include "esp_err.h"

#define SHTC3_SDA_GPIO 47
#define SHTC3_SCL_GPIO 48

esp_err_t shtc3_init(void);
/* true — данные прочитаны; t_c в °C, rh в %. */
bool shtc3_read(float *t_c, float *rh);

#endif /* SHTC3_H */
