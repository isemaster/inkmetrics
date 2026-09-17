/*
 * Связка «состояние → экран»: собираем всё, что знаем о себе и о хосте, и
 * перерисовываем панель. Живёт в отдельной задаче, чтобы не мешать сети.
 *
 * Правила обновления (требования пользователя 16.09, см. ISSUES З-34):
 *   * первый кадр рисуется СРАЗУ после старта задачи — белого поля при включении нет;
 *   * плановые перерисовки и смена экрана — частичным обновлением, без мигания;
 *   * полное обновление (с инверсией) включается само каждое 7-е — стирает «чернила»
 *     (счётчик внутри display_show);
 *   * инверсный вид — только авария: своего адреса нет и через минуту после старта.
 *
 * Кнопки (макет v5, docs/screens-v5.md):
 *   PWR короткое  — сводный ↔ SETUP (внутри SETUP: назад на сводный);
 *   PWR длинное   — принудительная перерисовка (полное обновление);
 *   PWR удержание — действие не назначено (уход в загрузчик — отдельный вопрос, З-33);
 *   BOOT короткое — на SETUP крутит поворот экрана по кругу, на сводном — перерисовка;
 *   BOOT удержание— на SETUP включает/выключает запись на диск прибора.
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
#include "ingest.h"
#include "netinfo.h"
#include "ping_inet.h"
#include "probe.h"
#include "screen.h"
#include "settings.h"
#include "shtc3.h"
#include "timesync.h"

#define DRAW_PERIOD_US   (5 * 1000000)    /* плановая перерисовка раз в 5 с (частичная) */
#define SENSOR_PERIOD_US (10 * 1000000)   /* датчик опрашиваем раз в 10 с */
#define EMERGENCY_S      60               /* адреса нет дольше этого — экран в аварии */

#define PAGE_SUMMARY 0
#define PAGE_SETUP   1

static const char *TAG = "ui";

static volatile bool s_link;
static volatile uint32_t s_reconnects;
static volatile int  s_page = PAGE_SUMMARY;
static volatile bool s_redraw;
static bool s_panel_ok;
static bool s_sensor_ok;
static volatile float s_t_c, s_rh_c;      /* последние показания датчика (для веб-страницы) */

static void btn_cb(btn_id_t btn, btn_event_t ev)
{
    /* BOOT на экране SETUP — единственные настройки, которые можно менять с прибора:
       короткое нажатие крутит поворот, удержание включает/выключает запись на диск.
       Значения (адрес, цель пинга, пороги) набираются только в браузере. */
    if (btn == BTN_BOOT && s_page == PAGE_SETUP) {
        if (ev == BTN_EV_HOLD) {
            const settings_t *cfg = settings_get();
            settings_set_disk_write_lock(!cfg->disk_write_lock);
            ESP_LOGW(TAG, "настройки: диск %s", settings_get()->disk_write_lock ?
                     "только чтение" : "чтение и запись");
            s_redraw = true;
        } else if (ev == BTN_EV_SHORT) {
            ESP_LOGI(TAG, "настройки: поворот экрана %u", (unsigned)settings_rotation_next());
            s_redraw = true;
        }
        return;
    }

    if (ev == BTN_EV_HOLD) {
        ESP_LOGW(TAG, "%s удержана 3 с (действие не назначено)", btn == BTN_BOOT ? "BOOT" : "PWR");
        return;
    }
    if (ev == BTN_EV_LONG) {
        if (btn == BTN_PWR) {                       /* длинное — принудительная перерисовка */
            display_force_full();
            s_redraw = true;
        }
        return;
    }
    /* короткое нажатие */
    if (btn == BTN_PWR) {
        s_page = (s_page == PAGE_SUMMARY) ? PAGE_SETUP : PAGE_SUMMARY;
    } else {
        display_force_full();                       /* BOOT: перерисовать */
    }
    s_redraw = true;                                /* перерисовать сразу, без вспышек */
}

static void ui_task(void *arg)
{
    (void)arg;
    int64_t last_draw = 0, last_sensor = 0;
    float t = 0, rh = 0;
    bool first = true;

    /* цель пинга берём из настроек и следим за изменениями (её правят на /setup) */
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

        const settings_t *cfg = settings_get();
        if (strcmp(cfg->ping_target, ping_applied) != 0) {
            snprintf(ping_applied, sizeof(ping_applied), "%s", cfg->ping_target);
            ping_inet_set_target(cfg->ping_target, NULL);   /* цель изменили — перезапустить */
        }

        screen_state_t st;
        memset(&st, 0, sizeof(st));

        /* метрики хоста от агента (POST /ingest): экран показывает ровно то, что
           прислал хост, и возраст данных. Строки живут в im до конца итерации —
           на них можно ссылаться в screen_state_t (кадр рисуется тут же). */
        ingest_state_t im;
        ingest_get(&im);
        st.agent_have = im.have;
        st.agent_age_s = im.age_s;
        st.agent_ok = im.have && im.age_s <= SCREEN_ONLINE_S;
        st.cpu_pct = im.cpu_pct;
        st.mem_pct = im.mem_pct;
        st.disk_pct = im.disk_pct;
        st.gpu_count = im.gpu_count;
        for (int i = 0; i < SCREEN_GPU_MAX; i++) {
            st.gpu_temp_c[i] = im.gpu_temp_c[i];
            st.gpu_pct[i] = im.gpu_pct[i];
        }
        st.hup_ok = im.up_h >= 0;
        st.hup_h = im.up_h;

        st.sensor_ok = s_sensor_ok;
        st.t_c = t;
        st.rh = rh;

        st.dev_ip = net_ip_str();
        st.up_s = (uint32_t)(now / 1000000);
        st.fw = fw_version_str();
        st.ping_target = cfg->ping_target;
        st.rotation = cfg->rotation;
        st.disk_write_lock = cfg->disk_write_lock;

        /* авария — не «хост молчит» (это видно по надписи в рамке), а отсутствие
           своего адреса: без него прибор недоступен и в браузере тоже */
        st.emergency = (net_mode() == NET_MODE_NONE) && (st.up_s > EMERGENCY_S);

        ESP_LOGI(TAG, "экран %d: %s, возраст данных %u с, карт %d, cpu %.0f%%, "
                      "gpu0 %d C, gpu1 %d C",
                 s_page, st.agent_ok ? "ONLINE" : "НЕТ СВЯЗИ", (unsigned)st.agent_age_s,
                 st.gpu_count, (double)st.cpu_pct, st.gpu_temp_c[0], st.gpu_temp_c[1]);
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
    /* пробы портов, пинг в интернет и время с хоста на новых двух экранах не
       показываются, но их данные нужны веб-кабинету (/api/state) — задачи держим */
    probe_init();
    timesync_init();
    buttons_init(btn_cb);
    xTaskCreate(ui_task, "ui", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "экран: %d страницы (сводный и SETUP), перерисовка раз в %d с "
                  "(полное — каждое 7-е)",
             SCREEN_PAGES, (int)(DRAW_PERIOD_US / 1000000));
}
