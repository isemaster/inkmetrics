/*
 * Отрисовка экранов прибора (200x200) — три страницы, ТОЛЬКО крупный шрифт 12x22.
 *
 * Сетка: 16 знаков в строке, 9 строк (y = 0, 22, 44, … 176).
 * Строчных букв в крупном шрифте нет → текст приводим к верхнему регистру.
 * Аварийное состояние (нет адреса/нет хоста) рисуется инверсией и полным
 * обновлением: мигание и инверсия — признак аварии, а не штатной работы.
 */
#include "screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "display.h"

#define LINE_H  22                       /* высота строки крупного шрифта */
#define ADV     display_text_big_advance()

/* ------------------------------------------------------------------ утилиты */

/* Привести строку к верхнему регистру: в крупном шрифте строчных букв нет.
   Латиница (ASCII) и кириллица а-я / ё. */
static void upper_utf8(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        if (p[0] < 0x80) {
            if (p[0] >= 'a' && p[0] <= 'z') {
                p[0] = (unsigned char)(p[0] - 0x20);
            }
            p++;
        } else if ((p[0] & 0xE0) == 0xC0) {
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
            uint16_t cp = (uint16_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
            if (cp >= 0x430 && cp <= 0x44F) {
                cp = (uint16_t)(cp - 0x20);           /* а-я → А-Я */
            } else if (cp == 0x451) {
                cp = 0x401;                           /* ё → Ё */
            }
            p[0] = (uint8_t)(0xE0 | (cp >> 12));
            p[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            p[2] = (uint8_t)(0x80 | (cp & 0x3F));
            p += 3;
        } else {
            p++;
        }
    }
}

/* Одна строка крупным шрифтом: printf-формат → верхний регистр → вывод. */
static void line(int y, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    upper_utf8(buf);
    display_text_big(1, y, buf);
}

/* Готовая строка (уже сформированная) крупным шрифтом с заданным отступом. */
static void line_at(int x, int y, const char *text)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", text);
    upper_utf8(buf);
    display_text_big(x, y, buf);
}

/* Заголовок страницы: название слева, номер страницы справа. */
static void header(const char *title, int page)
{
    char num[20];
    snprintf(num, sizeof(num), "%d/%d", page + 1, (int)SCREEN_PAGES);
    display_text_big(1, 0, title);
    display_text_big(DISP_W - (int)(strlen(num) * ADV) - 1, 0, num);
}

/* --------------------------------------------------------- страница 0: прибор */
static void show_device(const screen_state_t *st)
{
    header("DEVICE", 0);

    const char *big = "CHECKING";
    if (st->emergency) {
        big = "NO NET";                       /* аварийный вид: инверсия + полное обновление */
    } else if (st->net_router) {
        big = "ONLINE";
    }
    display_rect(0, 20, DISP_W, 26, false, true);
    int w = (int)(strlen(big) * ADV);
    display_text_big((DISP_W - w) / 2, 22, big);

    if (st->time_valid) {
        line(44, "TIME %s", st->time_str);
        line(66, "DATE %s", st->date_str);
    } else {
        line(44, "TIME WAITING");
        line(66, "DATE --");
    }

    if (st->sensor_ok) {
        line(88, "TEMP %.1fC RH %.0f%%", (double)st->t_c, (double)st->rh);
    } else {
        line(88, "TEMP N/A");
    }

    if (st->ping_ok && st->ping_count > 0) {
        line(110, "NET %u MS %s", (unsigned)st->ping_last[st->ping_count - 1],
             st->ping_target ? st->ping_target : "");
    } else {
        line(110, "NET NO ANSWER");
    }

    line(132, "ADDR %s", st->dev_ip ? st->dev_ip : "-");
    line(154, "NET %s", st->net_mode ? st->net_mode : "-");
    line(176, "UP %uD %02u:%02u", (unsigned)(st->up_s / 86400u),
         (unsigned)((st->up_s % 86400u) / 3600u), (unsigned)((st->up_s % 3600u) / 60u));
}

/* ----------------------------------------------------------- страница 1: пинг */
static void show_ping(const screen_state_t *st)
{
    char title[24];
    snprintf(title, sizeof(title), "PING %s", st->ping_target ? st->ping_target : "");
    header(title, 1);

    /* последние ответы столбиком — как в консоли ping */
    for (int i = 0; i < SCREEN_PING_LINES; i++) {
        int y = LINE_H + i * LINE_H;                   /* 22 … 132 */
        if (i < st->ping_count) {
            line(y, "%u MS", (unsigned)st->ping_last[i]);
        } else {
            line(y, "--");
        }
    }

    line(154, "LOSS %u%% AVG %u MS", (unsigned)st->ping_loss_pct, (unsigned)st->ping_avg);
    line(176, "MIN %u MAX %u MS", (unsigned)st->ping_min, (unsigned)st->ping_max);
}

/* ----------------------------------------------------------- страница 2: хост */
static void show_host(const screen_state_t *st)
{
    header("HOST", 2);

    if (st->host_known) {
        line(22, "%s", st->host_name && st->host_name[0] ? st->host_name : st->host_ip);
        line(44, "IP %s", st->host_ip ? st->host_ip : "-");
    } else {
        line(22, "HOST NOT SEEN");
        line(44, "IP --");
    }

    /* порты: только открытые, через запятую, максимум двумя строками */
    if (st->ports_known && st->ports[0]) {
        char buf[SCREEN_PORTS_LEN + 8];
        char first[20];
        snprintf(buf, sizeof(buf), "PORTS %s", st->ports);
        snprintf(first, sizeof(first), "%.16s", buf);
        line_at(1, 66, first);
        if (strlen(buf) > 16) {
            line_at(1, 88, buf + 16);
        }
    } else if (st->ports_known) {
        line(66, "NO OPEN PORTS");
    } else {
        line(66, "PORTS CHECKING");
    }

    if (st->web_ok) {
        line(110, "WEB %d %u MS", st->web_code, (unsigned)st->web_ms);
    } else {
        line(110, "WEB NO ANSWER");
    }

    line(132, "GW %s", st->gateway ? st->gateway : "-");
    line(176, "HOST SEEN BY WEB");
}

void screen_show(const screen_state_t *st, int page)
{
    display_clear();
    switch (page) {
    case 1:
        show_ping(st);
        break;
    case 2:
        show_host(st);
        break;
    default:
        show_device(st);
        break;
    }
    if (st->emergency) {
        display_invert();          /* авария: инверсный вид + полное обновление */
    }
    display_show(st->emergency);
}
