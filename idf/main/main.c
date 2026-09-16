/*
 * inkmetrics на ESP-IDF: сервер живёт в приборе.
 *
 * Что делает эта прошивка:
 *   1. Прибор представляется хосту сетевой картой (USB NCM). Никакого софта
 *      на хосте ставить не нужно — драйвер встроен в Windows/Linux/macOS.
 *   2. Прибор выдаёт хосту адрес по DHCP и имеет свой адрес 192.168.7.1.
 *   3. На приборе поднят HTTP-сервер: страница состояния и /api/state.
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

#define USB_NET_IP    "192.168.7.1"
#define USB_NET_MASK  "255.255.255.0"
#define USB_NET_GW    "192.168.7.1"
#define BOOT_GPIO     0
#define FW_VERSION    "0.3.0-idf"

static const char *TAG = "eink";
static esp_netif_t *s_netif = NULL;
static volatile bool s_link_up = false;

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

/* TinyUSB просит заполнить свой буфер кадром из lwIP */
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    struct pbuf *p = (struct pbuf *)ref;
    (void)arg;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void)
{
}

/* lwIP хочет отправить кадр — отдаём его стеку USB */
static esp_err_t usb_transmit(void *h, void *buffer, size_t len)
{
    struct pbuf *p = (struct pbuf *)buffer;
    if (!tud_network_can_xmit(p->tot_len)) {
        return ESP_ERR_NO_MEM;
    }
    tud_network_xmit(p, 0);
    s_tx_frames++;
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
    "h1{font-size:18px;margin:0 0 12px}table{border-collapse:collapse;width:100%;max-width:420px}"
    "td{padding:4px 8px;border-bottom:1px solid #333}td:last-child{text-align:right;color:#7fd}"
    "code{color:#fc6}"
    "</style></head><body><h1>inkmetrics — сервер в приборе</h1>"
    "<p>Страница отдана самим прибором по USB-сети. На хосте не установлено ничего:"
    " драйвер сетевой карты встроен в систему.</p>"
    "<table id=\"t\"></table>"
    "<p>Обновляется каждые 2 с. Этот же HTTP-сервер дальше будет отдавать"
    " страницу управления экраном, метрики ПК и настройки кнопок.</p>"
    "<script>async function up(){const r=await fetch('/api/state');const s=await r.json();"
    "document.getElementById('t').innerHTML="
    "`<tr><td>Прошивка</td><td>${s.fw}</td></tr>`+"
    "`<tr><td>Аптайм</td><td>${s.up} с</td></tr>`+"
    "`<tr><td>Свободно памяти</td><td>${Math.round(s.heap/1024)} КБ</td></tr>`+"
    "`<tr><td>USB-сеть</td><td>${s.link}</td></tr>`+"
    "`<tr><td>Адрес прибора</td><td>${s.ip}</td></tr>`;}"
    "up();setInterval(up,2000);</script></body></html>";

static esp_err_t index_get(httpd_req_t *req)
{
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
    char json[320];
    esp_netif_ip_info_t ip = {0};
    if (s_netif) {
        esp_netif_get_ip_info(s_netif, &ip);
    }
    int n = snprintf(json, sizeof(json),
                     "{\"fw\":\"%s\",\"up\":%lld,\"heap\":%u,\"link\":\"%s\",\"ip\":\"" IPSTR "\"}",
                     FW_VERSION, esp_timer_get_time() / 1000000,
                     (unsigned)esp_get_free_heap_size(),
                     s_link_up ? "поднята" : "нет",
                     IP2STR(&ip.ip));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_state));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_boot));
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
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        name = "хост отключился";
    }
    ESP_LOGI(TAG, "USB-событие: %s", name);
    diag_step("USB-событие: %s (порт %u)", name, (unsigned)event->rhport);
}

/* ------------------------------------------------------------------- USB-сеть */
static void start_usb_net(void)
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
    base.if_key = "USB_DEF";
    base.if_desc = "usb";
    base.route_prio = 90;
    base.get_ip_event = 0;
    base.lost_ip_event = 0;
    static esp_netif_ip_info_t s_ip_info;          /* должен жить, пока жив netif */
    esp_netif_str_to_ip4(USB_NET_IP, &s_ip_info.ip);
    esp_netif_str_to_ip4(USB_NET_GW, &s_ip_info.gw);
    esp_netif_str_to_ip4(USB_NET_MASK, &s_ip_info.netmask);
    base.ip_info = &s_ip_info;

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

    /* хост получит адрес от прибора (сам прибор — 192.168.7.1) */
    esp_err_t err = esp_netif_dhcps_start(s_netif);
    ESP_LOGI(TAG, "DHCP-сервер: %s", err == ESP_OK ? "поднят" : esp_err_to_name(err));
    diag_step("DHCP-сервер → %s", esp_err_to_name(err));

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
    ESP_LOGI(TAG, "USB-сеть поднята (MAC %02x:%02x:%02x:%02x:%02x:%02x), прибор на " USB_NET_IP,
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

    ESP_LOGI(TAG, "готово: подключи кабель и открой http://" USB_NET_IP "/");
    diag_step("идём в главный цикл");

    /* дальше — экран, кнопки, SHTC3, метрики хоста (перенос из Arduino-версии) */
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        tick++;
        ESP_LOGI(TAG, "работаю: heap %u, up %lld с", (unsigned)esp_get_free_heap_size(),
                 esp_timer_get_time() / 1000000);
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
