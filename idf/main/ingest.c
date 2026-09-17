/*
 * Приём и разбор метрик хоста (POST /ingest) — см. ingest.h.
 *
 * Разбор сделан вручную: тело запроса — плоский JSON из чисел, строк и булевых
 * значений, и ради него тянуть в прошивку cJSON не нужно. Побочная выгода: у нас
 * нет внешних зависимостей, и сборка не зависит от версии компонента json.
 *
 * Состояние защищено короткими критическими секциями: пишет задача HTTP-сервера,
 * читает задача экрана.
 */
#include "ingest.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define TAG "ingest"

static ingest_state_t   s_st;        /* последние принятые метрики */
static uint64_t         s_last_us;   /* когда их приняли (микросекунды с запуска) */
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------- разбор JSON */

/* Найти значение по ключу верхнего уровня: ищем "ключ" и двоеточие после него.
   Возвращает указатель на первый значимый символ значения. */
static const char *json_find(const char *json, const char *key)
{
    size_t keylen = strlen(key);
    const char *p = json;

    while (p && *p) {
        if (*p != '"') {
            p++;
            continue;
        }
        const char *name = p + 1;                 /* имя поля без кавычки */
        const char *end  = strchr(name, '"');
        if (!end) {
            break;
        }
        size_t len = (size_t)(end - name);
        const char *q = end + 1;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
            q++;
        }
        if (*q == ':' && len == keylen && strncmp(name, key, keylen) == 0) {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
                q++;
            }
            return q;
        }
        /* не наш ключ — идём дальше от конца имени. Значения в нашем формате
           кавычек внутри не содержат, поэтому «чужие» строки разбираются как имена
           и отбрасываются: после них нет двоеточия. */
        p = end + 1;
    }
    return NULL;
}

static bool json_num(const char *json, const char *key, double *out)
{
    const char *p = json_find(json, key);
    if (!p) {
        return false;
    }
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) {
        return false;                  /* не число: null, строка, мусор */
    }
    *out = v;
    return true;
}

static bool json_bool(const char *json, const char *key, bool *out)
{
    const char *p = json_find(json, key);
    if (!p) {
        return false;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    double d = 0;
    if (json_num(json, key, &d)) {
        *out = (d != 0);
        return true;
    }
    return false;
}

static bool json_str(const char *json, const char *key, char *out, size_t len)
{
    const char *p = json_find(json, key);
    if (!p || *p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < len) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

/* ------------------------------------------------------------------ приём данных */

/* Разобрать поля одной карты: <prefix>percent, <prefix>temp, <prefix>mem_percent.
   prefix — "gpu0_" / "gpu1_" (новый агент) или "gpu_" (старый, одна карта).
   found = true, если хоть одно поле нашлось: по этому признаку считаем число карт. */
static void gpu_parse(const char *json, const char *prefix, ingest_state_t *st, int idx,
                      bool *found)
{
    char key[24];
    double d = 0;

    snprintf(key, sizeof(key), "%spercent", prefix);
    if (json_num(json, key, &d)) {
        st->gpu_pct[idx] = (float)d;
        *found = true;
    }
    snprintf(key, sizeof(key), "%stemp", prefix);
    if (json_num(json, key, &d)) {
        st->gpu_temp_c[idx] = (int)(d + 0.5);
        *found = true;
    }
    snprintf(key, sizeof(key), "%smem_percent", prefix);
    if (json_num(json, key, &d)) {
        st->gpu_mem_pct[idx] = (float)d;
        *found = true;
    }
}

void ingest_apply(const char *json)
{
    if (!json) {
        return;
    }

    ingest_state_t st;
    memset(&st, 0, sizeof(st));
    st.have = true;
    st.fresh = true;
    st.age_s = 0;

    /* «н/д» по умолчанию: если поля в JSON нет, оно таким и останется */
    st.cpu_pct = st.mem_pct = st.disk_pct = INGEST_NA;
    for (int i = 0; i < INGEST_GPU_MAX; i++) {
        st.gpu_pct[i] = st.gpu_mem_pct[i] = INGEST_NA;
        st.gpu_temp_c[i] = INGEST_NA;
    }
    st.cpu_temp_c = INGEST_NA;
    st.up_h = INGEST_NA;
    st.tcp_est = INGEST_NA;
    st.ping_ok = 0;

    double d = 0;
    bool b = false;

    json_str(json, "hostname", st.host, sizeof(st.host));
    json_str(json, "smart_status", st.smart, sizeof(st.smart));
    json_str(json, "ping_target", st.ping_target, sizeof(st.ping_target));

    if (json_num(json, "cpu_percent", &d)) {
        st.cpu_pct = (float)d;
    }
    if (json_num(json, "mem_percent", &d)) {
        st.mem_pct = (float)d;
    }
    if (json_num(json, "disk_percent", &d)) {
        st.disk_pct = (float)d;
    }
    if (json_num(json, "cpu_temp", &d)) {
        st.cpu_temp_c = (int)(d + 0.5);
    }
    if (json_num(json, "ping_ms", &d)) {
        st.ping_ms = (uint32_t)(d < 0 ? 0 : d);
    }
    if (json_num(json, "uptime_hours", &d)) {
        st.up_h = (float)d;
    }
    if (json_num(json, "tcp_established", &d)) {
        st.tcp_est = (int32_t)d;
    }
    if (json_bool(json, "ping_ok", &b)) {
        st.ping_ok = b ? 1 : 0;
        st.ping_known = true;      /* хост проверил интернет и сказал результат */
    }

    /* карты: сначала новый формат, затем старый одиночный набор как карта 0 */
    bool g0 = false, g1 = false;
    gpu_parse(json, "gpu0_", &st, 0, &g0);
    if (!g0) {
        gpu_parse(json, "gpu_", &st, 0, &g0);
    }
    gpu_parse(json, "gpu1_", &st, 1, &g1);

    int count = (g0 ? 1 : 0) + (g1 ? 1 : 0);
    if (json_num(json, "gpu_count", &d) && d >= 0) {
        count = (int)d;                    /* агент знает точно, сколько карт */
    }
    st.gpu_count = count > INGEST_GPU_MAX ? INGEST_GPU_MAX : count;

    portENTER_CRITICAL(&s_mux);
    uint32_t cnt = s_st.count + 1;
    st.count = cnt;
    s_st = st;
    s_last_us = (uint64_t)esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "метрики хоста #%u: cpu %.0f%%, ram %.0f%%, диск %.0f%%, карт %d "
                  "(gpu0 %.0f%% %d C, gpu1 %.0f%% %d C)",
             (unsigned)cnt, (double)st.cpu_pct, (double)st.mem_pct, (double)st.disk_pct,
             st.gpu_count, (double)st.gpu_pct[0], st.gpu_temp_c[0],
             (double)st.gpu_pct[1], st.gpu_temp_c[1]);
}

void ingest_get(ingest_state_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    uint64_t last = s_last_us;
    portEXIT_CRITICAL(&s_mux);

    if (out->have && last > 0) {
        uint64_t age = ((uint64_t)esp_timer_get_time() - last) / 1000000u;
        out->age_s = (uint32_t)age;
        out->fresh = age <= INGEST_STALE_S;
    }
}
