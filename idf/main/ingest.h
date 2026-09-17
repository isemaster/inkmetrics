/*
 * Метрики хоста, присланные агентом (host_metrics.ps1) на POST /ingest.
 *
 * Зачем: часть данных о ПК прибор узнать по сети не может — загрузку CPU, память,
 * заполненность диска, температуру. Их собирает скрипт на хосте и раз в минуту
 * отправляет сюда плоским JSON. Прибор разбирает его сам (свой мини-парсер, без
 * внешних зависимостей) и показывает на экране (страница HOST SYS) и на странице
 * прибора в браузере.
 *
 * Формат (все поля необязательны — чего нет, то не разберётся, и в состоянии
 * останется «н/д»):
 *   {"hostname":"PC","cpu_percent":45.0,"mem_percent":72.0,"disk_percent":68.0,
 *    "gpu_percent":30.0,"gpu_temp":62,"gpu_mem_percent":41.0,"cpu_temp":55,
 *    "ping_ok":true,"ping_ms":3,"uptime_hours":12.5,"tcp_established":180,
 *    "smart_status":"OK","timestamp":"2026-09-17T14:04:05"}
 *
 * Данные считаются свежими INGEST_STALE_S секунд: если агент замолчал, экран
 * честно показывает возраст (AGE), а не выдаёт старые цифры за текущие.
 */
#ifndef INGEST_H
#define INGEST_H

#include <stdbool.h>
#include <stdint.h>

#define INGEST_HOST_LEN   24     /* имя хоста из данных агента */
#define INGEST_SMART_LEN  12     /* OK / WARNING / UNHEALTHY */
#define INGEST_BODY_LEN   512    /* максимум тела POST /ingest */
#define INGEST_STALE_S    180    /* дольше — данные устарели (агент шлёт раз в 60 с) */

/* Значение «неизвестно» для числовых полей: агент мог не прислать поле. */
#define INGEST_NA         (-1)

typedef struct {
    bool     have;                        /* хоть раз получали данные */
    bool     fresh;                       /* возраст не больше INGEST_STALE_S */
    uint32_t age_s;                       /* сколько секунд с последнего приёма */
    uint32_t count;                       /* сколько раз принимали всего */

    char     host[INGEST_HOST_LEN];       /* как хост себя назвал */
    char     smart[INGEST_SMART_LEN];

    float    cpu_pct, mem_pct, disk_pct;  /* % загрузки; INGEST_NA — нет данных */
    float    gpu_pct, gpu_mem_pct;        /* % загрузки GPU и его памяти */
    int      gpu_temp_c, cpu_temp_c;      /* температура, °C */
    int      ping_ok;                     /* 1 — хост пингует свою цель */
    uint32_t ping_ms;
    float    up_h;                        /* аптайм хоста, часы */
    int32_t  tcp_est;                     /* установленных TCP-соединений; INGEST_NA — нет данных */
} ingest_state_t;

/* Разобрать тело запроса и запомнить метрики. Можно звать из задачи HTTP-сервера. */
void ingest_apply(const char *json);
/* Снимок состояния (можно звать из задачи экрана). */
void ingest_get(ingest_state_t *out);

#endif /* INGEST_H */
