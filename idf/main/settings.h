/*
 * Настройки прибора: хранятся в NVS, переживают перезагрузку и перепрошивку.
 *
 * Что настраивается (решения пользователя 16.09):
 *   * поворот экрана — 0 / 90 / 180 / 270 (как прибор стоит на столе);
 *   * блокировка записи на диск (прибор отдаёт диску хосту и на чтение, и на запись;
 *     при включённой блокировке запись отклоняется);
 *   * цель пинга в интернете (по умолчанию ya.ru).
 *
 * Менять настройки можно с веб-страницы прибора (/setup) и кнопками с экрана настроек
 * (BOOT — поворот, BOOT удержать — блокировка диска).
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_PING_LEN 32

typedef struct {
    uint8_t rotation;          /* 0, 90, 180, 270 */
    bool    disk_write_lock;   /* true — диск только для чтения */
    char    ping_target[SETTINGS_PING_LEN];   /* имя цели пинга, например "ya.ru" */
} settings_t;

void             settings_init(void);          /* читает NVS, при первом запуске ставит умолчания */
const settings_t *settings_get(void);

void settings_set_rotation(uint8_t deg);       /* 0/90/180/270, остальное приводится к 0 */
void settings_set_disk_write_lock(bool lock);
void settings_set_ping_target(const char *name);
uint8_t settings_rotation_next(void);          /* следующий поворот по кругу, возвращает новый */

#endif /* SETTINGS_H */
