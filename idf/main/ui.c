/*
 * Связка «состояние → экран»: раз в 10 с (или по кнопке) собираем всё, что знаем
 * о хосте и о себе, и перерисовываем панель. Полная перерисовка ~1,4 с, поэтому
 * живёт это в отдельной задаче и не мешает сети.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "probe.h"
#include "screen.h"
#include "selfcheck.h"
#include "shtc3.h"
#include "timesync.h"

#define DRAW_PERIOD_US   (10 * 1000000)   /* перерисовка раз в 10 с */
#define SENSOR_PERIOD_US (10 * 1000000)
#define ONLINE_WINDOW_S  60               /* «в сети», если хост подавал признаки за минуту */

static const char *TAG = "ui";

static volatile bool s_link;
static volatile uint32_t s_rx_frames, s_tx_frames;
static volatile uint32_t s_rx_bytes, s_tx_bytes;
static volatile uint32_t s_reconnects;
static volatile int  s_page;
static volatile bool s_redraw;
static bool s_panel_ok;
static bool s_sensor_ok;

static const char *FW = "0.3.0-idf";
static const char *DEV_IP = "192.168.7.1";

static void btn_cb(btn_id_t btn, btn_event_t ev)
{
    if (ev == BTN_EV_HOLD) {
        /* Удержание пока только помечаем: уход в загрузчик по кнопке — отдельный
           вопрос (есть /api/boot и автопереход самопроверки, см. ISSUES З-33). */
        ESP_LOGW(TAG, "%s удержана 3 с (действие не назначено)", btn == BTN_BOOT ? "BOOT" : "PWR");
        return;
    }
    if (btn == BTN_PWR && ev == BTN_EV_SHORT) {
        s_page = (s_page + 1) % SCREEN_PAGES;
    } else if (btn == BTN_PWR) {
        s_page = (s_page + SCREEN_PAGES - 1) % SCREEN_PAGES;   /* длинное — назад */
    } else {
        ESP_LOGI(TAG, "%s → перерисовка", ev == BTN_EV_LONG ? "BOOT (длинное)" : "BOOT");
    }
    s_redraw = true;
}

/* Скорость канала считаем по разнице счётчиков между перерисовками. */
static void rates(uint32_t frames, uint32_t bytes, int64_t dt_us,
                  uint32_t *fps, uint32_t *kbs)
{
    static uint32_t prev_frames, prev_bytes;
    static int64_t prev_us;
    static uint32_t last_fps, last_kbs;
    /* счётчики передаются раздельно для RX и TX — здесь только накопительная логика */
    if (prev_us && dt_us > 0) {
        last_fps = (uint32_t)(((uint64_t)(frames - prev_frames) * 1000000ull) / (uint64_t)dt_us);
        last_kbs = (uint32_t)(((uint64_t)((bytes - prev_bytes) / 1024ull) * 1000000ull) / (uint64_t)dt_us);
    }
    prev_frames = frames;
    prev_bytes = bytes;
    prev_us = esp_timer_get_time();
    *fps = last_fps;
    *kbs = last_kbs;
}

static void ui_task(void *arg)
{
    (void)arg;
    int64_t last_draw = 0, last_sensor = 0;
    float t = 0, rh = 0;
    uint32_t rx_rate = 0, tx_rate = 0, rx_kbs = 0, tx_kbs = 0;
    uint32_t prev_rx = 0, prev_tx = 0, prev_rx_b = 0, prev_tx_b = 0;
    int64_t prev_us = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();

        if (now - last_sensor > SENSOR_PERIOD_US) {
            last_sensor = now;
            s_sensor_ok = shtc3_read(&t, &rh);
        }
        if (!s_panel_ok || (now - last_draw < DRAW_PERIOD_US && !s_redraw)) {
            continue;
        }
        s_redraw = false;
        last_draw = now;

        if (prev_us) {
            uint32_t dt_ms = (uint32_t)((now - prev_us) / 1000);
            if (dt_ms > 0) {
                rx_rate = (uint32_t)(((s_rx_frames - prev_rx) * 1000ull) / dt_ms);
                tx_rate = (uint32_t)(((s_tx_frames - prev_tx) * 1000ull) / dt_ms);
                rx_kbs = (uint32_t)((((s_rx_bytes - prev_rx_b) / 1024ull) * 1000ull) / dt_ms);
                tx_kbs = (uint32_t)((((s_tx_bytes - prev_tx_b) / 1024ull) * 1000ull) / dt_ms);
            }
        }
        prev_us = now;
        prev_rx = s_rx_frames;
        prev_tx = s_tx_frames;
        prev_rx_b = s_rx_bytes;
        prev_tx_b = s_tx_bytes;
        (void)rates;                       /* оставлено на случай отдельного расчёта */

        selfcheck_status_t sc;
        selfcheck_status(&sc);
        selfcheck_ping_stat_t ps;
        selfcheck_ping_stats(&ps);
        probe_state_t pr;
        probe_get(&pr);

        if (sc.host_known) {               /* цель проверок и SNTP — адрес хоста */
            probe_set_host(sc.host);
            timesync_set_host(sc.host);
        }

        bool online = sc.enabled ? (sc.silence_s < ONLINE_WINDOW_S) : (s_rx_frames > 0);

        char time_str[16], date_str[16], src[8];
        timesync_time_str(time_str, sizeof(time_str));
        timesync_date_str(date_str, sizeof(date_str));
        bool time_ok = timesync_source(src, sizeof(src));

        screen_state_t st;
        memset(&st, 0, sizeof(st));
        st.fw = FW;
        st.online = online;
        st.link_up = s_link;
        st.dev_ip = DEV_IP;
        st.up_s = (uint32_t)(now / 1000000);
        st.host_ip = sc.host;
        st.host_known = sc.host_known != 0;
        st.host_name = selfcheck_host_name();
        st.time_valid = time_ok;
        st.time_str = time_str;
        st.date_str = date_str;
        st.time_src = src;
        st.silence_s = sc.silence_s;
        st.fallback_left_s = sc.enabled ? sc.fallback_left_s : SCREEN_NEVER;
        st.http_hits = sc.http_hits;
        st.frames = s_rx_frames ? s_rx_frames : sc.host_frames;
        st.dhcp_pkts = sc.dhcp_frames;
        st.ping_ok = sc.ping_ok;
        st.ping_fail = sc.ping_fail;
        st.loss_pct = ps.loss_pct;
        st.rtt_last = ps.last_ms;
        st.rtt_min = ps.min_ms;
        st.rtt_avg = ps.avg_ms;
        st.rtt_max = ps.max_ms;
        st.rtt_hist_len = selfcheck_rtt_history(st.rtt_hist, SCREEN_RTT_LEN);
        st.rx_rate = rx_rate;
        st.tx_rate = tx_rate;
        st.rx_kbs = rx_kbs;
        st.tx_kbs = tx_kbs;
        st.reconnects = s_reconnects;
        for (int i = 0; i < SCREEN_SVCS; i++) {
            st.svc_name[i] = pr.svc[i].name;
            st.svc_port[i] = pr.svc[i].port;
            st.svc_open[i] = pr.svc[i].open;
        }
        st.http_ok = pr.http_ok;
        st.http_code = pr.http_code;
        st.http_ms = pr.http_ms;
        st.http_server = pr.http_server[0] ? pr.http_server : NULL;
        st.sensor_ok = s_sensor_ok;
        st.t_c = t;
        st.rh = rh;

        ESP_LOGI(TAG, "экран %d: %s, host %s, rtt %u ms, потери %u %%, время %s",
                 s_page, online ? "ONLINE" : "NO DATA",
                 st.host_known ? (st.host_name[0] ? st.host_name : st.host_ip) : "-",
                 (unsigned)st.rtt_last, (unsigned)st.loss_pct, time_ok ? time_str : "-");
        screen_show(&st, s_page);
    }
}

void ui_set_net_info(bool link_up, uint32_t rx_frames, uint32_t tx_frames,
                     uint32_t rx_bytes, uint32_t tx_bytes, uint32_t reconnects)
{
    s_link = link_up;
    s_rx_frames = rx_frames;
    s_tx_frames = tx_frames;
    s_rx_bytes = rx_bytes;
    s_tx_bytes = tx_bytes;
    s_reconnects = reconnects;
}

void ui_init(void)
{
    if (display_init() == ESP_OK) {
        s_panel_ok = true;
    } else {
        ESP_LOGE(TAG, "панель не поднялась — экран работать не будет");
    }
    if (shtc3_init() != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 не поднялся — температура/влажность будут н/д");
    }
    probe_init();
    timesync_init();
    buttons_init(btn_cb);
    xTaskCreate(ui_task, "ui", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "экран: %d страниц, перерисовка раз в %d с",
             SCREEN_PAGES, (int)(DRAW_PERIOD_US / 1000000));
}
