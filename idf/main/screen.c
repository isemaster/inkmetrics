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
    } else {
        line(44, "TIME WAITING");
    }
    /* шлюз на этой же странице: если адрес и шлюз совпали — сеть настроена неверно,
       и это видно сразу (без похода в веб-кабинет) */
    line(66, "GW %s", st->gateway ? st->gateway : "-");

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
    /* Режим и СВОЙ адрес — рядом с адресом хоста и шлюзом: сразу видно, совпадает ли
       свой адрес со шлюзом (тогда сеть не работает, это и есть конфликт). */
    line(154, "NET %s", st->net_mode ? st->net_mode : "-");
    line(176, "ME %s", st->dev_ip ? st->dev_ip : "-");
}

/* ------------------------------------------- страница 4: метрики хоста (агент) */
/* Значения «н/д» (агент не прислал поле) показываем честно как N/A, а не нулём:
   ноль и «неизвестно» — разные вещи, и по экрану это должно быть видно. */
static void show_metrics(const screen_state_t *st)
{
    header("HOST SYS", 4);

    if (!st->metrics_have) {
        line(22, "NO DATA");
        line(44, "START AGENT");
        line(66, "POST /INGEST");
        line(88, "HOST %s", st->host_ip ? st->host_ip : "-");
        line(110, "ME %s", st->dev_ip ? st->dev_ip : "-");
        line(132, "FW %s", st->fw ? st->fw : "-");
        line(154, "WEB /INGEST");
        line(176, "PWR PAGE");
        return;
    }

    /* CPU и RAM в одну строку: собираем части заранее, чтобы «н/д» выглядело как N/A,
       а не как пустое место */
    char cpu_s[8], mem_s[8];
    if (st->cpu_pct >= 0) {
        snprintf(cpu_s, sizeof(cpu_s), "%.0f%%", (double)st->cpu_pct);
    } else {
        snprintf(cpu_s, sizeof(cpu_s), "N/A");
    }
    if (st->mem_pct >= 0) {
        snprintf(mem_s, sizeof(mem_s), "%.0f%%", (double)st->mem_pct);
    } else {
        snprintf(mem_s, sizeof(mem_s), "N/A");
    }
    line(22, "CPU %s RAM %s", cpu_s, mem_s);

    if (st->disk_pct >= 0) {
        line(44, "DSK %3.0f%%", (double)st->disk_pct);
    } else {
        line(44, "DSK N/A");
    }

    if (st->gpu_pct >= 0) {
        if (st->gpu_temp_c >= 0) {
            line(66, "GPU %3.0f%% %dC", (double)st->gpu_pct, st->gpu_temp_c);
        } else {
            line(66, "GPU %3.0f%%", (double)st->gpu_pct);
        }
    } else {
        line(66, "GPU N/A");
    }

    if (st->cpu_temp_c >= 0) {
        line(88, "TMP %dC SMART %s", st->cpu_temp_c,
             st->smart && st->smart[0] ? st->smart : "-");
    } else {
        line(88, "TMP N/A SMART %s", st->smart && st->smart[0] ? st->smart : "-");
    }

    if (st->hping_ok) {
        line(110, "PING %u MS", (unsigned)st->hping_ms);
    } else {
        line(110, "PING NO ANSWER");
    }

    if (st->tcp_ok) {
        line(132, "TCP %u", (unsigned)st->tcp_est);
    } else {
        line(132, "TCP N/A");
    }

    if (st->hup_ok && st->hup_h >= 0) {
        uint32_t total_min = (uint32_t)(st->hup_h * 60.0f);
        line(154, "UP %uD %02u:%02u", (unsigned)(total_min / 1440u),
             (unsigned)((total_min % 1440u) / 60u), (unsigned)(total_min % 60u));
    } else {
        line(154, "UP N/A");
    }

    /* Возраст данных: по нему сразу видно, живы ли метрики. Устаревшие помечаем. */
    if (st->metrics_fresh) {
        line(176, "AGE %u S %s", (unsigned)st->metrics_age_s,
             st->metrics_host && st->metrics_host[0] ? st->metrics_host : "");
    } else {
        line(176, "AGE %u S STALE", (unsigned)st->metrics_age_s);
    }
}

/* ------------------------------------------------------- страница 3: настройки */
static void show_settings(const screen_state_t *st)
{
    header("SETTINGS", 3);

    line(22, "ROTATE %u", (unsigned)st->rotation);
    if (st->disk_write_lock) {
        line(44, "DISK READ ONLY");
    } else {
        line(44, "DISK READ WRITE");
    }
    line(66, "PING %s", st->ping_target ? st->ping_target : "-");
    line(88, "FW %s", st->fw ? st->fw : "-");
    line(110, "BOOT ROTATE");
    line(132, "HOLD LOCK DISK");
    line(154, "PWR PAGE");
    line(176, "WEB /SETUP");
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
    case 3:
        show_settings(st);
        break;
    case 4:
        show_metrics(st);
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
