/*
 * Связка «состояние → экран»: собираем всё, что знаем о себе и о хосте, и
 * перерисовываем панель. Живёт в отдельной задаче, чтобы не мешать сети.
 *
 * Правила обновления (требования пользователя 16.09, см. ISSUES З-34):
 *   * первый кадр рисуется СРАЗУ после старта задачи — белого поля при включении нет;
 *   * плановые перерисовки и смена страницы — частичным обновлением, без мигания;
 *   * полное обновление (с инверсией) включается само каждое 10-е — стирает «чернила»
 *     (счётчик внутри display_show);
 *   * инверсный вид — только авария: хост не подаёт признаков; в этом случае кадр
 *     инвертируется и обновляется полностью.
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
#include "netinfo.h"
#include "ping_inet.h"
#include "probe.h"
#include "screen.h"
#include "selfcheck.h"
#include "settings.h"
#include "shtc3.h"
#include "timesync.h"

#define DRAW_PERIOD_US   (5 * 1000000)    /* плановая перерисовка раз в 5 с (частичная) */
#define SENSOR_PERIOD_US (10 * 1000000)   /* датчик опрашиваем раз в 10 с */
#define ONLINE_WINDOW_S  60               /* «хост на связи», если подавал признаки за минуту */

static const char *TAG = "ui";

static volatile bool s_link;
static volatile uint32_t s_reconnects;
static volatile int  s_page;
static volatile bool s_redraw;
static bool s_panel_ok;
static bool s_sensor_ok;
static volatile float s_t_c, s_rh_c;      /* последние показания датчика (для веб-страницы) */

static void btn_cb(btn_id_t btn, btn_event_t ev)
{
    /* На странице настроек (последняя) кнопка BOOT управляет настройками:
       короткое нажатие — поворот экрана по кругу, удержание — блокировка записи на диск. */
    if (btn == BTN_BOOT && s_page == SCREEN_PAGES - 1) {
        if (ev == BTN_EV_HOLD) {
            const settings_t *cfg = settings_get();
            settings_set_disk_write_lock(!cfg->disk_write_lock);
            ESP_LOGW(TAG, "настройки: диск %s", settings_get()->disk_write_lock ?
                     "только чтение" : "чтение и запись");
        } else if (ev == BTN_EV_SHORT) {
            uint8_t deg = settings_rotation_next();
            ESP_LOGI(TAG, "настройки: поворот экрана %u", (unsigned)deg);
        }
        s_redraw = true;
        return;
    }

    if (ev == BTN_EV_HOLD) {
        /* Удержание пока только помечаем: уход в загрузчик по кнопке — отдельный
           вопрос (есть /api/boot и автопереход самопроверки, см. ISSUES З-33). */
        ESP_LOGW(TAG, "%s удержана 3 с (действие не назначено)", btn == BTN_BOOT ? "BOOT" : "PWR");
        return;
    }
    if (btn == BTN_PWR && ev == BTN_EV_SHORT) {
        s_page = (s_page + 1) % SCREEN_PAGES;                 /* короткое — следующая */
    } else if (btn == BTN_PWR) {
        s_page = (s_page + SCREEN_PAGES - 1) % SCREEN_PAGES;  /* длинное — назад */
    }
    s_redraw = true;                                          /* перерисовать сразу, без вспышек */
}

static void ui_task(void *arg)
{
    (void)arg;
    int64_t last_draw = 0, last_sensor = 0;
    float t = 0, rh = 0;
    bool first = true;

    /* цель пинга берём из настроек и следим за изменениями (её правят на странице настроек) */
    char ping_applied[SETTINGS_PING_LEN];
    snprintf(ping_applied, sizeof(ping_applied), "%s", settings_get()->ping_target);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();

        if (now - last_sensor > SENSOR_PERIOD_US) {
            last_sensor = now;
            s_sensor_ok = shtc3_read(&t, &rh);
            s_t_c = t;
            s_rh_c = rh;
        }
        /* первый проход — рисуем сразу: иначе панель стоит пустой до первой перерисовки */
        if (!s_panel_ok || (!first && (now - last_draw < DRAW_PERIOD_US) && !s_redraw)) {
            continue;
        }
        first = false;
        s_redraw = false;
        last_draw = now;

        selfcheck_status_t sc;
        selfcheck_status(&sc);
        probe_state_t pr;
        probe_get(&pr);
        ping_inet_stat_t pi;
        ping_inet_get(&pi);

        bool host_seen = sc.host_known != 0;
        bool online = sc.enabled ? (sc.silence_s < ONLINE_WINDOW_S) : host_seen;
        bool router = net_from_router();
        ping_inet_enable(router);          /* без адреса от роутера маршрута в сеть нет */

        const settings_t *cfg = settings_get();
        if (strcmp(cfg->ping_target, ping_applied) != 0) {
            snprintf(ping_applied, sizeof(ping_applied), "%s", cfg->ping_target);
            ping_inet_set_target(cfg->ping_target, NULL);   /* цель изменили — перезапустить пинг */
        }

        if (sc.host_known) {               /* цель проверок портов и SNTP — адрес хоста */
            probe_set_host(sc.host);
            timesync_set_host(sc.host);
        }

        char time_str[16], date_str[16];
        timesync_time_str(time_str, sizeof(time_str));
        timesync_date_str(date_str, sizeof(date_str));
        bool time_ok = timesync_source(NULL, 0);

        screen_state_t st;
        memset(&st, 0, sizeof(st));
        st.dev_ip = net_ip_str();
        st.net_mode = net_mode_text();
        st.gateway = net_gw_str();
        st.net_router = router;
        st.up_s = (uint32_t)(now / 1000000);

        st.time_valid = time_ok;
        st.time_str = time_str;
        st.date_str = date_str;

        st.sensor_ok = s_sensor_ok;
        st.t_c = t;
        st.rh = rh;

        st.ping_target = pi.target;
        st.ping_ok = pi.ok;
        st.ping_count = pi.hist_len > SCREEN_PING_LINES ? SCREEN_PING_LINES : pi.hist_len;
        for (int i = 0; i < st.ping_count; i++) {
            st.ping_last[i] = pi.hist[i];
        }
        st.ping_loss_pct = pi.loss_pct;
        st.ping_avg = pi.avg_ms;
        st.ping_min = pi.min_ms;
        st.ping_max = pi.max_ms;

        st.host_known = host_seen;
        st.host_ip = sc.host;
        st.host_name = selfcheck_host_name();
        st.ports_known = pr.checked;
        snprintf(st.ports, sizeof(st.ports), "%s", pr.ports);
        st.web_ok = pr.http_ok;
        st.web_code = pr.http_code;
        st.web_ms = pr.http_ms;

        st.fw = fw_version_str();
        st.rotation = cfg->rotation;
        st.disk_write_lock = cfg->disk_write_lock;

        st.emergency = !online;            /* авария: инверсия + полное обновление */

        ESP_LOGI(TAG, "экран %d: %s, адрес %s (%s), хост %s, ping %s %u мс",
                 s_page, st.emergency ? "АВАРИЯ" : "норма", st.dev_ip, st.net_mode,
                 host_seen ? (st.host_name[0] ? st.host_name : sc.host) : "-",
                 pi.target ? pi.target : "-", (unsigned)pi.avg_ms);
        screen_show(&st, s_page);
    }
}

void ui_set_net_info(bool link_up, uint32_t rx_frames, uint32_t tx_frames,
                     uint32_t rx_bytes, uint32_t tx_bytes, uint32_t reconnects)
{
    (void)rx_frames;
    (void)tx_frames;
    (void)rx_bytes;
    (void)tx_bytes;
    s_link = link_up;
    s_reconnects = reconnects;
}

void ui_get_sensor(bool *ok, float *t_c, float *rh)
{
    if (ok) {
        *ok = s_sensor_ok;
    }
    if (t_c) {
        *t_c = s_t_c;
    }
    if (rh) {
        *rh = s_rh_c;
    }
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
    ESP_LOGI(TAG, "экран: %d страниц, перерисовка раз в %d с (полное — каждое 10-е)",
             SCREEN_PAGES, (int)(DRAW_PERIOD_US / 1000000));
}
