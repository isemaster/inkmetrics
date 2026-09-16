/*
 * Состояние сети, каким его видит остальная прошивка: кто мы и откуда взялся адрес.
 *
 * Два режима (см. docs/addressing.md, вариант A):
 *   ROUTER    — прибор подключён к локальной сети как обычный клиент: адрес, шлюз и
 *               DNS выдал роутер (запросы уходят через мост Windows на ПК);
 *   EMERGENCY — адрес от роутера не получен (нет сети, ПК стоит отдельно): прибор
 *               берёт статический 192.168.7.1 и поднимает DHCP-сервер для хоста,
 *               чтобы остаться доступным.
 * Реализация — в main.c (там живёт esp_netif).
 */
#ifndef NETINFO_H
#define NETINFO_H

#include <stdbool.h>

typedef enum {
    NET_MODE_NONE = 0,     /* адреса ещё нет (ждём DHCP) */
    NET_MODE_ROUTER,       /* адрес от роутера локальной сети */
    NET_MODE_EMERGENCY,    /* аварийный: 192.168.7.1 + DHCP-сервер для хоста */
} net_mode_t;

net_mode_t  net_mode(void);
bool        net_from_router(void);      /* есть маршрут в сеть (можно пинговать интернет) */
const char *net_mode_text(void);        /* "FROM ROUTER" / "EMERGENCY" / "NO ADDR" */
const char *net_ip_str(void);           /* адрес прибора (для входа в браузер) */
const char *net_gw_str(void);           /* шлюз, если известен, иначе "-" */

#endif /* NETINFO_H */
