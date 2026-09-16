/*
 * Содержимое экрана прибора. Язык интерфейса — английский (решение 16.09:
 * переключатель языка сделаем позже, в экране настроек).
 */
#ifndef SCREEN_H
#define SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_PAGES 2
#define SCREEN_NEVER 0xFFFFFFFFu

typedef struct {
    const char *fw;
    bool        online;          /* хост отвечал недавно (признак жизни) */
    bool        link_up;         /* USB-стек видит хоста */
    const char *dev_ip;          /* адрес, который открывать в браузере */
    const char *host_ip;
    bool        host_known;
    uint32_t    up_s;
    uint32_t    ping_ok;
    uint32_t    ping_fail;
    uint32_t    ping_age_s;      /* SCREEN_NEVER — ответа не было */
    uint32_t    http_hits;
    uint32_t    frames;
    uint32_t    dhcp_pkts;
    uint32_t    silence_s;
    uint32_t    fallback_left_s; /* SCREEN_NEVER — автопереход выключен */
    bool        sensor_ok;
    float       t_c;
    float       rh;
} screen_state_t;

void screen_show(const screen_state_t *st, int page);

#endif /* SCREEN_H */
