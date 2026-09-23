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

/* Поднять питание панели, SPI и сам драйвер. Кадр не заливается: панель остаётся
   в том состоянии, в каком была, первый кадр рисует задача экрана сразу (без
   белого поля, которое раньше висело 10–15 с). */
esp_err_t display_init(void);
bool      display_ready(void);

void display_clear(void);
/* Вывести кадр на панель.
     force_full = false — частичное обновление (быстро, без мигания и инверсии);
     force_full = true  — полное (с инверсией, ~1,4 с): аварийный вид экрана.
   Полное обновление само включается каждое DISP_FULL_EVERY-е обновление, чтобы
   стирать «чернила»/призраки от частичных. */
void display_show(bool force_full);
/* Инвертировать весь кадровый буфер (белое↔чёрное) — аварийный вид. */
void display_invert(void);
/* Следующий вывод — обязательно полное обновление: стирает «чернила» и призраков
   от частичных. Так работает принудительная перерисовка кнопкой (PWR удержать). */
void display_force_full(void);
/* Поворот кадра: 0 / 90 / 180 / 270 градусов (настройка прибора). */
void display_set_rotation(uint16_t deg);
void display_px(int x, int y, bool black);
void display_hline(int x, int y, int len, bool black);
void display_rect(int x, int y, int w, int h, bool filled, bool black);

/* Шрифты. Индексы — те же, что в fonts.h (генерирует tools/make_font.py):
     0 small  6x11  — подписи, адреса, нижние строки (чернила 6);
     1 mid    18x30 — проценты CPU/RAM/DISK (чернила 19);
     2 big    12x22 — заголовок SETUP, аптайм, значения строк SETUP (чернила 14);
     3 huge   36x44 — крупные числа сводного экрана (чернила 41);
     4 ping   18x32 — слово состояния хоста: в 1.5 раза крупнее big (чернила 21);
     5 txt9    9x16 — задержка и счёт пингов: в 1.5 раза крупнее small (чернила 9).
   y задаёт верх ЧЕРНИЛ, а не верха клетки шрифта: внутри клетки есть пустые строки,
   их высоту знает генератор шрифтов (FONT_*_INK_TOP), и без этой поправки раскладка
   «плывёт» на 2—3 px. */
#define DISP_F_SMALL 0
#define DISP_F_MID   1
#define DISP_F_BIG   2
#define DISP_F_HUGE  3
#define DISP_F_PING  4    /* 18x32: слово состояния в строке пинга */
#define DISP_F_TXT9  5    /* 9x16:  задержка и счёт пингов */
#define DISP_FONTS   6

void display_text_f(int font, int x, int y, const char *utf8);
void display_text_center(int font, int y, const char *utf8);          /* по центру экрана */
void display_text_center_x(int font, int x0, int x1, int y, const char *utf8);
void display_text_right(int font, int y, const char *utf8);           /* по правому краю */
int  display_text_w(int font, const char *utf8);                      /* ширина строки, px */
int  display_text_h(int font);                                        /* высота чернил, px */
int  display_text_adv(int font);                                      /* шаг знака, px */

/* Старые вызовы: мелкий шрифт и «крупный» (12x22) — оставлены для совместимости. */
void display_text(int x, int y, const char *utf8);
void display_text_big(int x, int y, const char *utf8);
int  display_text_advance(void);
int  display_text_big_advance(void);
int  display_utf8_len(const char *utf8);                  /* длина строки в знаках */

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
