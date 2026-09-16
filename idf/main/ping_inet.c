/*
 * Пинг во внешнюю сеть (интернет) — то, что показывается на экране прибора.
 *
 * Зачем отдельно от самопроверки: самопроверка (selfcheck.c) пингует ХОСТ, чтобы
 * понять «жив ли тот, кто нас воткнул» и уйти в загрузчик, если нет. Для человека
 * же интересен интернет: «есть ли сеть вообще». Адрес цели задаётся именем
 * (по умолчанию `ya.ru`) и разрешается через DNS, который прибор получает от
 * роутера по DHCP; если DNS недоступен — берётся запасной IP-литерал.
 *
 * Храним последние PING_INET_HIST откликов (старые → новые) — экран выводит их
 * столбиком, как консольный ping.
 */
#include "ping_inet.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

#define PING_PERIOD_MS   1500     /* между проверками цели */
#define PING_INTERVAL_MS 1000     /* между запросами внутри сессии */
#define RESOLVE_RETRY_MS 60000    /* как часто пробовать разрешить имя заново */

static const char *TAG = "pingin";

static char     s_name[PING_INET_TARGET_LEN] = "ya.ru";   /* имя цели (как в вопросе) */
static char     s_label[PING_INET_TARGET_LEN] = "YA.RU";  /* как показывать на экране */
static char     s_fallback[16] = "77.88.55.242";          /* запасной адрес, если DNS молчит */
static char     s_ip[16] = "";
static volatile bool s_resolved;
static volatile bool s_enabled;                           /* есть куда пинговать (адрес+шлюз) */

static esp_ping_handle_t s_ping;
static volatile int64_t s_last_resolve_us;
static volatile int64_t s_last_reply_us;

static volatile uint32_t s_ok, s_fail;
static volatile uint32_t s_hist[PING_INET_HIST];          /* последние отклики, старые → новые */
static volatile int      s_hist_len;
static volatile uint32_t s_min_ms, s_max_ms;

static void push_hist(uint32_t ms)
{
    if (s_hist_len < PING_INET_HIST) {
        s_hist[s_hist_len++] = ms;
    } else {
        for (int i = 1; i < PING_INET_HIST; i++) {
            s_hist[i - 1] = s_hist[i];
        }
        s_hist[PING_INET_HIST - 1] = ms;
    }
}

static void ping_ok_cb(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t ms = 0;
    if (esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms)) != ESP_OK) {
        return;
    }
    s_ok++;
    s_last_reply_us = esp_timer_get_time();
    push_hist(ms);
    if (!s_min_ms || ms < s_min_ms) {
        s_min_ms = ms;
    }
    if (ms > s_max_ms) {
        s_max_ms = ms;
    }
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
    s_fail++;
    push_hist(0);              /* потеря — нулевая отметка (на экране будет 0 MS) */
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
}

static void ping_stop(void)
{
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
}

static bool ping_start(const char *ip)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    ping_stop();
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PING_INTERVAL_MS;
    cfg.timeout_ms = 1000;
    cfg.data_size = 32;
    cfg.task_stack_size = 3072;
    cfg.task_prio = 3;
    IP_ADDR4(&cfg.target_addr, a, b, c, d);

    esp_ping_callbacks_t cbs = {
        .cb_args = NULL,
        .on_ping_success = ping_ok_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
    };
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) != ESP_OK) {
        s_ping = NULL;
        ESP_LOGW(TAG, "не удалось поднять ping на %s", ip);
        return false;
    }
    esp_ping_start(s_ping);
    snprintf(s_ip, sizeof(s_ip), "%s", ip);
    ESP_LOGI(TAG, "пингую %s (%s)", s_label, ip);
    return true;
}

/* -------------------------------------------------------------------- API */

void ping_inet_set_target(const char *name, const char *fallback_ip)
{
    if (name && name[0]) {
        snprintf(s_name, sizeof(s_name), "%s", name);
        snprintf(s_label, sizeof(s_label), "%s", name);
        for (char *p = s_label; *p; p++) {         /* на экране — заглавными */
            if (*p >= 'a' && *p <= 'z') {
                *p = (char)(*p - 0x20);
            }
        }
    }
    if (fallback_ip && fallback_ip[0]) {
        snprintf(s_fallback, sizeof(s_fallback), "%s", fallback_ip);
    }
    s_resolved = false;
    s_last_resolve_us = 0;
    s_ok = s_fail = s_hist_len = 0;
    s_min_ms = s_max_ms = 0;
    ping_stop();
}

void ping_inet_enable(bool on)
{
    s_enabled = on;
    if (!on) {
        ping_stop();
        s_resolved = false;
    }
}

static const char *ping_inet_label(void)
{
    return s_label;
}

/* Разрешить имя цели в адрес; при неудаче — запасной литерал. */
static bool resolve_target(void)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    s_last_resolve_us = esp_timer_get_time();
    if (getaddrinfo(s_name, NULL, &hints, &res) == 0 && res && res->ai_addr) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        char ip[16];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                 (unsigned)(ntohl(sa->sin_addr.s_addr) >> 24) & 0xFF,
                 (unsigned)(ntohl(sa->sin_addr.s_addr) >> 16) & 0xFF,
                 (unsigned)(ntohl(sa->sin_addr.s_addr) >> 8) & 0xFF,
                 (unsigned)ntohl(sa->sin_addr.s_addr) & 0xFF);
        freeaddrinfo(res);
        s_resolved = ping_start(ip);
        if (s_resolved) {
            ESP_LOGI(TAG, "%s → %s (DNS)", s_name, ip);
        }
        return s_resolved;
    }
    if (res) {
        freeaddrinfo(res);
    }
    ESP_LOGW(TAG, "%s не разрешилось, беру запасной адрес %s", s_name, s_fallback);
    s_resolved = ping_start(s_fallback);
    return s_resolved;
}

static void ping_inet_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_enabled) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!s_ping) {
            int64_t now = esp_timer_get_time();
            if (!s_last_resolve_us || (now - s_last_resolve_us) > (int64_t)RESOLVE_RETRY_MS * 1000) {
                resolve_target();
            }
        } else if (s_last_reply_us && (esp_timer_get_time() - s_last_reply_us) > 60000000LL) {
            s_last_reply_us = 0;          /* минуту молчит — сессию переподнять */
            ESP_LOGW(TAG, "цель %s молчит минуту — перезапускаю сессию", s_ip);
            ping_stop();
        }
        vTaskDelay(pdMS_TO_TICKS(PING_PERIOD_MS));
    }
}

void ping_inet_get(ping_inet_stat_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->target = ping_inet_label();
    out->resolved = s_resolved;
    snprintf(out->ip, sizeof(out->ip), "%s", s_ip);
    uint32_t ok = s_ok, fail = s_fail;
    out->ok = ok > 0;
    out->hist_len = s_hist_len;
    for (int i = 0; i < s_hist_len && i < PING_INET_HIST; i++) {
        out->hist[i] = s_hist[i];
    }
    uint32_t total = ok + fail;
    out->loss_pct = total ? (uint32_t)((fail * 100u) / total) : 100u;
    out->min_ms = s_min_ms;
    out->max_ms = s_max_ms;
    if (s_hist_len) {
        uint32_t sum = 0, cnt = 0;
        for (int i = 0; i < s_hist_len; i++) {
            if (s_hist[i]) {
                sum += s_hist[i];
                cnt++;
            }
        }
        out->avg_ms = cnt ? (sum / cnt) : 0;
    }
}

void ping_inet_init(const char *name, const char *fallback_ip)
{
    ping_inet_set_target(name, fallback_ip);
    xTaskCreate(ping_inet_task, "pingin", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "цель пинга: %s (запасной адрес %s)", s_name, s_fallback);
}
