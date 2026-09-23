/*
 * Настройки прибора: хранятся в NVS, переживают перезагрузку и перепрошивку.
 *
 * Что настраивается (решения пользователя 16.09):
 *   * поворот экрана — 0 / 90 / 180 / 270 (как прибор стоит на столе);
 *   * блокировка записи на диск (прибор отдаёт диску хосту и на чтение, и на запись;
 *     при включённой блокировке запись отклоняется);
 *   * цель пинга в интернете (по умолчанию ya.ru);
 *   * два крупных числа сводного экрана (slot1, slot2) — что именно показывать:
 *     на разных ПК доступны разные данные, поэтому выбор отдан пользователю.
 *     Умолчания: CPU % и вторая карта (если карта одна — первая).
 *
 * Менять настройки можно с веб-страницы прибора (/setup) и кнопками с экрана настроек
 * (BOOT — поворот, BOOT удержать — блокировка диска).
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_PING_LEN 32

/* Что можно вывести в крупное число сводного экрана. Ноль — слот выключен.
   Порядок значений менять нельзя: это то, что лежит в NVS. */
typedef enum {
    SLOT_OFF = 0,          /* пусто */
    SLOT_CPU_PCT,          /* загрузка процессора хоста */
    SLOT_RAM_PCT,          /* память */
    SLOT_DISK_PCT,         /* диск C: */
    SLOT_GPU0_PCT,         /* загрузка карты 0 */
    SLOT_GPU1_PCT,         /* загрузка карты 1 */
    SLOT_GPU_LAST_PCT,     /* загрузка второй карты, а если она одна — первой */
    SLOT_GPU0_TEMP,        /* температура карты 0, °C */
    SLOT_GPU1_TEMP,        /* температура карты 1, °C */
    SLOT_KIND_MAX
} slot_kind_t;

#define SETTINGS_SLOT_COUNT 2

typedef struct {
    uint16_t rotation;         /* 0, 90, 180, 270 (в uint8_t 270 не помещается — было так,
                                  и поворот 270° не сохранялся: 270 → 14 → 0) */
    bool    disk_write_lock;   /* true — диск только для чтения */
    char    ping_target[SETTINGS_PING_LEN];   /* имя цели пинга, например "ya.ru" */
    uint8_t slot[SETTINGS_SLOT_COUNT];        /* slot_kind_t для двух крупных чисел */
} settings_t;

/* Два крупных числа сводного экрана выбирает пользователь: на разных ПК доступны
   разные данные (где-то есть температуры карт, где-то нет видеокарты вовсе). */
uint8_t     settings_slot(uint8_t idx);        /* idx: 0 или 1 */
void        settings_set_slot(uint8_t idx, uint8_t kind);
const char *settings_slot_name(uint8_t kind);  /* подпись в списке на веб-странице */


void             settings_init(void);          /* читает NVS, при первом запуске ставит умолчания */
const settings_t *settings_get(void);

void settings_set_rotation(uint16_t deg);      /* 0/90/180/270, остальное приводится к 0 */
void settings_set_disk_write_lock(bool lock);
void settings_set_ping_target(const char *name);
uint16_t settings_rotation_next(void);         /* следующий поворот по кругу, возвращает новый */

#endif /* SETTINGS_H */
