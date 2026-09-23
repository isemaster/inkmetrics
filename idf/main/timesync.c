#include "timesync.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time";

#define SNTP_RETRY_US   (60 * 1000000)   /* раз в минуту пробуем, пока не получится */
#define TIME_MIN_VALID  1600000000ULL    /* всё, что раньше 2020 года, — не время хоста */

static char s_server[16];
static volatile bool s_sntp_started;
static volatile bool s_valid;
static volatile int  s_source;           /* 0 — нет, 1 — SNTP, 2 — браузер хоста */
static volatile int  s_tz_min;           /* сдвиг от UTC в минутах (Москва = +180) */

static void sntp_synced_cb(struct timeval *tv)
{
    (void)tv;
    s_valid = true;
    s_source = 1;
    ESP_LOGI(TAG, "время получено по SNTP от хоста");
}

static void sntp_start(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_server);
    esp_sntp_set_time_sync_notification_cb(sntp_synced_cb);
    esp_sntp_init();
    s_sntp_started = true;
}

void timesync_set_host(const char *ip)
{
    if (!ip || !ip[0]) {
        return;
    }
    if (strncmp(s_server, ip, sizeof(s_server) - 1) == 0) {
        return;
    }
    snprintf(s_server, sizeof(s_server), "%s", ip);
    if (s_sntp_started) {
        esp_sntp_stop();
        s_sntp_started = false;
    }
    ESP_LOGI(TAG, "SNTP-сервер: %s (если хост не отдаёт NTP — время придёт из браузера)", s_server);
}

void timesync_set_from_host(uint64_t unix_s, int tz_minutes)
{
    if (unix_s < TIME_MIN_VALID) {
        return;
    }
    struct timeval tv = { .tv_sec = (time_t)unix_s, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_tz_min = tz_minutes;
    if (!s_valid) {
        ESP_LOGI(TAG, "время взято у браузера хоста (сдвиг %d мин)", tz_minutes);
    }
    s_valid = true;
    s_source = 2;
}

bool timesync_valid(void)
{
    return s_valid;
}

bool timesync_source(char *dst, int len)
{
    if (dst && len > 0) {
        snprintf(dst, len, "%s", s_source == 1 ? "SNTP" : (s_source == 2 ? "host" : "-"));
    }
    return s_valid;
}

/* Локальное время прибора с учётом сдвига хоста. Время собираем в struct tm один раз,
   а форматируем уже каждый своей строкой: общий форматтер с одним списком аргументов
   (%02d,%02d,%02d,%02d,%02d,%04d) не мог отдать дате день и год — дата печатала
   «час.минута.секунда» (23.09 на странице было «14.09.0032»). */
static bool now_local(struct tm *out)
{
    if (!s_valid) {
        return false;
    }
    time_t now = time(NULL);
    time_t local = now + (time_t)s_tz_min * 60;
    gmtime_r(&local, out);
    return true;
}

void timesync_time_str(char *dst, int len)
{
    struct tm tm;
    if (!dst || len <= 0) {
        return;
    }
    if (!now_local(&tm)) {
        snprintf(dst, len, "--");
        return;
    }
    snprintf(dst, len, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void timesync_date_str(char *dst, int len)
{
    struct tm tm;
    if (!dst || len <= 0) {
        return;
    }
    if (!now_local(&tm)) {
        snprintf(dst, len, "--");
        return;
    }
    snprintf(dst, len, "%02d.%02d.%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
}

static void timesync_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SNTP_RETRY_US / 1000));
        if (!s_valid && s_server[0]) {
            if (s_sntp_started) {          /* попытка не удалась — начинаем заново */
                esp_sntp_stop();
                s_sntp_started = false;
            }
            sntp_start();
        }
    }
}

void timesync_init(void)
{
    s_tz_min = 180;                        /* по умолчанию Москва (+03), уточнит браузер хоста */
    xTaskCreate(timesync_task, "timesync", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "время: SNTP с адреса хоста + подсказка от браузера (/api/host-time)");
}
