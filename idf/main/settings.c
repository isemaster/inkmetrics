#include "settings.h"

#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NS   "inkcfg"
#define KEY_ROT  "rot"
#define KEY_WLK  "wlock"
#define KEY_PING "ping"

static const char *TAG = "settings";

static settings_t s_st = {
    .rotation = 0,
    .disk_write_lock = false,
    .ping_target = "ya.ru",
};

static nvs_handle_t s_nvs;
static bool s_nvs_ok;

/* Поворот храним 16 битами: 270 в uint8_t не помещается (270 → 14 → «нет поворота»). */
static void save_rot(const char *key, uint16_t v)
{
    if (!s_nvs_ok) {
        return;
    }
    nvs_set_u16(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void save_u8(const char *key, uint8_t v)
{
    if (!s_nvs_ok) {
        return;
    }
    nvs_set_u8(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void save_str(const char *key, const char *v)
{
    if (!s_nvs_ok) {
        return;
    }
    nvs_set_str(s_nvs, key, v);
    nvs_commit(s_nvs);
}

void settings_init(void)
{
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS недоступна (%s) — работаю на умолчаниях", esp_err_to_name(err));
        return;
    }
    s_nvs_ok = true;

    uint16_t rot = 0;
    esp_err_t rerr = nvs_get_u16(s_nvs, KEY_ROT, &rot);
    if (rerr != ESP_OK) {
        /* запись, сделанная прежней версией (поворот лежал в u8): читаем как u8 */
        uint8_t old = 0;
        rerr = nvs_get_u8(s_nvs, KEY_ROT, &old);
        rot = old;
        if (rerr != ESP_OK) {
            rot = 0;
        }
    }
    if (rot == 90 || rot == 180 || rot == 270) {
        s_st.rotation = rot;
    } else {
        s_st.rotation = 0;
    }
    uint8_t wlk = 0;
    if (nvs_get_u8(s_nvs, KEY_WLK, &wlk) == ESP_OK) {
        s_st.disk_write_lock = wlk != 0;
    }
    size_t len = sizeof(s_st.ping_target);
    if (nvs_get_str(s_nvs, KEY_PING, s_st.ping_target, &len) != ESP_OK || !s_st.ping_target[0]) {
        snprintf(s_st.ping_target, sizeof(s_st.ping_target), "ya.ru");
    }

    display_set_rotation(s_st.rotation);
    ESP_LOGI(TAG, "настройки: поворот %u, диск %s, цель пинга %s",
             (unsigned)s_st.rotation, s_st.disk_write_lock ? "только чтение" : "чтение и запись",
             s_st.ping_target);
}

const settings_t *settings_get(void)
{
    return &s_st;
}

void settings_set_rotation(uint16_t deg)
{
    if (deg != 90 && deg != 180 && deg != 270) {
        deg = 0;
    }
    s_st.rotation = deg;
    display_set_rotation(deg);
    save_rot(KEY_ROT, deg);
    ESP_LOGI(TAG, "поворот экрана: %u", (unsigned)deg);
}

uint16_t settings_rotation_next(void)
{
    uint16_t next = 0;
    switch (s_st.rotation) {
    case 0:   next = 90;  break;
    case 90:  next = 180; break;
    case 180: next = 270; break;
    default:  next = 0;   break;
    }
    settings_set_rotation(next);
    return next;
}

void settings_set_disk_write_lock(bool lock)
{
    s_st.disk_write_lock = lock;
    save_u8(KEY_WLK, lock ? 1 : 0);
    ESP_LOGI(TAG, "диск хоста: %s", lock ? "только чтение" : "чтение и запись");
}

void settings_set_ping_target(const char *name)
{
    if (!name || !name[0]) {
        return;
    }
    snprintf(s_st.ping_target, sizeof(s_st.ping_target), "%s", name);
    save_str(KEY_PING, s_st.ping_target);
    ESP_LOGI(TAG, "цель пинга: %s", s_st.ping_target);
}
