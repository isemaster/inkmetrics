/*
 * Содержимое экрана прибора — 4 страницы, язык английский (переключатель языка
 * сделаем позже, в экране настроек; см. MEMORY.md §14).
 *
 *   0 STATUS   — состояние, адрес для входа, время хоста, климат, ping
 *   1 NETWORK  — канал: кадры/байты в секунду, DHCP, HTTP, разрывы USB
 *   2 SERVICES — что открыто на хосте (TCP-пробы) и ответ его веб-сервера
 *   3 PING     — задержки (last/min/avg/max), потери и график за 3 минуты
 */
#ifndef SCREEN_H
#define SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_PAGES   4
#define SCREEN_NEVER   0xFFFFFFFFu
#define SCREEN_SVCS    6
#define SCREEN_RTT_LEN 60

typedef struct {
    /* состояние прибора */
    const char *fw;
    bool        online;          /* хост подавал признаки за последнюю минуту */
    bool        link_up;         /* USB-стек видит хоста */
    const char *dev_ip;          /* адрес, который открывать в браузере */
    uint32_t    up_s;

    /* что знаем о хосте (пункты 1–6) */
    const char *host_ip;
    bool        host_known;
    const char *host_name;       /* DHCP опция 12 */
    bool        time_valid;
    const char *time_str;        /* ЧЧ:ММ:СС по времени хоста */
    const char *date_str;
    const char *time_src;        /* SNTP / host / - */

    /* самопроверка */
    uint32_t    silence_s;
    uint32_t    fallback_left_s; /* SCREEN_NEVER — автопереход выключен */
    uint32_t    http_hits;
    uint32_t    frames;
    uint32_t    dhcp_pkts;

    /* ping */
    uint32_t    ping_ok, ping_fail, loss_pct;
    uint32_t    rtt_last, rtt_min, rtt_avg, rtt_max;
    uint16_t    rtt_hist[SCREEN_RTT_LEN];
    int         rtt_hist_len;

    /* канал */
    uint32_t    rx_rate, tx_rate;    /* кадров в секунду */
    uint32_t    rx_kbs, tx_kbs;      /* килобайт в секунду */
    uint32_t    reconnects;

    /* сервисы хоста */
    const char *svc_name[SCREEN_SVCS];
    uint16_t    svc_port[SCREEN_SVCS];
    bool        svc_open[SCREEN_SVCS];
    bool        http_ok;
    int         http_code;
    uint32_t    http_ms;
    const char *http_server;

    /* климат */
    bool        sensor_ok;
    float       t_c;
    float       rh;
} screen_state_t;

void screen_show(const screen_state_t *st, int page);

#endif /* SCREEN_H */
