/*
 * Пинг во внешнюю сеть (интернет) — данные для экрана прибора.
 * Цель задаётся именем (по умолчанию ya.ru, хранится в настройках) и разрешается
 * через DNS от роутера; при недоступном DNS берётся запасной IP-литерал.
 */
#ifndef PING_INET_H
#define PING_INET_H

#include <stdbool.h>
#include <stdint.h>

#define PING_INET_TARGET_LEN 32
#define PING_INET_HIST        6     /* сколько последних откликов показываем */

typedef struct {
    const char *target;                  /* как показывать цель на экране (YA.RU) */
    bool        resolved;                /* адрес получен */
    char        ip[16];                  /* адрес, по которому идёт пинг */
    bool        ok;                      /* хоть один ответ был */
    uint32_t    hist[PING_INET_HIST];    /* последние отклики, старые → новые, 0 = потеря */
    int         hist_len;
    uint32_t    loss_pct, avg_ms, min_ms, max_ms;
} ping_inet_stat_t;

void ping_inet_init(const char *name, const char *fallback_ip);
void ping_inet_set_target(const char *name, const char *fallback_ip);
void ping_inet_enable(bool on);          /* выключать, когда у прибора нет маршрута в сеть */
void ping_inet_get(ping_inet_stat_t *out);

#endif /* PING_INET_H */
