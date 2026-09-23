#include "settings.h"

#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NS      "inkcfg"    /* набор настроек текущего имени проекта */
#define NVS_NS_OLD  "inkcfg"   /* прежнее имя (проект звался inkmetrics до 23.09) */
#define KEY_ROT  "rot"
#define KEY_WLK  "wlock"
#define KEY_PING "ping"
#define KEY_SLOT1 "slot1"
#define KEY_SLOT2 "slot2"

static const char *TAG = "settings";

static settings_t s_st = {
    .rotation = 0,
    .disk_write_lock = false,
    .ping_target = "ya.ru",
    /* Первый слот — загрузка процессора: она есть на любом ПК. Второй — вторая карта,
       если её нет, первая (см. SLOT_GPU_LAST_PCT): так на машине с одной картой
       вторая цифра не пустует. */
    .slot = { SLOT_CPU_PCT, SLOT_GPU_LAST_PCT },
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

/* Прочитать слот из NVS: мусор и значения из будущих версий приводим к умолчанию. */
static uint8_t load_slot(const char *key, uint8_t def)
{
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, key, &v) == ESP_OK && v < SLOT_KIND_MAX) {
        return v;
    }
    return def;
}

/* Переезд настроек со старого имени набора после переименования проекта: если в новом
   наборе пусто, а в прежнем настройки лежат — переносим их как есть. Срабатывает один
   раз (при первой загрузке прошивки с новым именем), дальше пишем и читаем только новый
   набор. Иначе поворот экрана, слоты и узел пинга пришлось бы задавать заново. */
static void migrate_from_old_ns(void)
{
    nvs_handle_t old;
    if (nvs_open(NVS_NS_OLD, NVS_READONLY, &old) != ESP_OK) {
        return;                            /* прежнего набора нет — переезжать нечему */
    }
    uint16_t rot = 0;
    if (nvs_get_u16(old, KEY_ROT, &rot) == ESP_OK) {
        nvs_set_u16(s_nvs, KEY_ROT, rot);
    }
    uint8_t wlk = 0;
    if (nvs_get_u8(old, KEY_WLK, &wlk) == ESP_OK) {
        nvs_set_u8(s_nvs, KEY_WLK, wlk);
    }
    char ping[SETTINGS_PING_LEN] = {0};
    size_t len = sizeof(ping);
    if (nvs_get_str(old, KEY_PING, ping, &len) == ESP_OK && ping[0]) {
        nvs_set_str(s_nvs, KEY_PING, ping);
    }
    uint8_t s1 = 0;
    if (nvs_get_u8(old, KEY_SLOT1, &s1) == ESP_OK) {
        nvs_set_u8(s_nvs, KEY_SLOT1, s1);
    }
    uint8_t s2 = 0;
    if (nvs_get_u8(old, KEY_SLOT2, &s2) == ESP_OK) {
        nvs_set_u8(s_nvs, KEY_SLOT2, s2);
    }
    nvs_commit(s_nvs);
    nvs_close(old);
    ESP_LOGI(TAG, "настройки перенесены из прежнего набора %s", NVS_NS_OLD);
}

void settings_init(void)
{
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS недоступна (%s) — работаю на умолчаниях", esp_err_to_name(err));
        return;
    }
    s_nvs_ok = true;

    uint16_t probe = 0;
    uint8_t probe8 = 0;
    if (nvs_get_u16(s_nvs, KEY_ROT, &probe) != ESP_OK &&
        nvs_get_u8(s_nvs, KEY_ROT, &probe8) != ESP_OK) {
        migrate_from_old_ns();             /* в новом наборе пусто — ищем прежний */
    }

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
    s_st.slot[0] = load_slot(KEY_SLOT1, s_st.slot[0]);
    s_st.slot[1] = load_slot(KEY_SLOT2, s_st.slot[1]);

    display_set_rotation(s_st.rotation);
    ESP_LOGI(TAG, "настройки: поворот %u, диск %s, цель пинга %s, экран: %s + %s",
             (unsigned)s_st.rotation, s_st.disk_write_lock ? "только чтение" : "чтение и запись",
             s_st.ping_target, settings_slot_name(s_st.slot[0]), settings_slot_name(s_st.slot[1]));
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

/* ------------------------------------------------------- крупные числа сводного экрана */

uint8_t settings_slot(uint8_t idx)
{
    return idx < SETTINGS_SLOT_COUNT ? s_st.slot[idx] : SLOT_OFF;
}

void settings_set_slot(uint8_t idx, uint8_t kind)
{
    if (idx >= SETTINGS_SLOT_COUNT || kind >= SLOT_KIND_MAX) {
        return;
    }
    s_st.slot[idx] = kind;
    save_u8(idx == 0 ? KEY_SLOT1 : KEY_SLOT2, kind);
    ESP_LOGI(TAG, "слот %u: %s", (unsigned)(idx + 1), settings_slot_name(kind));
}

const char *settings_slot_name(uint8_t kind)
{
    switch (kind) {
    case SLOT_CPU_PCT:      return "CPU %";
    case SLOT_RAM_PCT:      return "RAM %";
    case SLOT_DISK_PCT:     return "DISK %";
    case SLOT_GPU0_PCT:     return "GPU0 %";
    case SLOT_GPU1_PCT:     return "GPU1 %";
    case SLOT_GPU_LAST_PCT: return "GPU: вторая, иначе первая";
    case SLOT_GPU0_TEMP:    return "GPU0 °C";
    case SLOT_GPU1_TEMP:    return "GPU1 °C";
    default:                return "пусто";
    }
}
