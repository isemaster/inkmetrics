/*
 * Отрисовка экранов прибора (200x200, английский текст).
 *
 * Экран 0 — статус: адрес для входа в веб-кабинет, состояние «в сети / нет данных»,
 * аптайм, температура и влажность (SHTC3), счётчик ping, обратный отсчёт до
 * автоперехода в режим прошивки.
 * Экран 1 — сеть: адрес хоста, принятые кадры, DHCP-пакеты, запросы к серверу,
 * возраст последнего ответа, молчание хоста.
 */
#include "screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "display.h"

static void text_fmt(int x, int y, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    display_text(x, y, buf);
}

void screen_show(const screen_state_t *st, int page)
{
    display_clear();

    if (page == 1) {
        /* ---------------- экран «сеть» ---------------- */
        display_text(3, 3, "NETWORK");
        char fwline[48];
        snprintf(fwline, sizeof(fwline), "%s", st->fw ? st->fw : "");
        display_text(DISP_W - 3 - display_utf8_len(fwline) * display_text_advance(), 3, fwline);
        display_hline(0, 16, DISP_W, true);

        text_fmt(3, 22, "LINK   %s", st->link_up ? "up" : "down");
        if (st->host_known) {
            text_fmt(3, 36, "HOST   %s", st->host_ip);
        } else {
            display_text(3, 36, "HOST   not seen");
        }
        text_fmt(3, 50, "RX     %u frames", (unsigned)st->frames);
        text_fmt(3, 64, "DHCP   %u packets", (unsigned)st->dhcp_pkts);
        text_fmt(3, 78, "HTTP   %u requests", (unsigned)st->http_hits);
        if (st->ping_age_s == SCREEN_NEVER) {
            display_text(3, 92, "PING   no reply");
        } else {
            text_fmt(3, 92, "PING   %u s ago", (unsigned)st->ping_age_s);
        }
        text_fmt(3, 106, "PING   %u ok / %u lost", (unsigned)st->ping_ok, (unsigned)st->ping_fail);
        text_fmt(3, 120, "SILENT %u s", (unsigned)st->silence_s);
        display_text(3, 134, "PWR: next page");
        display_show();
        return;
    }

    /* ---------------- экран «статус» ---------------- */
    display_text(3, 3, "inkmetrics");
    const char *fw = st->fw ? st->fw : "";
    display_text(DISP_W - 3 - display_utf8_len(fw) * display_text_advance(), 3, fw);
    display_hline(0, 16, DISP_W, true);

    /* крупный статус по центру */
    const char *big = st->online ? "ONLINE" : "NO DATA";
    int big_w = display_utf8_len(big) * display_text_big_advance();
    display_text_big((DISP_W - big_w) / 2, 24, big);

    /* рамка вокруг главного статуса — видно издалека */
    display_rect(6, 20, DISP_W - 12, 34, false, true);

    text_fmt(3, 64, "OPEN  %s", st->dev_ip ? st->dev_ip : "-");
    if (st->host_known) {
        text_fmt(3, 78, "HOST  %s", st->host_ip);
    } else {
        display_text(3, 78, "HOST  not seen");
    }
    text_fmt(3, 92, "UP    %u s", (unsigned)st->up_s);
    if (st->sensor_ok) {
        text_fmt(3, 106, "TEMP  %.1f C   RH %.0f %%", (double)st->t_c, (double)st->rh);
    } else {
        display_text(3, 106, "TEMP  n/a");
    }
    text_fmt(3, 120, "PING  %u ok / %u lost", (unsigned)st->ping_ok, (unsigned)st->ping_fail);
    if (st->fallback_left_s == SCREEN_NEVER) {
        display_text(3, 134, "AUTOFLASH off");
    } else {
        text_fmt(3, 134, "AUTOFLASH in %u s", (unsigned)st->fallback_left_s);
    }
    display_hline(0, 150, DISP_W, true);
    display_text(3, 156, st->http_hits ? "web: page opened" : "web: no requests yet");
    display_text(3, 170, "PWR: next page");
    display_show();
}
