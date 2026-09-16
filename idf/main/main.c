/*
 * inkmetrics на ESP-IDF: сервер живёт в приборе.
 *
 * Что делает эта прошивка:
 *   1. Прибор представляется хосту сетевой картой (USB NCM). Никакого софта
 *      на хосте ставить не нужно — драйвер встроен в Windows/Linux/macOS.
 *   2. Прибор выдаёт хосту адрес по DHCP и имеет свой адрес 192.168.7.1.
 *   3. На приборе поднят HTTP-сервер: страница состояния и /api/state.
 *
 * 4. Самопроверка (selfcheck.c): если за 5 минут хост ни разу себя не проявил
*    (нет ответов на ping, кадров в USB-сети и HTTP-запросов), прибор сам уходит
*    в режим загрузки — перепрошивать можно без кнопки BOOT.
*
* Аварийный выход: если кнопку BOOT держать при включении (3 с), прибор уходит
 * в режим загрузчика — это нужно, чтобы перепрошивать без выдёргивания USB.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/pbuf.h"
#include "driver/gpio.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_wrap_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "esp_private/usb_phy.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/net/net_device.h"   /* низкоуровневый сетевой класс TinyUSB (ECM/RNDIS) */

#include "diag.h"                   /* «чёрный ящик» логов (см. diag.c) */
#include "usb_desc.h"               /* описатели USB: без них ECM/RNDIS не поднимается */
#include "selfcheck.h"              /* самопроверка: уход в загрузчик без кнопки BOOT */
#include "ui.h"                     /* экран прибора: панель, кнопки, SHTC3 (см. ui.c) */
#include "timesync.h"               /* время с хоста: SNTP + подсказка от браузера */
#include "probe.h"                  /* проверка сервисов хоста по сети */
#include "ping_inet.h"              /* пинг во внешнюю сеть — то, что на экране */
#include "netinfo.h"                /* режим адресации: от роутера или аварийный */
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define USB_NET_IP    "192.168.7.1"  /* АВАРИЙНЫЙ адрес прибора (см. net_addr_task) */
#define USB_NET_MASK  "255.255.255.0"
#define USB_NET_GW    "192.168.7.1"
#define USB_NET_MTU   1514           /* кадр без FCS: 14 (Ethernet) + 1500 (MTU) */
#define NET_DHCP_TIMEOUT_MS 15000    /* сколько ждём адрес от роутера локальной сети */
#define PING_TARGET_NAME "ya.ru"     /* цель пинга во внешнюю сеть (показывается на экране) */
#define PING_TARGET_IP   "77.88.55.242"   /* запасной адрес, если DNS не отвечает */
#define BOOT_GPIO     0
#define FW_VERSION    "0.3.1-idf"

static const char *TAG = "eink";
static esp_netif_t *s_netif = NULL;
static volatile bool s_link_up = false;

/* ------------------------------------------------- режим адресации (netinfo.h) */
/* Адрес прибор получает сам: сначала просит у роутера локальной сети (обычный
   DHCP-клиент, запросы уходят через мост Windows на ПК), а если адреса нет за
   NET_DHCP_TIMEOUT_MS — берёт аварийный 192.168.7.1 и выдаёт адрес хосту сам. */
static volatile net_mode_t s_net_mode = NET_MODE_NONE;
static char s_ip_str[16] = "";
static char s_gw_str[16] = "";

net_mode_t net_mode(void) { return s_net_mode; }

bool net_from_router(void) { return s_net_mode == NET_MODE_ROUTER; }

const char *net_mode_text(void)
{
    switch (s_net_mode) {
    case NET_MODE_ROUTER:    return "FROM ROUTER";
    case NET_MODE_EMERGENCY: return "EMERGENCY";
    default:                 return "NO ADDR";
    }
}

const char *net_ip_str(void) { return s_ip_str[0] ? s_ip_str : USB_NET_IP; }
const char *net_gw_str(void)  { return s_gw_str[0] ? s_gw_str : "-"; }

static void net_info_refresh(void)
{
    esp_netif_ip_info_t ip = {0};
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ip.ip));
        snprintf(s_gw_str, sizeof(s_gw_str), IPSTR, IP2STR(&ip.gw));
    }
}

/* ------------------------------------------------------------------ аварийный выход */
static void boot_escape_check(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    if (gpio_get_level(BOOT_GPIO) != 0) {
        return;
    }
    ESP_LOGW(TAG, "BOOT удержан при старте — жду 3 с");
    diag_step("BOOT удержан при старте — жду 3 с");
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (gpio_get_level(BOOT_GPIO) == 0) {
        /* Пока BOOT нажат, наш сброс приводит к тому, что ROM видит низкий уровень
           на GPIO0 и уходит в режим загрузчика. RTC-бит FORCE_DOWNLOAD_BOOT
           сознательно НЕ используем: он «липкий» — после него чип входит в загрузчик
           на каждом сбросе, пока не снимешь питание полностью. */
        ESP_LOGW(TAG, "перезапуск: BOOT удержан, ROM уйдёт в загрузчик");
        diag_step("BOOT удержан → ребут в загрузчик");
        esp_restart();
    }
    ESP_LOGI(TAG, "BOOT отпущен — работаем дальше");
}

/* ---------------------------------------------------- обмен кадрами с lwIP (USB) */
/* Счётчики кадров: без них не понять, доходят ли пакеты хоста до прибора вообще
   (сеть поднялась, а адрес по DHCP хост не получил — это первое, что нужно знать). */
static volatile uint32_t s_rx_frames;
static volatile uint32_t s_tx_frames;
static volatile uint32_t s_rx_bytes;
static volatile uint32_t s_tx_bytes;
static volatile uint32_t s_reconnects;   /* сколько раз хост отключался */

#if CONFIG_TINYUSB_NET_MODE_NONE
/* Диагностическая сборка (sdkconfig.diag): USB-сети нет, логи идут в USB Serial/JTAG.
   Нужна, чтобы понять, где именно ломается запуск, не имея UART-адаптера. */
static esp_err_t usb_transmit(void *h, void *buffer, size_t len)
{
    (void)h; (void)buffer; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

static void usb_free_rx(void *h, void *buffer)
{
    free(buffer);
}
#else
/* MAC, которую увидит хост: локально администрируемая (первый байт 0x02) */
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};

static void usb_free_rx(void *h, void *buffer)
{
    free(buffer);
}

/* хост прислал кадр — отдаём его в lwIP */
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    s_rx_frames++;
    s_rx_bytes += size;
    selfcheck_host_frame(src, size);   /* признак жизни хоста + разбор ARP/DHCP */
    if (size == 0) {
        tud_network_recv_renew();
        return true;
    }
    uint8_t *buf = malloc(size);
    if (!buf) {
        tud_network_recv_renew();
        return false;
    }
    memcpy(buf, src, size);
    if (s_netif && s_link_up) {
        esp_netif_receive(s_netif, buf, size, buf);   /* освободит usb_free_rx */
    } else {
        free(buf);
    }
    tud_network_recv_renew();
    return true;
}

/* TinyUSB просит заполнить свой буфер кадром из lwIP.
   ref — уже линейный кадр, arg — его длина: так его передаёт наш usb_transmit
   (см. ниже про контракт esp_netif). Раньше здесь стоял pbuf_copy_partial по
   указателю, приведённому к struct pbuf*, и первый же кадр читал мусор. */
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    memcpy(dst, ref, arg);
    return arg;
}

void tud_network_init_cb(void)
{
}

/* lwIP хочет отправить кадр — отдаём его стеку USB.
 *
 * ВАЖНО (нашли 16.09 по дампу паники): buffer — это НЕ pbuf, а линейный кадр
 * (q->payload, q->len) — так его передаёт esp_netif: components/esp_netif/lwip/netif/
 * ethernetif.c:84 «esp_netif_transmit(esp_netif, q->payload, q->len)». Раньше здесь
 * стояло приведение к struct pbuf*, и первый же исходящий кадр (ответ DHCP) читал
 * «pbuf» из байтов MAC-адреса → pbuf_copy_partial шёл по мусорному указателю →
 * LoadProhibited (cause 28, vaddr 0x0472e820) в задаче lwIP «tiT» → плата
 * перезагружалась через ~20 с после включения, адрес по DHCP хост не получал
 * (З-01). Проверять контракт драйвера по исходникам IDF, а не по примерам TinyUSB. */
static esp_err_t usb_transmit(void *h, void *buffer, size_t len)
{
    if (len == 0 || len > USB_NET_MTU) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!tud_network_can_xmit((uint16_t)len)) {
        return ESP_ERR_NO_MEM;          /* хост ещё не готов принимать — lwIP повторит */
    }
    tud_network_xmit(buffer, (uint16_t)len);   /* ref = кадр, arg = длина */
    s_tx_frames++;
    s_tx_bytes += len;
    return ESP_OK;
}
#endif /* CONFIG_TINYUSB_NET_MODE_NONE */

/* Правильная привязка драйвера к netif — как это делает esp_eth:
   esp_netif_new() БЕЗ driver -> esp_netif_attach(base) -> post_attach -> set_driver_config().
   Раньше конфигурация драйвера передавалась прямо в esp_netif_new(cfg.driver): она терялась,
   и первый же esp_netif_set_mac() (его нет в диагностической сборке) ронял приложение. */
static esp_err_t usb_net_post_attach(esp_netif_t *netif, void *args)
{
    static esp_netif_driver_ifconfig_t drv = {
        .handle = (void *)"usb-ncm",
        .transmit = usb_transmit,
        .driver_free_rx_buffer = usb_free_rx,
    };
    return esp_netif_set_driver_config(netif, &drv);
}

static esp_netif_driver_base_t s_usb_base = {
    .post_attach = usb_net_post_attach,
};

/* -------------------------------------------------------------- веб-сервер прибора */
static const char INDEX_HTML[] =
    "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>inkmetrics</title><style>"
    "body{font:14px/1.5 system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}"
    "h1{font-size:18px;margin:0 0 12px}table{border-collapse:collapse;width:100%;max-width:520px}"
    "td{padding:4px 8px;border-bottom:1px solid #333}td:last-child{text-align:right;color:#7fd}"
    "a{color:#fc6}"
    "</style></head><body><h1>inkmetrics — сервер в приборе</h1>"
    "<p>Страница отдана самим прибором по USB-сети. На хосте не установлено ничего:"
    " драйвер сетевой карты встроен в систему.</p>"
    "<table id=\"t\"></table>"
    "<p><a href=\"/api/boot\">Уйти в режим загрузки сейчас</a> — то же прибор сделает сам,"
    " если хост молчит 5 минут (перепрошивка без кнопки BOOT).</p>"
    "<script>const NEVER=4294967295;"
    "function hostTime(){const d=new Date();"
    "fetch('/api/host-time?unix='+Math.floor(d.getTime()/1000)+'&tz='+(-d.getTimezoneOffset()))"
    ".catch(()=>{});}"
    "async function up(){try{const r=await fetch('/api/state');const s=await r.json();"
    "const age=(s.ping_age_s===NEVER)?'ни разу':(s.ping_age_s+' с назад');"
    "const fb=s.fallback?(s.fallback_left_s+' с'):'выключен';"
    "const host=s.host_name?(s.host_name+' ('+s.host+')'):s.host;"
    "const tm=s.time_valid?(s.time+' '+s.tsrc):'нет (нужен NTP на хосте или открытая страница)';"
    "document.getElementById('t').innerHTML="
    "`<tr><td>Прошивка</td><td>${s.fw}</td></tr>`+"
    "`<tr><td>Аптайм</td><td>${s.up} с</td></tr>`+"
    "`<tr><td>Свободно памяти</td><td>${Math.round(s.heap/1024)} КБ</td></tr>`+"
    "`<tr><td>USB-сеть</td><td>${s.link}</td></tr>`+"
    "`<tr><td>Адрес прибора</td><td>${s.ip}</td></tr>`+"
    "`<tr><td>Хост</td><td>${host}</td></tr>`+"
    "`<tr><td>Время хоста</td><td>${tm}</td></tr>`+"
    "`<tr><td>Ответ ping</td><td>${age} (${s.ping_ok} ок / ${s.ping_fail} таймаутов)</td></tr>`+"
    "`<tr><td>Задержка</td><td>${s.rtt_last} мс (min ${s.rtt_min} / avg ${s.rtt_avg} / max ${s.rtt_max}), потери ${s.loss}%</td></tr>`+"
    "`<tr><td>Канал</td><td>RX ${s.rx_rate}/с ${s.rx_kbs} КБ/с · TX ${s.tx_rate}/с ${s.tx_kbs} КБ/с</td></tr>`+"
    "`<tr><td>Разрывы USB</td><td>${s.reconnects}</td></tr>`+"
    "`<tr><td>Кадры от хоста</td><td>${s.frames} (DHCP ${s.dhcp})</td></tr>`+"
    "`<tr><td>Запросы к серверу</td><td>${s.http}</td></tr>`+"
    "`<tr><td>Сервисы хоста</td><td>${s.svc}</td></tr>`+"
    "`<tr><td>Веб-сервер хоста</td><td>${s.http_ok?(s.http_code+' за '+s.http_ms+' мс'):'нет ответа'}</td></tr>`+"
    "`<tr><td>Молчание хоста</td><td>${s.silence_s} с</td></tr>`+"
    "`<tr><td>Уход в загрузчик</td><td>${fb}</td></tr>`;"
    "}catch(e){document.getElementById('t').innerHTML="
    "'<tr><td>нет связи с прибором</td><td>'+e+'</td></tr>';}}"
    "hostTime();up();setInterval(up,2000);setInterval(hostTime,60000);</script></body></html>";

/* Кто к нам пришёл — это и есть «хост». В режиме моста (прибор в локальной сети)
   узнать адрес ПК иначе нельзя: DHCP-обмена с ним больше нет, а ARP-подсказка
   ловит любой узел. Браузер на хосте открыл нашу страницу — вот его адрес. */
static void note_host_from_request(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return;
    }
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &len) != 0) {
        return;
    }
    uint32_t a = ntohl(sa.sin_addr.s_addr);
    char ip[16];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)((a >> 24) & 0xFF),
             (unsigned)((a >> 16) & 0xFF), (unsigned)((a >> 8) & 0xFF), (unsigned)(a & 0xFF));
    if (strcmp(ip, net_ip_str()) == 0 || strncmp(ip, "127.", 4) == 0) {
        return;                       /* это мы сами, не хост */
    }
    selfcheck_set_host(ip);
    probe_set_host(ip);
    timesync_set_host(ip);
}

static esp_err_t index_get(httpd_req_t *req)
{
    selfcheck_http_hit();
    note_host_from_request(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Перезагрузка в режим загрузчика по HTTP — чтобы перепрошивать без BOOT и без USB-дёрганья.
   rom-флаг FORCE_DOWNLOAD_BOOT действует до сброса, поэтому загрузчик поднимается сразу. */
static esp_err_t boot_get(httpd_req_t *req)
{
    httpd_resp_send(req, "ok: ухожу в загрузчик\n", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "перезагрузка в загрузчик по HTTP-запросу");
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    return ESP_OK;
}

static esp_err_t state_get(httpd_req_t *req)
{
    selfcheck_http_hit();          /* хост дотянулся до нас — это признак жизни */
    note_host_from_request(req);
    char json[1024];
    char svc[128] = "";
    esp_netif_ip_info_t ip = {0};
    if (s_netif) {
        esp_netif_get_ip_info(s_netif, &ip);
    }
    selfcheck_status_t sc;
    selfcheck_status(&sc);
    selfcheck_ping_stat_t ps;
    selfcheck_ping_stats(&ps);
    probe_state_t pr;
    probe_get(&pr);
    for (int i = 0; i < 6; i++) {
        char one[24];
        snprintf(one, sizeof(one), "%s%s", i ? " " : "", pr.svc[i].name ? pr.svc[i].name : "?");
        strlcat(svc, one, sizeof(svc));
        strlcat(svc, pr.svc[i].open ? ":ok" : ":-", sizeof(svc));
    }
    char tstr[16], dstr[16], tsrc[8];
    timesync_time_str(tstr, sizeof(tstr));
    timesync_date_str(dstr, sizeof(dstr));
    bool time_ok = timesync_source(tsrc, sizeof(tsrc));

    int n = snprintf(json, sizeof(json),
                     "{\"fw\":\"%s\",\"up\":%lld,\"heap\":%u,\"link\":\"%s\",\"ip\":\"" IPSTR "\","
                     "\"host\":\"%s\",\"host_name\":\"%s\",\"host_known\":%u,"
                     "\"time\":\"%s\",\"date\":\"%s\",\"tsrc\":\"%s\",\"time_valid\":%u,"
                     "\"silence_s\":%u,\"fallback_left_s\":%u,"
                     "\"ping_ok\":%u,\"ping_fail\":%u,\"ping_age_s\":%u,"
                     "\"rtt_last\":%u,\"rtt_min\":%u,\"rtt_avg\":%u,\"rtt_max\":%u,\"loss\":%u,"
                     "\"rx_rate\":%u,\"tx_rate\":%u,\"rx_kbs\":%u,\"tx_kbs\":%u,\"reconnects\":%u,"
                     "\"http\":%u,\"frames\":%u,\"dhcp\":%u,\"http_up\":%u,\"fallback\":%u,"
                     "\"svc\":\"%s\",\"http_ok\":%u,\"http_code\":%d,\"http_ms\":%u}",
                     FW_VERSION, esp_timer_get_time() / 1000000,
                     (unsigned)esp_get_free_heap_size(),
                     s_link_up ? "поднята" : "нет",
                     IP2STR(&ip.ip),
                     sc.host, selfcheck_host_name(), (unsigned)sc.host_known,
                     tstr, dstr, tsrc, (unsigned)(time_ok ? 1 : 0),
                     (unsigned)sc.silence_s, (unsigned)sc.fallback_left_s,
                     (unsigned)sc.ping_ok, (unsigned)sc.ping_fail, (unsigned)sc.ping_age_s,
                     (unsigned)ps.last_ms, (unsigned)ps.min_ms, (unsigned)ps.avg_ms,
                     (unsigned)ps.max_ms, (unsigned)ps.loss_pct,
                     (unsigned)(s_rx_frames), (unsigned)(s_tx_frames),
                     (unsigned)(s_rx_bytes / 1024), (unsigned)(s_tx_bytes / 1024),
                     (unsigned)s_reconnects,
                     (unsigned)sc.http_hits, (unsigned)sc.host_frames, (unsigned)sc.dhcp_frames,
                     sc.http_up ? 1u : 0u, (unsigned)(SELFCHECK_ENABLED ? 1 : 0),
                     svc, pr.http_ok ? 1u : 0u, pr.http_code, (unsigned)pr.http_ms);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
}

/* Подсказка времени от браузера хоста: страница открыта на хосте — берём его часы.
   Нужна потому, что на стоковой Windows служба w32time не отдаёт NTP (проверено
   16.09: UDP 123 не отвечает), а время на экране нужно. */
static esp_err_t hosttime_get(httpd_req_t *req)
{
    selfcheck_http_hit();
    char q[96];
    uint64_t unix_s = 0;
    int tz_min = 180;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(q, "unix", val, sizeof(val)) == ESP_OK) {
            unix_s = strtoull(val, NULL, 10);
        }
        if (httpd_query_key_value(q, "tz", val, sizeof(val)) == ESP_OK) {
            tz_min = atoi(val);
        }
    }
    timesync_set_from_host(unix_s, tz_min);
    return httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
}

static void start_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_get };
    httpd_uri_t uri_state = { .uri = "/api/state", .method = HTTP_GET, .handler = state_get };
    httpd_uri_t uri_boot  = { .uri = "/api/boot", .method = HTTP_GET, .handler = boot_get };
    httpd_uri_t uri_time  = { .uri = "/api/host-time", .method = HTTP_GET, .handler = hosttime_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_state));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_boot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_time));
    selfcheck_http_up(true);
    ESP_LOGI(TAG, "HTTP-сервер поднят: http://" USB_NET_IP "/");
}

/* ------------------------------------------------------------- диагностика USB */
/* Консоль на USB-Serial/JTAG запрещена вместе с TinyUSB (общий USB-PHY), а UART
   наружу не выведен — при падении инициализации USB логов не видно вообще.
   Поэтому состояние USB-блока пишется в «чёрный ящик» (раздел diag, см. diag.c).
   GSNPSID (0x60080040) читается только при включённых часах USB-контроллера:
   если там не 0x4F54xxxx, ядро DWC2 недоступно — вопрос в PHY/часах, а не в
   стеке TinyUSB. */
#define DWC2_BASE        0x60080000u  /* USB-OTG (DWC2) на ESP32-S3: USB_DWC из esp32s3.peripherals.ld */
#define DWC2_GSNPSID_REG (DWC2_BASE + 0x40)

static void usb_dump(const char *where)
{
    uint32_t id = REG_READ(DWC2_GSNPSID_REG);
    uint32_t wrap = REG_READ(USB_WRAP_OTG_CONF_REG);
    uint32_t usj = REG_READ(USB_SERIAL_JTAG_CONF0_REG);
    ESP_LOGI(TAG, "USB[%s]: GSNPSID=0x%08x WRAP.otg_conf=0x%08x USJ.conf0=0x%08x",
             where, (unsigned)id, (unsigned)wrap, (unsigned)usj);
    /* в «чёрный ящик» — отдельно: уровень INFO в него не попадает */
    diag_step("USB[%s] GSNPSID=0x%08x WRAP.otg_conf=0x%08x USJ.conf0=0x%08x",
              where, (unsigned)id, (unsigned)wrap, (unsigned)usj);
}

/* PHY поднимаем сами и ОСТАВЛЯЕМ СЕБЕ (esp_tinyusb узнаёт об этом через
   .phy.skip_setup = true). Так проверено опытом: создание и удаление PHY по кругу
   валит плату — лог обрывается ровно на usb_del_phy(), причина сброса
   interrupt-wdt (169→188 загрузок подряд). Регистры при этом показывают, что
   после usb_new_phy ядро DWC2 отвечает: GSNPSID=0x4f54400a, USB_WRAP.otg_conf
   = 0x001c0000 — то есть часы и PHY в порядке, дело дальше. */
static esp_err_t usb_phy_setup(void)
{
    usb_dump("до PHY");
    usb_phy_config_t conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    usb_phy_handle_t phy = NULL;
    esp_err_t err = usb_new_phy(&conf, &phy);
    diag_step("usb_new_phy → %s", esp_err_to_name(err));
    usb_dump("после PHY");
    return err;
}

/* События USB-стека: хост подключился/отключился. По ним видно, перечисляется ли
   устройство заново (у нас после первого успеха хост один раз терял RNDIS-адаптер). */
static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    const char *name = "прочее";
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        name = "хост подключился";
        /* Хост подключился — с этого момента кадры можно и принимать, и отдавать.
           Раньше s_link_up просто выставлялся после установки стека, и при
           отключении хоста lwIP об этом не узнавал. */
        if (s_netif) {
            esp_netif_action_connected(s_netif, NULL, 0, NULL);
        }
        s_link_up = true;
        selfcheck_set_link(true);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        name = "хост отключился";
        if (s_netif) {
            esp_netif_action_disconnected(s_netif, NULL, 0, NULL);
        }
        s_link_up = false;
        s_reconnects++;
        selfcheck_set_link(false);
    }
    ESP_LOGI(TAG, "USB-событие: %s", name);
    diag_step("USB-событие: %s (порт %u)", name, (unsigned)event->rhport);
}

/* ------------------------------------------------------------------- USB-сеть */
/* Подробные логи сети — в «чёрный ящик» (diag.c сохраняет строки этих тегов в любом
   уровне). Нужно, чтобы видеть обмен DHCP, когда хост не получает адрес. */
static void enable_verbose_tags(void)
{
    static const char *tags[] = {
        "esp_netif", "esp_netif_lwip", "dhcps", "lwip", "tusb_net", "tinyusb_task", "tusb_desc"
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], ESP_LOG_DEBUG);
    }
    diag_step("включены подробные логи сети (esp_netif/dhcps/tusb_net)");
}

/* Адрес прибора: сначала просим у роутера локальной сети (DHCP-клиент), если за
   NET_DHCP_TIMEOUT_MS адреса нет — аварийный режим: свой адрес 192.168.7.1 и
   DHCP-сервер для хоста, чтобы прибор остался доступным (см. docs/addressing.md). */
static void net_addr_task(void *arg)
{
    (void)arg;

    for (int i = 0; i < NET_DHCP_TIMEOUT_MS / 500; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_netif_ip_info_t ip = {0};
        if (!s_netif) {
            continue;
        }
        if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            s_net_mode = NET_MODE_ROUTER;
            net_info_refresh();
            ESP_LOGI(TAG, "адрес от роутера локальной сети: " IPSTR " (шлюз " IPSTR ")",
                     IP2STR(&ip.ip), IP2STR(&ip.gw));
            diag_step("адрес от роутера: " IPSTR ", шлюз " IPSTR, IP2STR(&ip.ip), IP2STR(&ip.gw));
            ping_inet_enable(true);          /* есть маршрут — можно пинговать интернет */
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGW(TAG, "адрес от роутера не получен за %d с — аварийный режим", NET_DHCP_TIMEOUT_MS / 1000);
    diag_step("DHCP-адрес не получен за %d с → аварийный режим", NET_DHCP_TIMEOUT_MS / 1000);
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t st = {0};
    esp_netif_str_to_ip4(USB_NET_IP, &st.ip);
    esp_netif_str_to_ip4(USB_NET_GW, &st.gw);
    esp_netif_str_to_ip4(USB_NET_MASK, &st.netmask);
    esp_netif_set_ip_info(s_netif, &st);
    esp_err_t err = esp_netif_dhcps_start(s_netif);
    s_net_mode = NET_MODE_EMERGENCY;
    net_info_refresh();
    ESP_LOGW(TAG, "аварийный режим: прибор " USB_NET_IP ", DHCP-сервер для хоста (%s)",
             esp_err_to_name(err));
    diag_step("аварийный режим: " USB_NET_IP " + DHCP-сервер — %s", esp_err_to_name(err));
    ping_inet_enable(false);                 /* маршрута в сеть нет */
    vTaskDelete(NULL);
}

static void start_usb_net(void)
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    /* Прибор — DHCP-КЛИЕНТ: адрес, шлюз и DNS выдаёт роутер локальной сети
       (запросы уходят через мост Windows на ПК). Аварийный статический адрес
       включается в net_addr_task, если адреса нет. */
    base.flags = ESP_NETIF_FLAG_AUTOUP;
    base.if_key = "USB_DEF";
    base.if_desc = "usb";
    base.route_prio = 90;
    base.get_ip_event = 0;
    base.lost_ip_event = 0;

    esp_netif_config_t cfg = {
        .base = &base,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    diag_step("start_usb_net: создаю netif");
    s_netif = esp_netif_new(&cfg);
    if (!s_netif) {
        ESP_LOGE(TAG, "не удалось создать сетевой интерфейс");
        diag_step("netif НЕ создан");
        return;
    }
    /* привязываем наш USB-драйвер (внутри вызовется usb_net_post_attach).
       ESP_ERROR_CHECK тут не ставим специально: паника перезагружала плату по
       кругу, и «чёрный ящик» нельзя было снять. */
    esp_err_t nerr = esp_netif_attach(s_netif, &s_usb_base);
    diag_step("esp_netif_attach → %s", esp_err_to_name(nerr));
    if (nerr != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach: %s", esp_err_to_name(nerr));
        return;
    }
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    esp_netif_action_connected(s_netif, NULL, 0, NULL);

    /* Просим адрес у роутера локальной сети. Если адреса не дадут (нет моста, ПК
       стоит отдельно) — net_addr_task через NET_DHCP_TIMEOUT_MS переведёт прибор
       в аварийный режим: свой адрес 192.168.7.1 + DHCP-сервер для хоста. */
    esp_err_t err = esp_netif_dhcpc_start(s_netif);
    bool dhcpc_ok = (err == ESP_OK) || (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED);
    ESP_LOGI(TAG, "DHCP-клиент: %s", dhcpc_ok ? "запущен" : esp_err_to_name(err));
    diag_step("DHCP-клиент %s", dhcpc_ok ? "запущен" : esp_err_to_name(err));
    xTaskCreate(net_addr_task, "netaddr", 4096, NULL, 5, NULL);

#if CONFIG_TINYUSB_NET_MODE_NONE
    ESP_LOGW(TAG, "диагностический режим: USB-сеть выключена, логи в USB Serial/JTAG");
    s_link_up = true;
#else
    /* MAC берём из eFuse и делаем её локально администрируемой */
    esp_read_mac(tud_network_mac_address, ESP_MAC_WIFI_STA);
    tud_network_mac_address[0] = (uint8_t)((tud_network_mac_address[0] | 0x02) & 0xFE);
    esp_err_t merr = esp_netif_set_mac(s_netif, tud_network_mac_address);
    diag_step("esp_netif_set_mac → %s", esp_err_to_name(merr));

    /* сам USB-стек: сетевой класс (ECM/RNDIS) + наш драйвер для lwIP */
    esp_err_t perr = usb_phy_setup();
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY: %s — дальше идти незачем", esp_err_to_name(perr));
        return;
    }

    diag_step("ставлю TinyUSB (tinyusb_driver_install)");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.phy.skip_setup = true;      /* PHY уже поднят нами, см. usb_phy_setup() */
    /* Описатели задаём сами: для ECM/RNDIS esp_tinyusb не подставляет свои
       (они есть только для CDC/MSC/MTP/NCM), без них задача стека падает и
       наружу это видно только как ESP_ERR_TIMEOUT. */
    tusb_cfg.descriptor.device = &usb_desc_device;
    tusb_cfg.descriptor.full_speed_config = usb_desc_fs_config;
    tusb_cfg.descriptor.string = usb_desc_strings;
    tusb_cfg.descriptor.string_count = usb_desc_string_count;
    tusb_cfg.event_cb = usb_event_cb;
    esp_err_t usb_err = tinyusb_driver_install(&tusb_cfg);
    if (usb_err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s (0x%x) — USB-сеть не поднялась",
                 esp_err_to_name(usb_err), (unsigned)usb_err);
        diag_step("tinyusb_driver_install → %s (0x%x)", esp_err_to_name(usb_err), (unsigned)usb_err);
        usb_dump("после ошибки");
        return;
    }
    diag_step("TinyUSB поднят");
    s_link_up = true;
    ESP_LOGI(TAG, "USB-сеть поднята (MAC %02x:%02x:%02x:%02x:%02x:%02x): прошу адрес у роутера",
             tud_network_mac_address[0], tud_network_mac_address[1], tud_network_mac_address[2],
             tud_network_mac_address[3], tud_network_mac_address[4], tud_network_mac_address[5]);
#endif
}

void app_main(void)
{
    /* «чёрный ящик» — самым первым делом: если он не заведётся, дальше идти незачем */
    esp_err_t dg = diag_init(FW_VERSION);
    ESP_LOGI(TAG, "=== inkmetrics IDF %s: старт, сброс %d, чёрный ящик %s",
             FW_VERSION, (int)esp_reset_reason(), dg == ESP_OK ? "готов" : "НЕ ПОДНЯЛСЯ");
    diag_step("app_main: ящик → %s (heap %u)", esp_err_to_name(dg), (unsigned)esp_get_free_heap_size());

    /* Если прошлая загрузка закончилась паникой, строк её в ящике нет (они идут через
       ROM-консоль). Достаём причину из дампа: задача, PC, cause, обратный стек. */
    diag_report_panic();

    enable_verbose_tags();

    /* Снимаем «липкий» RTC-бит FORCE_DOWNLOAD_BOOT, если он остался от прошлого
       цикла (/api/boot или автоматического ухода в загрузчик): пока приложение
       работает, бит не нужен, а забытый бит отправит чип в загрузчик при
       следующем сбросе. */
    REG_WRITE(RTC_CNTL_OPTION1_REG, 0);
    selfcheck_init();

    boot_escape_check();
    diag_step("boot_escape_check пройден (BOOT не удержан)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    diag_step("NVS готова");

    esp_err_t ierr = esp_netif_init();
    diag_step("esp_netif_init → %s", esp_err_to_name(ierr));
    ESP_ERROR_CHECK(ierr);
    ierr = esp_event_loop_create_default();
    diag_step("event_loop_create_default → %s", esp_err_to_name(ierr));
    ESP_ERROR_CHECK(ierr);

    diag_step("вызываю start_usb_net");
    start_usb_net();
    diag_step("start_usb_net вернулся (link %s)", s_link_up ? "есть" : "нет");
    start_http();
    selfcheck_start(s_netif);   /* следим за хостом и уходим в загрузчик, если он молчит */
    ping_inet_init(PING_TARGET_NAME, PING_TARGET_IP);   /* пинг во внешнюю сеть (для экрана) */
    ui_init();                  /* панель, SHTC3 и кнопки: прибор начинает показывать состояние */

    ESP_LOGI(TAG, "готово: страница прибора откроется по адресу, который выдал роутер");
    diag_step("идём в главный цикл");

    /* дальше — экран, кнопки, SHTC3, метрики хоста (перенос из Arduino-версии) */
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        tick++;
        ESP_LOGI(TAG, "работаю: heap %u, up %lld с", (unsigned)esp_get_free_heap_size(),
                 esp_timer_get_time() / 1000000);
        ui_set_net_info(s_link_up, s_rx_frames, s_tx_frames,
                        s_rx_bytes, s_tx_bytes, s_reconnects);   /* экран видит состояние сети */
        if (tick % 2 == 0) {           /* раз в 10 с — строка в «чёрный ящик» */
            esp_netif_ip_info_t ip = {0};
            if (s_netif) {
                esp_netif_get_ip_info(s_netif, &ip);
            }
            diag_step("цикл: %u с, ip " IPSTR ", link %s, кадры приём %u / отдача %u, heap %u",
                      (unsigned)(esp_timer_get_time() / 1000000), IP2STR(&ip.ip),
                      s_link_up ? "есть" : "нет",
                      (unsigned)s_rx_frames, (unsigned)s_tx_frames,
                      (unsigned)esp_get_free_heap_size());
        }
    }
}
