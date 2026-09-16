#include "selfcheck.h"

#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

static const char *TAG = "selfcheck";

#if SELFCHECK_ENABLED

static esp_netif_t      *s_netif;
static esp_ping_handle_t s_ping;
static TaskHandle_t      s_task;

static volatile bool     s_link;
static volatile bool     s_http_up;
static volatile uint32_t s_host_frames;
static volatile uint32_t s_http_hits;
static volatile uint32_t s_dhcp_frames;
static volatile uint64_t s_boot_us;
static volatile uint64_t s_last_host_us;   /* любой признак хоста; 0 = не было */
static volatile uint64_t s_last_reply_us;  /* ответ на ping; 0 = не было */
static volatile uint32_t s_ping_ok, s_ping_fail;
static volatile uint32_t s_host_ip;        /* сетевой порядок; 0 = неизвестен */
static char              s_ping_target[16] = "";
static char              s_host_name[32];         /* имя хоста из DHCP-опции 12 */
/* задержки ping: сводка и кольцевая история для графика на экране */
static volatile uint32_t s_rtt_last, s_rtt_min, s_rtt_max, s_rtt_sum, s_rtt_cnt;
static volatile uint16_t s_rtt_hist[SELFCHECK_RTT_HISTORY];
static volatile uint32_t s_rtt_head;

static uint64_t now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

/* секунд с момента t; SELFCHECK_NEVER, если t == 0 */
static uint32_t age_s(uint64_t t)
{
    if (t == 0) {
        return SELFCHECK_NEVER;
    }
    return (uint32_t)((now_us() - t) / 1000000u);
}

/* ------------------------------------------------------------------ ping хоста */
/* Ответ на ping — самый прямой признак «хост жив и видит нас по USB-сети».
   Разрешение ICMP на Windows по умолчанию может быть закрыто, поэтому ping —
   не единственный признак: любой кадр от хоста и HTTP-запрос считаются тоже. */
static void ping_ok_cb(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t ms = 0;
    /* ESP_PING_PROF_TIMEGAP — время между запросом и ответом в миллисекундах */
    if (esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms)) != ESP_OK) {
        ms = 0;
    }
    s_ping_ok++;
    s_last_reply_us = now_us();
    s_last_host_us = now_us();
    s_rtt_last = ms;
    if (ms) {
        if (s_rtt_min == 0 || ms < s_rtt_min) {
            s_rtt_min = ms;
        }
        if (ms > s_rtt_max) {
            s_rtt_max = ms;
        }
        s_rtt_sum += ms;
        s_rtt_cnt++;
        s_rtt_hist[s_rtt_head % SELFCHECK_RTT_HISTORY] = (uint16_t)ms;
        s_rtt_head++;
    }
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    (void)hdl; (void)args;
    s_ping_fail++;
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    (void)hdl; (void)args;   /* сессия кончилась — тик поднимет заново */
}

static void ping_start(const char *target)
{
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(target, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = SELFCHECK_PING_INTERVAL_MS;
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
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &s_ping);
    if (err != ESP_OK) {
        s_ping = NULL;
        ESP_LOGW(TAG, "ping %s: %s", target, esp_err_to_name(err));
        diag_step("самопроверка: ping не поднялся (%s) → %s", esp_err_to_name(err), target);
        return;
    }
    esp_ping_start(s_ping);
    strncpy(s_ping_target, target, sizeof(s_ping_target) - 1);
    s_ping_target[sizeof(s_ping_target) - 1] = '\0';
    diag_step("самопроверка: пингую хост %s раз в %u мс", target,
              (unsigned)SELFCHECK_PING_INTERVAL_MS);
    ESP_LOGI(TAG, "пингую %s", target);
}

/* ------------------------------------------------------- разбор кадров от хоста */
/* Нам достаточно двух вещей: узнать адрес хоста (из ARP-запроса — значит он уже
   настроил сеть) и увидеть DHCP-обмен (хост просит адрес у нас). Всё остальное —
   просто признак жизни. */
void selfcheck_host_frame(const uint8_t *frame, uint16_t len)
{
    s_last_host_us = now_us();
    s_host_frames++;
    if (!frame || len < 34) {
        return;
    }
    uint16_t type = (uint16_t)((frame[12] << 8) | frame[13]);
    if (type == 0x0806 && len >= 42) {              /* ARP */
        uint16_t oper = (uint16_t)((frame[20] << 8) | frame[21]);
        if (oper == 1) {                            /* запрос от хоста */
            const uint8_t *spa = frame + 28;        /* sender protocol address */
            uint32_t ip = (uint32_t)spa[0] | ((uint32_t)spa[1] << 8) |
                          ((uint32_t)spa[2] << 16) | ((uint32_t)spa[3] << 24);
            if (ip && ip != s_host_ip) {
                s_host_ip = ip;
                diag_step("самопроверка: хост в сети — %u.%u.%u.%u (по ARP-запросу)",
                          spa[0], spa[1], spa[2], spa[3]);
            }
        }
    } else if (type == 0x0800 && len >= 34) {       /* IPv4 */
        uint8_t ihl = (uint8_t)((frame[14] & 0x0Fu) * 4u);
        if (frame[23] == 17 && ihl >= 20 && len >= (uint16_t)(14 + ihl + 8)) {
            const uint8_t *udp = frame + 14 + ihl;
            uint16_t sp = (uint16_t)((udp[0] << 8) | udp[1]);
            uint16_t dp = (uint16_t)((udp[2] << 8) | udp[3]);
            if (sp == 68 && dp == 67) {             /* хост спрашивает адрес у нас */
                s_dhcp_frames++;
                /* Разбираем опции DHCP: опция 12 — имя, которое хост сообщает о себе
                   (Windows и Linux шлют его в DISCOVER/REQUEST). Это единственное, что
                   хост рассказывает о себе сам, без установки чего-либо на нём. */
                const uint8_t *dhcp = udp + 8;
                uint16_t dhcp_len = (uint16_t)(len - (14 + ihl + 8));
                if (dhcp_len > 240) {
                    const uint8_t *opt = dhcp + 240;
                    const uint8_t *end = dhcp + dhcp_len;
                    while (opt + 1 < end && *opt != 255) {
                        uint8_t code = opt[0];
                        if (code == 0) {            /* padding */
                            opt++;
                            continue;
                        }
                        uint8_t olen = opt[1];
                        if (opt + 2 + olen > end) {
                            break;
                        }
                        if (code == 12 && olen > 0 && olen < sizeof(s_host_name)) {
                            memcpy(s_host_name, opt + 2, olen);
                            s_host_name[olen] = 0;
                            diag_step("самопроверка: хост назвался «%s» (DHCP опция 12)", s_host_name);
                        }
                        opt += 2 + olen;
                    }
                }
            }
        }
    }
}

/* --------------------------------------------------------- уход в режим загрузки */
void selfcheck_ping_stats(selfcheck_ping_stat_t *out)
{
    if (!out) {
        return;
    }
    out->last_ms = s_rtt_last;
    out->min_ms = s_rtt_min;
    out->max_ms = s_rtt_max;
    out->avg_ms = s_rtt_cnt ? (s_rtt_sum / s_rtt_cnt) : 0;
    uint32_t total = s_ping_ok + s_ping_fail;
    out->loss_pct = total ? (uint32_t)((s_ping_fail * 100u) / total) : 0;
}

int selfcheck_rtt_history(uint16_t *dst, int max)
{
    if (!dst || max <= 0) {
        return 0;
    }
    uint32_t have = s_rtt_head < SELFCHECK_RTT_HISTORY ? s_rtt_head : SELFCHECK_RTT_HISTORY;
    if ((uint32_t)max < have) {
        have = (uint32_t)max;
    }
    for (uint32_t i = 0; i < have; i++) {            /* от старых к новым */
        uint32_t idx = (s_rtt_head - have + i) % SELFCHECK_RTT_HISTORY;
        dst[i] = s_rtt_hist[idx];
    }
    return (int)have;
}

const char *selfcheck_host_name(void)
{
    return s_host_name;
}

void selfcheck_enter_download_mode(const char *why)
{
    ESP_LOGW(TAG, "ухожу в режим загрузки: %s", why);
    diag_step("УХОД В ЗАГРУЗЧИК: %s | кадры хоста %u (DHCP %u), http %u, ping %u/%u",
              why, (unsigned)s_host_frames, (unsigned)s_dhcp_frames,
              (unsigned)s_http_hits, (unsigned)s_ping_ok, (unsigned)s_ping_fail);
    /* даём записи лечь во флеш, иначе последней строки в «чёрном ящике» не будет */
    vTaskDelay(pdMS_TO_TICKS(300));
    /* Тот же путь, что у /api/boot: ROM видит бит и уходит в загрузчик.
       После прошивки бит снимает tools/try_flash.py (esptool write-mem). */
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

/* ------------------------------------------------------------- фоновая задача */
static void selfcheck_task(void *arg)
{
    (void)arg;
    uint64_t last_log_us = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* цель ping: как только узнали адрес хоста из ARP — пингуем именно его */
        char want[16];
        if (s_host_ip) {
            snprintf(want, sizeof(want), "%u.%u.%u.%u",
                     (unsigned)(s_host_ip & 0xFF), (unsigned)((s_host_ip >> 8) & 0xFF),
                     (unsigned)((s_host_ip >> 16) & 0xFF), (unsigned)((s_host_ip >> 24) & 0xFF));
        } else {
            snprintf(want, sizeof(want), "%s", SELFCHECK_HOST_DEFAULT);
        }
        if (!s_ping || strcmp(want, s_ping_target) != 0) {
            if (s_link || s_netif) {
                ping_start(want);
            }
        }

        /* отсчёт молчания хоста: от последнего признака, а если признаков не было —
           от старта прошивки */
        uint64_t base = s_last_host_us ? s_last_host_us : s_boot_us;
        uint32_t silence_ms = (uint32_t)((now_us() - base) / 1000u);

        if (now_us() - last_log_us >= 30000000ull) {     /* раз в 30 с — в «чёрный ящик» */
            last_log_us = now_us();
            diag_step("самопроверка: link %s, http %s, хост %s, молчание %u с,"
                      " кадры %u (DHCP %u), ping %u/%u",
                      s_link ? "есть" : "нет", s_http_up ? "поднят" : "нет",
                      s_host_ip ? want : "не найден", (unsigned)(silence_ms / 1000u),
                      (unsigned)s_host_frames, (unsigned)s_dhcp_frames,
                      (unsigned)s_ping_ok, (unsigned)s_ping_fail);
        }

        if (silence_ms >= SELFCHECK_FALLBACK_MS) {
            selfcheck_enter_download_mode("нет признаков хоста");
        }
    }
}

/* ------------------------------------------------------------------- интерфейс */
void selfcheck_init(void)
{
    s_boot_us = now_us();
    s_last_host_us = 0;
    s_last_reply_us = 0;
    diag_step("самопроверка включена: уход в загрузчик после %u с молчания хоста",
              (unsigned)(SELFCHECK_FALLBACK_MS / 1000u));
}

void selfcheck_start(esp_netif_t *netif)
{
    s_netif = netif;
    if (s_task) {
        return;
    }
    if (xTaskCreate(selfcheck_task, "selfcheck", 4096, NULL, 3, &s_task) != pdPASS) {
        diag_step("самопроверка: задачу создать не удалось");
    }
}

void selfcheck_http_up(bool up)
{
    s_http_up = up;
}

void selfcheck_set_link(bool up)
{
    s_link = up;
}

void selfcheck_http_hit(void)
{
    s_http_hits++;
    s_last_host_us = now_us();       /* хост дотянулся до нас — значит связь есть */
}

void selfcheck_status(selfcheck_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = true;
    out->link = s_link;
    out->http_up = s_http_up;
    out->host_known = s_host_ip ? 1 : 0;
    if (s_host_ip) {
        snprintf(out->host, sizeof(out->host), "%u.%u.%u.%u",
                 (unsigned)(s_host_ip & 0xFF), (unsigned)((s_host_ip >> 8) & 0xFF),
                 (unsigned)((s_host_ip >> 16) & 0xFF), (unsigned)((s_host_ip >> 24) & 0xFF));
    } else {
        snprintf(out->host, sizeof(out->host), "%s", SELFCHECK_HOST_DEFAULT);
    }
    uint64_t base = s_last_host_us ? s_last_host_us : s_boot_us;
    uint32_t silence_s = (uint32_t)((now_us() - base) / 1000000u);
    out->silence_s = silence_s;
    uint32_t limit_s = SELFCHECK_FALLBACK_MS / 1000u;
    out->fallback_left_s = silence_s < limit_s ? (limit_s - silence_s) : 0;
    out->ping_ok = s_ping_ok;
    out->ping_fail = s_ping_fail;
    out->ping_age_s = age_s(s_last_reply_us);
    out->http_hits = s_http_hits;
    out->host_frames = s_host_frames;
    out->dhcp_frames = s_dhcp_frames;
}

#else  /* SELFCHECK_ENABLED == 0 — релиз: ничего не делаем */

void selfcheck_init(void) {}
void selfcheck_start(esp_netif_t *netif) { (void)netif; }
void selfcheck_http_up(bool up) { (void)up; }
void selfcheck_set_link(bool up) { (void)up; }
void selfcheck_http_hit(void) {}
void selfcheck_host_frame(const uint8_t *frame, uint16_t len) { (void)frame; (void)len; }
void selfcheck_enter_download_mode(const char *why) { (void)why; }
void selfcheck_status(selfcheck_status_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

void selfcheck_ping_stats(selfcheck_ping_stat_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

int selfcheck_rtt_history(uint16_t *dst, int max)
{
    (void)dst; (void)max;
    return 0;
}

const char *selfcheck_host_name(void)
{
    return "";
}

#endif /* SELFCHECK_ENABLED */
