/*
 * Отрисовка экранов прибора (200x200, английский текст) — четыре страницы:
 * статус, сеть, сервисы хоста, задержки с графиком.
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

static void header(const char *title, const char *fw)
{
    display_text(3, 3, title);
    if (fw && fw[0]) {
        int w = display_utf8_len(fw) * display_text_advance();
        display_text(DISP_W - 3 - w, 3, fw);
    }
    display_hline(0, 16, DISP_W, true);
}

static void footer(int page)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "PWR: page %d/%d", page, SCREEN_PAGES);
    display_text(3, DISP_H - 13, buf);
}

/* --------------------------------------------------------------- экран 0: статус */
static void show_status(const screen_state_t *st)
{
    header("inkmetrics", st->fw);

    const char *big = st->online ? "ONLINE" : "NO DATA";
    int big_w = display_utf8_len(big) * display_text_big_advance();
    display_text_big((DISP_W - big_w) / 2, 24, big);
    display_rect(6, 20, DISP_W - 12, 34, false, true);

    text_fmt(3, 60, "OPEN  %s", st->dev_ip ? st->dev_ip : "-");
    if (st->host_known && st->host_name && st->host_name[0]) {
        text_fmt(3, 72, "HOST  %s", st->host_name);
    } else if (st->host_known) {
        text_fmt(3, 72, "HOST  %s", st->host_ip);
    } else {
        display_text(3, 72, "HOST  not seen");
    }
    text_fmt(3, 84, "UP    %u s", (unsigned)st->up_s);
    if (st->time_valid) {
        text_fmt(3, 96, "TIME  %s %s", st->time_str, st->time_src);
    } else {
        display_text(3, 96, "TIME  waiting host");
    }
    if (st->sensor_ok) {
        text_fmt(3, 108, "TEMP  %.1f C   RH %.0f %%", (double)st->t_c, (double)st->rh);
    } else {
        display_text(3, 108, "TEMP  n/a");
    }
    text_fmt(3, 120, "PING  %u ok / %u lost", (unsigned)st->ping_ok, (unsigned)st->ping_fail);
    if (st->fallback_left_s == SCREEN_NEVER) {
        display_text(3, 132, "AUTOFLASH off");
    } else {
        text_fmt(3, 132, "AUTOFLASH in %u s", (unsigned)st->fallback_left_s);
    }
    display_hline(0, 146, DISP_W, true);
    if (st->link_up) {
        text_fmt(3, 152, "usb link up   %s", st->http_hits ? "web seen" : "no web yet");
    } else {
        display_text(3, 152, "usb link down");
    }
    display_text(3, 164, st->date_str && st->time_valid ? st->date_str : "--");
    footer(0);
}

/* ---------------------------------------------------------------- экран 1: сеть */
static void show_network(const screen_state_t *st)
{
    header("NETWORK", st->fw);

    text_fmt(3, 22, "LINK   %s (lost %u)", st->link_up ? "up" : "down",
             (unsigned)st->reconnects);
    if (st->host_known) {
        if (st->host_name && st->host_name[0]) {
            text_fmt(3, 36, "HOST   %s", st->host_name);
            text_fmt(3, 50, "IP     %s", st->host_ip);
        } else {
            text_fmt(3, 36, "HOST   %s", st->host_ip);
        }
    } else {
        display_text(3, 36, "HOST   not seen");
    }
    text_fmt(3, 64, "RX     %u frm  %u/s  %u KB/s",
             (unsigned)st->frames, (unsigned)st->rx_rate, (unsigned)st->rx_kbs);
    text_fmt(3, 78, "TX     %u frm  %u/s  %u KB/s",
             0u, (unsigned)st->tx_rate, (unsigned)st->tx_kbs);
    text_fmt(3, 92, "DHCP   %u pkts", (unsigned)st->dhcp_pkts);
    text_fmt(3, 106, "HTTP   %u requests", (unsigned)st->http_hits);
    text_fmt(3, 120, "SILENT %u s", (unsigned)st->silence_s);
    display_hline(0, 136, DISP_W, true);
    display_text(3, 142, "watchdog: autoflash");
    if (st->fallback_left_s == SCREEN_NEVER) {
        display_text(3, 154, "state off");
    } else {
        text_fmt(3, 154, "in %u s of silence", (unsigned)st->fallback_left_s);
    }
    footer(1);
}

/* ------------------------------------------------------------ экран 2: сервисы */
static void show_services(const screen_state_t *st)
{
    header("SERVICES", st->fw);

    for (int i = 0; i < SCREEN_SVCS; i++) {
        int y = 22 + i * 13;
        const char *name = st->svc_name[i] ? st->svc_name[i] : "?";
        text_fmt(3, y, "%-5s %-5u %s", name, (unsigned)st->svc_port[i],
                 st->svc_open[i] ? "open" : "-");
    }
    display_hline(0, 104, DISP_W, true);
    if (st->http_ok) {
        text_fmt(3, 110, "WEB reply %d in %u ms", st->http_code, (unsigned)st->http_ms);
        if (st->http_server && st->http_server[0]) {
            text_fmt(3, 122, "server %s", st->http_server);
        } else {
            display_text(3, 122, "server unknown");
        }
    } else {
        display_text(3, 110, "no HTTP answer from host");
        display_text(3, 122, "(port 80 closed or filtered)");
    }
    display_hline(0, 136, DISP_W, true);
    display_text(3, 142, "TCP probes, ok = connect");
    display_text(3, 154, "timeout 400 ms each");
    footer(2);
}

/* ------------------------------------------------------------- экран 3: задержки */
static void show_ping(const screen_state_t *st)
{
    header("PING", st->fw);

    text_fmt(3, 22, "last   %u ms", (unsigned)st->rtt_last);
    text_fmt(3, 34, "min    %u ms", (unsigned)st->rtt_min);
    text_fmt(3, 46, "avg    %u ms", (unsigned)st->rtt_avg);
    text_fmt(3, 58, "max    %u ms", (unsigned)st->rtt_max);
    text_fmt(3, 70, "loss   %u %%  (%u ok / %u lost)",
             (unsigned)st->loss_pct, (unsigned)st->ping_ok, (unsigned)st->ping_fail);

    /* график: столбик на каждый замер, масштаб по максимуму (не меньше 10 мс) */
    const int gx = 3, gy = 88, gw = DISP_W - 6, gh = 60;
    display_rect(gx - 1, gy - 1, gw + 2, gh + 2, false, true);
    uint32_t scale = st->rtt_max > 10 ? st->rtt_max : 10;
    int n = st->rtt_hist_len;
    if (n > 0) {
        int step = gw / (n > gw ? gw : n);
        if (step < 1) {
            step = 1;
        }
        for (int i = 0; i < n; i++) {
            uint32_t v = st->rtt_hist[i];
            int h = (int)((v * (uint32_t)gh) / scale);
            if (h > gh) {
                h = gh;
            }
            int x = gx + i * step;
            if (h > 0) {
                for (int j = 0; j < h; j++) {
                    display_px(x, gy + gh - 1 - j, true);
                }
            } else {
                display_px(x, gy + gh - 1, true);      /* потеря — отметка на нуле */
            }
        }
    } else {
        display_text(gx + 4, gy + gh / 2, "no replies yet");
    }
    text_fmt(3, 154, "scale 0..%u ms", (unsigned)scale);
    footer(3);
}

void screen_show(const screen_state_t *st, int page)
{
    display_clear();
    switch (page) {
    case 1:
        show_network(st);
        break;
    case 2:
        show_services(st);
        break;
    case 3:
        show_ping(st);
        break;
    default:
        show_status(st);
        break;
    }
    display_show();
}
