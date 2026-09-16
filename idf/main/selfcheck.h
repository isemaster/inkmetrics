/*
 * Самопроверка прибора: «заработал ли я?» и аварийный уход в режим загрузки.
 *
 * Зачем: в IDF-прошивке у платы нет COM-порта (USB занят сетью TinyUSB/RNDIS),
 * поэтому чтобы перепрошить прибор, надо было зажать BOOT и передёрнуть USB.
 * Здесь прибор сам решает, что связи с хостом нет, и уходит в загрузчик — кнопку
 * BOOT нажимать не нужно.
 *
 * Логика (требование от 16.09.2026):
 *   1. Признак «заработал» — есть признаки жизни хоста в USB-сети: ответ на ping,
 *      кадр от хоста (ARP/DHCP/что угодно ещё) или HTTP-запрос к нашему серверу.
 *   2. Если за SELFCHECK_FALLBACK_MS подряд признаков не было — уходим в загрузчик
 *      (RTC-бит FORCE_DOWNLOAD_BOOT + сброс; тот же путь, что у /api/boot).
 *   3. Как только хост подал признаки жизни — не делаем ничего.
 *
 * Выключение в релизе: SELFCHECK_ENABLED 0 в selfcheck.h (как просил пользователь:
 * «потом в финальной версии отключим»).
 */
#ifndef SELFCHECK_H
#define SELFCHECK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"

#define SELFCHECK_ENABLED          1
#define SELFCHECK_FALLBACK_MS      (5u * 60u * 1000u)   /* 5 минут молчания хоста */
#define SELFCHECK_PING_INTERVAL_MS 3000u
#define SELFCHECK_HOST_DEFAULT     "192.168.7.2"        /* первый адрес пула DHCP */

/* маркер «данных ещё не было» */
#define SELFCHECK_NEVER            0xFFFFFFFFu

/* история задержек (для графика на экране): 60 замеров по 3 с — три минуты */
#define SELFCHECK_RTT_HISTORY      60

typedef struct {
    bool     enabled;          /* самопроверка включена */
    bool     link;             /* хост подключён по USB (событие стека) */
    bool     http_up;          /* HTTP-сервер поднят */
    char     host[16];         /* адрес хоста: из ARP либо адрес по умолчанию */
    uint32_t host_known;       /* 1 — адрес узнали из ARP хоста, 0 — адрес по умолчанию */
    uint32_t silence_s;        /* сколько секунд без признаков хоста */
    uint32_t fallback_left_s;  /* сколько осталось до ухода в загрузчик (NEVER = выключено) */
    uint32_t ping_ok;          /* успешных ответов */
    uint32_t ping_fail;        /* таймаутов */
    uint32_t ping_age_s;       /* секунд с последнего ответа (NEVER = не отвечал) */
    uint32_t http_hits;        /* запросов от хоста к веб-серверу */
    uint32_t host_frames;      /* кадров от хоста всего */
    uint32_t dhcp_frames;      /* из них DHCP (хост запрашивает адрес) */
} selfcheck_status_t;

void selfcheck_init(void);
void selfcheck_start(esp_netif_t *netif);          /* запускает фоновую задачу проверки */
void selfcheck_http_up(bool up);
void selfcheck_set_link(bool up);
void selfcheck_http_hit(void);                     /* вызывать из обработчиков HTTP */
void selfcheck_host_frame(const uint8_t *frame, uint16_t len);  /* вызывать из приёма USB-кадров */
typedef struct {
    uint32_t last_ms;      /* последний ответ */
    uint32_t min_ms;
    uint32_t avg_ms;
    uint32_t max_ms;
    uint32_t loss_pct;     /* потери, % */
} selfcheck_ping_stat_t;

void selfcheck_status(selfcheck_status_t *out);
/* Задержки ping: last/min/avg/max и потери (для экрана и веб-страницы). */
void selfcheck_ping_stats(selfcheck_ping_stat_t *out);
/* История последних замеров, старые → новые. Возвращает число записей. */
int  selfcheck_rtt_history(uint16_t *dst, int max);
/* Имя хоста из DHCP-опции 12 (как хост себя называет); "" — ещё не видели. */
const char *selfcheck_host_name(void);
/* Хост сам назвал себя: адрес взят из запроса к нашей странице (браузер на хосте).
   Нужно в режиме моста, когда DHCP-обмена с хостом нет и ARP-подсказки не хватает. */
void selfcheck_set_host(const char *ip);
void selfcheck_enter_download_mode(const char *why);

#endif /* SELFCHECK_H */
