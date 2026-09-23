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
 *    "gpu_count":2,
 *    "gpu0_percent":30.0,"gpu0_temp":62,"gpu0_mem_percent":41.0,
 *    "gpu1_percent":95.0,"gpu1_temp":78,"gpu1_mem_percent":88.0,
 *    "cpu_temp":55,"ping_ok":true,"ping_ms":3,"uptime_hours":12.5,
 *    "tcp_established":180,"smart_status":"OK","timestamp":"2026-09-17T14:04:05"}
 *
 * Один набор `gpu_percent`/`gpu_temp`/`gpu_mem_percent` (старый агент) читается как
 * карта 0 — так экран работает и со старой версией скрипта на хосте.
 *
 * Данные считаются свежими INGEST_STALE_S секунд: если агент замолчал дольше, прибор
 * считает связь потерянной (в рамке сводного экрана OFFLINE, метрики стираются в прочерки,
 * в /api/state поле fresh = 0).
 */
#ifndef INGEST_H
#define INGEST_H

#include <stdbool.h>
#include <stdint.h>

#define INGEST_HOST_LEN   24     /* имя хоста из данных агента */
#define INGEST_SMART_LEN  12     /* OK / WARNING / UNHEALTHY */
#define INGEST_BODY_LEN   512    /* максимум тела POST /ingest */
#define INGEST_STALE_S    90     /* дольше — связь потеряна (агент шлёт раз в 60 с).
                                    Одно значение с SCREEN_ONLINE_S в screen.h: если
                                    менять, менять в обоих, иначе веб-страница и экран
                                    будут показывать разное состояние */
#define INGEST_GPU_MAX    2      /* сколько карт принимаем и показываем */

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

    /* Видеокарты: по карте на индекс. gpu_count = 0 — карт нет (или старый агент,
       который их не прислал), тогда на экране по обоим местам прочерки. */
    int      gpu_count;
    float    gpu_pct[INGEST_GPU_MAX];     /* загрузка GPU, % */
    float    gpu_mem_pct[INGEST_GPU_MAX]; /* заполненность памяти GPU, % */
    int      gpu_temp_c[INGEST_GPU_MAX];  /* температура, °C */

    int      cpu_temp_c;                  /* температура CPU, °C */
    bool     ping_known;                  /* хост вообще проверял интернет (поле пришло) */
    int      ping_ok;                     /* 1 — хост дотянулся до цели пинга (интернет есть) */
    uint32_t ping_ms;
    uint32_t ping_got;                    /* сколько из четырёх пингов ответило (0..4) */
    bool     ping_got_known;              /* поле пришло; старый агент его не шлёт */
    char     ping_target[INGEST_HOST_LEN];/* что хост пинговал: 8.8.8.8, ya.ru и т.п. */
    float    up_h;                        /* аптайм хоста, часы */
    int32_t  tcp_est;                     /* установленных TCP-соединений; INGEST_NA — нет данных */
} ingest_state_t;

/* Разобрать тело запроса и запомнить метрики. Можно звать из задачи HTTP-сервера. */
void ingest_apply(const char *json);
/* Снимок состояния (можно звать из задачи экрана). */
void ingest_get(ingest_state_t *out);

#endif /* INGEST_H */
