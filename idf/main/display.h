/*
 * Экран прибора (панель 1.54" 200x200) — C-интерфейс к драйверу Waveshare.
 *
 * Рисование идёт прямо в кадровый буфер панели (5000 байт: 25 Б на строку,
 * 1 = белый, 0 = чёрный), затем display_flush() выводит кадр на панель целиком
 * (~1,4 с — как в Arduino-версии).
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DISP_W 200
#define DISP_H 200

#ifdef __cplusplus
extern "C" {
#endif

/* Поднять питание панели, SPI и сам драйвер, залить пустой кадр. */
esp_err_t display_init(void);
bool      display_ready(void);

void display_clear(void);
void display_show(void);                                  /* вывести кадр на панель */
void display_px(int x, int y, bool black);
void display_hline(int x, int y, int len, bool black);
void display_rect(int x, int y, int w, int h, bool filled, bool black);
void display_text(int x, int y, const char *utf8);        /* мелкий шрифт 6x11 */
void display_text_big(int x, int y, const char *utf8);    /* крупный 12x22 */
int  display_text_advance(void);                          /* ширина одного знака (мелкий) */
int  display_text_big_advance(void);
int  display_utf8_len(const char *utf8);                  /* длина строки в знаках */

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
