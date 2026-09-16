/*
 * Связка «состояние → экран»: раз в 10 с (или по кнопке) собираем состояние сети,
 * датчика и самопроверки и перерисовываем панель. Полная перерисовка ~1,4 с, поэтому
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
#include "screen.h"
#include "selfcheck.h"
#include "shtc3.h"

#define DRAW_PERIOD_US   (10 * 1000000)   /* перерисовка раз в 10 с */
#define SENSOR_PERIOD_US (10 * 1000000)
#define ONLINE_WINDOW_S  60               /* «в сети», если хост подавал признаки за минуту */

static const char *TAG = "ui";

static volatile bool s_link;
static volatile uint32_t s_rx, s_tx;
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
           вопрос (есть страница /api/boot и автопереход самопроверки). */
        ESP_LOGW(TAG, "%s удержана 3 с (действие пока не назначено)", btn == BTN_BOOT ? "BOOT" : "PWR");
        return;
    }
    if (btn == BTN_PWR) {
        s_page = (s_page + 1) % SCREEN_PAGES;
        ESP_LOGI(TAG, "%s → экран %d", ev == BTN_EV_LONG ? "PWR (длинное)" : "PWR", s_page);
    } else {
        ESP_LOGI(TAG, "%s → перерисовка", ev == BTN_EV_LONG ? "BOOT (длинное)" : "BOOT");
    }
    s_redraw = true;
}

static void ui_task(void *arg)
{
    (void)arg;
    int64_t last_draw = 0, last_sensor = 0;
    float t = 0, rh = 0;

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

        selfcheck_status_t sc;
        selfcheck_status(&sc);
        bool online = sc.enabled ? (sc.silence_s < ONLINE_WINDOW_S) : (s_rx > 0);

        screen_state_t st = {
            .fw = FW,
            .online = online,
            .link_up = s_link,
            .dev_ip = DEV_IP,
            .host_ip = sc.host,
            .host_known = sc.host_known != 0,
            .up_s = (uint32_t)(now / 1000000),
            .ping_ok = sc.ping_ok,
            .ping_fail = sc.ping_fail,
            .ping_age_s = sc.ping_age_s,
            .http_hits = sc.http_hits,
            .frames = s_rx ? s_rx : sc.host_frames,
            .dhcp_pkts = sc.dhcp_frames,
            .silence_s = sc.silence_s,
            .fallback_left_s = sc.enabled ? sc.fallback_left_s : SCREEN_NEVER,
            .sensor_ok = s_sensor_ok,
            .t_c = t,
            .rh = rh,
        };
        ESP_LOGI(TAG, "экран %d: %s, host %s, up %u с", s_page, online ? "ONLINE" : "NO DATA",
                 st.host_known ? st.host_ip : "-", (unsigned)st.up_s);
        screen_show(&st, s_page);
    }
}

void ui_set_net_info(bool link_up, uint32_t rx_frames, uint32_t tx_frames)
{
    s_link = link_up;
    s_rx = rx_frames;
    s_tx = tx_frames;
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
    buttons_init(btn_cb);
    xTaskCreate(ui_task, "ui", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "экран: %d страниц, перерисовка раз в %d с",
             SCREEN_PAGES, (int)(DRAW_PERIOD_US / 1000000));
}
