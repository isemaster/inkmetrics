/*
 * Содержимое экрана прибора — ТРИ страницы, только крупным шрифтом 12x22.
 *
 * Сетка, из которой всё считается: 200 / 12 = 16 знаков в строке,
 * 200 / 22 = 9 строк. Мелкого шрифта на страницах нет (требование пользователя
 * 16.09: «шрифт как надпись ONLINE, меньше не надо»).
 *
 * Строчных букв в крупном шрифте нет, поэтому весь текст перед выводом
 * приводится к верхнему регистру (см. upper_utf8() в screen.c).
 *
 *   0 DEVICE — состояние прибора: статус, время, датчик, пинг в интернет, адрес
 *   1 PING   — пинг во внешнюю сеть: последние ответы столбиком, потери, min/max
 *   2 HOST   — что видно о хосте: имя, адрес, ОТКРЫТЫЕ порты одной строкой, веб-ответ
 */
#ifndef SCREEN_H
#define SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_PAGES      3
#define SCREEN_NEVER      0xFFFFFFFFu
#define SCREEN_PING_LINES 6      /* сколько последних ответов показываем столбиком */
#define SCREEN_PORTS_LEN  48     /* буфер строки с открытыми портами */

typedef struct {
    /* прибор и сеть */
    const char *dev_ip;          /* адрес прибора (что открывать в браузере) */
    const char *net_mode;        /* "FROM ROUTER" / "EMERGENCY" / "NO ADDR" */
    const char *gateway;         /* шлюз, если известен */
    bool        net_router;      /* адрес получен от роутера (а не аварийный) */
    uint32_t    up_s;            /* аптайм прибора, с */

    /* время */
    bool        time_valid;
    const char *time_str;        /* ЧЧ:ММ:СС */
    const char *date_str;        /* ДД.ММ.ГГГГ */

    /* датчик на плате */
    bool        sensor_ok;
    float       t_c;
    float       rh;

    /* пинг в интернет */
    const char *ping_target;                       /* имя цели, например YA.RU */
    bool        ping_ok;                           /* хоть один ответ был */
    uint32_t    ping_last[SCREEN_PING_LINES];      /* последние замеры, старые → новые */
    int         ping_count;                        /* сколько записей заполнено */
    uint32_t    ping_loss_pct, ping_avg, ping_min, ping_max;

    /* хост */
    bool        host_known;
    const char *host_name;
    const char *host_ip;
    bool        ports_known;                       /* проверка портов выполнялась */
    char        ports[SCREEN_PORTS_LEN];           /* только открытые: "22,80,445" */
    bool        web_ok;
    int         web_code;
    uint32_t    web_ms;

    /* аварийное состояние: инверсный вид + полное обновление */
    bool        emergency;
} screen_state_t;

void screen_show(const screen_state_t *st, int page);

#endif /* SCREEN_H */
