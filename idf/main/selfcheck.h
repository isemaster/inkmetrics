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
void selfcheck_status(selfcheck_status_t *out);
void selfcheck_enter_download_mode(const char *why);

#endif /* SELFCHECK_H */
