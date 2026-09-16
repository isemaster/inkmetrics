/*
 * Время для экрана берём с хоста, без интернета:
 *   1) SNTP-запрос к самому хосту (UDP 123) — работает, если на хосте поднят
 *      NTP-сервер (Windows: служба w32time в режиме сервера; Linux: chrony/ntpd
 *      с разрешением для нашей подсети);
 *   2) подсказка от браузера хоста: страница прибора, открытая на хосте, шлёт
 *      свой момент времени на /api/host-time — это работает всегда, когда страницу
 *      открыли, и не требует настройки хоста.
 */
#ifndef TIMESYNC_H
#define TIMESYNC_H

#include <stdbool.h>
#include <stdint.h>

void timesync_init(void);
void timesync_set_host(const char *ip);                     /* сервер SNTP = адрес хоста */
void timesync_set_from_host(uint64_t unix_s, int tz_minutes); /* подсказка из браузера */
bool timesync_valid(void);
bool timesync_source(char *dst, int len);                   /* откуда время: "SNTP" / "host" / "-" */
void timesync_time_str(char *dst, int len);                 /* ЧЧ:ММ:СС по времени хоста */
void timesync_date_str(char *dst, int len);                 /* ДД.ММ.ГГГГ */

#endif /* TIMESYNC_H */
