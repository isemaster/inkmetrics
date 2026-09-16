/*
 * Что прибор может узнать о хосте сам, по сети, ничего не устанавливая на хост:
 *   * доступность типовых сервисов (TCP-подключение к порту);
 *   * ответ веб-сервера хоста: код, время отклика, строка Server.
 * Задача крутится раз в 30 с, порты проверяются по очереди с коротким таймаутом,
 * чтобы не задерживать сеть.
 */
#ifndef PROBE_H
#define PROBE_H

#include <stdbool.h>
#include <stdint.h>

#define PROBE_PORTS_LEN 48       /* буфер строки «22,80,135,445» */
#define PROBE_PORTS 6
typedef struct {
    uint16_t    port;
    const char *name;
    bool        open;
    uint32_t    rtt_ms;      /* время установки соединения */
} probe_svc_t;

typedef struct {
    probe_svc_t svc[PROBE_PORTS];
    bool        http_ok;         /* на порту 80 ответил HTTP */
    int         http_code;       /* код ответа (200, 301, 403 …) */
    uint32_t    http_ms;         /* время до первого байта ответа */
    char        http_server[32]; /* строка Server: из ответа (если есть) */
    uint32_t    checked_s;       /* когда была последняя проверка (секунды аптайма) */
    bool        host_known;
    bool        checked;         /* хотя бы одна проверка выполнена */
    char        ports[PROBE_PORTS_LEN];  /* ТОЛЬКО ОТКРЫТЫЕ порты через запятую: "22,80,445" */
} probe_state_t;

void probe_init(void);
void probe_set_host(const char *ip);      /* адрес хоста; "" — хост неизвестен */
void probe_get(probe_state_t *out);

#endif /* PROBE_H */
