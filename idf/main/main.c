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
#include "ingest.h"                 /* метрики хоста от агента (POST /ingest) */
#include "msc.h"                    /* диск хоста: прибор выглядит ещё и флешкой */
#include "ping_inet.h"              /* пинг во внешнюю сеть — то, что на экране */
#include "netinfo.h"                /* режим адресации: от роутера или аварийный */
#include "settings.h"               /* настройки прибора (поворот, блокировка диска, цель пинга) */
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define USB_NET_IP    "192.168.7.1"  /* АВАРИЙНЫЙ адрес прибора (см. net_addr_task) */
#define USB_NET_MASK  "255.255.255.0"
#define USB_NET_GW    "192.168.7.1"
#define USB_NET_MTU   1514           /* кадр без FCS: 14 (Ethernet) + 1500 (MTU) */
/* Сколько ждём адрес от роутера (раздача на ПК), прежде чем стать DHCP-сервером для хоста.
   Было 15 с: хост всё это время оставался без адреса, а Windows просит адрес не каждую
   секунду — чем позже поднимется наш сервер, тем позже появится страница. */
#define NET_DHCP_TIMEOUT_MS 6000
#define NET_DHCP_RETRY_MS   60000    /* в аварийном режиме — как часто пробуем снова */
#define NET_DHCP_RETRY_WINDOW_MS 8000   /* сколько ждём ответ в повторной попытке */
#define NET_WATCH_MS        2000     /* такт сторожа адресации (проверяем, не появился ли DHCP-сервер) */
#define PING_TARGET_NAME "ya.ru"     /* цель пинга по умолчанию (значение хранится в настройках) */
#define PING_TARGET_IP   "77.88.55.242"   /* запасной адрес, если DNS не отвечает */
#define BOOT_GPIO     0
#define FW_VERSION    "0.6.0-idf"

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
static volatile bool s_ip_conflict;      /* адрес совпал со шлюзом — сеть не работает */

net_mode_t net_mode(void) { return s_net_mode; }

bool net_from_router(void) { return s_net_mode == NET_MODE_ROUTER; }

const char *net_mode_text(void)
{
    if (s_ip_conflict) {
        return "IP CONFLICT";            /* адрес = шлюз: сеть не работает */
    }
    switch (s_net_mode) {
    case NET_MODE_ROUTER:    return "FROM ROUTER";
    case NET_MODE_EMERGENCY: return "EMERGENCY";
    default:                 return "NO ADDR";
    }
}

const char *net_ip_str(void) { return s_ip_str[0] ? s_ip_str : USB_NET_IP; }
const char *net_gw_str(void)  { return s_gw_str[0] ? s_gw_str : "-"; }

/* Версия прошивки — для экрана настроек и веб-страницы. */
const char *fw_version_str(void) { return FW_VERSION; }

static void net_info_refresh(void)
{
    esp_netif_ip_info_t ip = {0};
    bool conflict = false;
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ip.ip));
        snprintf(s_gw_str, sizeof(s_gw_str), IPSTR, IP2STR(&ip.gw));
        /* Свой адрес совпал со шлюзом — так бывает, когда раздача на ПК настроена
           наполовину: адрес выдал её DHCP, а шлюзом числится он же. Сеть в этом
           состоянии не работает: ни пинг, ни страница. Видно на экране (ADDR и GW). */
        /* Конфликт считаем ТОЛЬКО когда адрес дал внешний DHCP (режим ROUTER).
           В аварийном режиме адрес и шлюз совпадают ПО ЗАМЫСЛУ: шлюз для хоста — мы
           сами (192.168.7.1). Из-за этого 17.09 экран показывал «IP CONFLICT» вместо
           «EMERGENCY» и врал про причину отказа сети. */
        conflict = (s_net_mode == NET_MODE_ROUTER && ip.ip.addr != 0 && ip.ip.addr == ip.gw.addr);
    }
    if (conflict != s_ip_conflict) {
        s_ip_conflict = conflict;
        if (conflict) {
            ESP_LOGW(TAG, "адрес прибора совпал со шлюзом (" IPSTR ") — сеть не заработает,"
                     " прошу адрес заново", IP2STR(&ip.ip));
            diag_step("КОНФЛИКТ: адрес = шлюз = " IPSTR " — прошу адрес заново", IP2STR(&ip.ip));
        }
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

/* Признаки для смены адресации (см. net_addr_task):
   s_dhcp_answer  — в сети появился ЧУЖОЙ DHCP-сервер (раздача на ПК): он предложил
                    адрес именно нам, значит прибору есть смысл снова стать клиентом;
   s_renew_wanted — пользователь попросил обновить адрес (страница /api/renew). */
static volatile bool s_dhcp_answer;
static volatile bool s_renew_wanted;
static volatile uint32_t s_last_renew_s;   /* когда последний раз просили адрес по подсказке */

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

/* Чужой DHCP-сервер в сети (раздача интернета на ПК, роутер): в кадре от хоста
   приходит ответ DHCP (op=2, OFFER/ACK) на НАШ MAC. Это единственный надёжный признак,
   что «снаружи» кто-то готов дать нам адрес: гадать по таймеру нельзя (см. З-37). */
static void dhcp_answer_note(const uint8_t *f, uint16_t len)
{
    if (len < 14 + 20 + 8 + 44) {
        return;                                  /* короче кадра DHCP быть не может */
    }
    if (f[12] != 0x08 || f[13] != 0x00 || f[14 + 9] != 17) {
        return;                                  /* не IPv4/UDP */
    }
    uint16_t ihl = (uint16_t)((f[14] & 0x0Fu) * 4u);
    const uint8_t *udp  = f + 14 + ihl;
    const uint8_t *dhcp = udp + 8;
    if (udp[0] != 0x00 || udp[1] != 0x43) {       /* порт отправителя 67 = сервер */
        return;
    }
    if (dhcp[0] != 2) {                           /* 2 = BOOTREPLY (ответ сервера) */
        return;
    }
    if (memcmp(dhcp + 28, tud_network_mac_address, 6) != 0) {
        return;                                   /* предложение не нам */
    }
    if (!s_dhcp_answer) {
        s_dhcp_answer = true;
        ESP_LOGI(TAG, "в сети есть DHCP-сервер (ответ на наш MAC) — вернусь в клиента");
        diag_step("внешний DHCP-сервер ответил нам — попрошу адрес заново");
    }
}

/* хост прислал кадр — отдаём его в lwIP */
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    s_rx_frames++;
    s_rx_bytes += size;
    selfcheck_host_frame(src, size);   /* признак жизни хоста + разбор ARP/DHCP */
    dhcp_answer_note(src, size);       /* кто-то предлагает нам адрес по DHCP? */
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
/* Страница состояния: показывает то же, что на экране прибора (решения 16.09):
   режим адресации и адрес, пинг в интернет, датчик, хост и ОТКРЫТЫЕ порты, диск.
   Настройки — на отдельной странице /setup. */
static const char INDEX_HTML[] =
    "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>inkmetrics</title><style>"
    "body{font:14px/1.5 system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}"
    "h1{font-size:18px;margin:0 0 12px}table{border-collapse:collapse;width:100%;max-width:560px}"
    "td{padding:4px 8px;border-bottom:1px solid #333}td:last-child{text-align:right;color:#7fd}"
    "a{color:#fc6} .warn{color:#f88}"
    "</style></head><body><h1>inkmetrics — страница прибора</h1>"
    "<table id=\"t\"></table>"
    "<p><a href=\"/setup\">Настройки прибора</a> · "
    "<a href=\"/api/state\">API (JSON)</a> · "
    "<a href=\"/api/boot\">Уйти в режим загрузки сейчас</a> · "
    "<a href=\"/api/renew\">Просить адрес заново</a></p>"
    "<script>"
    "function hostTime(){const d=new Date();"
    "fetch('/api/host-time?unix='+Math.floor(d.getTime()/1000)+'&tz='+(-d.getTimezoneOffset()))"
    ".catch(()=>{});}"
    "async function up(){try{const r=await fetch('/api/state');const s=await r.json();"
    "const host=s.host_known?((s.host_name?s.host_name+' ('+s.host+')':s.host)):'не виден';"
    "const tm=s.time_valid?(s.time+' '+s.date):'ожидаю время от браузера';"
    "const ping=s.p_target?'пинг '+s.p_target+': '+(s.p_ok?(s.p_avg+' мс, потери '+s.p_loss+'%'):'нет ответа'):'-';"
    "const hist=s.p_hist?s.p_hist:'-';"
    "const ports=s.ports_checked?(s.ports?s.ports:'нет открытых портов'):'проверяю';"
    "const fmt=(v,u)=>v>=0?(v+u):'-';"
    "const im=s.ingest||{have:0};"
    "const met=im.have?(('CPU '+fmt(im.cpu,'%')+' RAM '+fmt(im.mem,'%')+' диск '+fmt(im.disk,'%'))"
    "+(im.gpu0>=0?(' GPU0 '+im.gpu0+'%'+(im.gpu0_temp>=0?(' '+im.gpu0_temp+'C'):'')):'')"
    "+(im.gpu1>=0?(' GPU1 '+im.gpu1+'%'+(im.gpu1_temp>=0?(' '+im.gpu1_temp+'C'):'')):'')"
    "+(im.cpu_temp>=0?(' TMP '+im.cpu_temp+'C'):'')"
    "+', '+im.age+' с назад'+(im.fresh?'':' (УСТАРЕЛО)')):'нет данных от агента';"
    "const met2=im.have?((im.ping_known?(im.ping_ok?('интернет хоста: есть, '+im.ping_ms+' мс до '+(im.ping_target||'-')):('интернет хоста: НЕТ (пинг '+(im.ping_target||'-')+' не отвечает)')):'интернет хоста: не проверялся')"
    "+' · TCP '+im.tcp+(im.up_h>=0?(' · аптайм '+im.up_h.toFixed(1)+' ч'):'')"
    "+' · SMART '+(im.smart?im.smart:'-')+(im.count?(' · приёмов '+im.count):'')):'-';"
    "document.getElementById('t').innerHTML="
    "`<tr><td>Режим сети</td><td>${s.mode_ru}</td></tr>`+"
    "`<tr><td>Адрес прибора</td><td>${s.ip}</td></tr>`+"
    "`<tr><td>Шлюз</td><td>${s.gw}</td></tr>`+"
    "`<tr><td>Интернет</td><td>${ping}</td></tr>`+"
    "`<tr><td>Последние отклики</td><td>${hist}</td></tr>`+"
    "`<tr><td>Температура / влажность</td><td>${s.sensor?(s.t.toFixed(1)+' C / '+s.rh.toFixed(0)+' %'):'нет датчика'}</td></tr>`+"
    "`<tr><td>Время</td><td>${tm}</td></tr>`+"
    "`<tr><td>Хост (кто открыл страницу)</td><td>${host}</td></tr>`+"
    "`<tr><td>Открытые порты хоста</td><td>${ports}</td></tr>`+"
    "`<tr><td>Метрики хоста (агент)</td><td>${met}</td></tr>`+"
    "`<tr><td>Хост: пинг, TCP, SMART</td><td>${met2}</td></tr>`+"
    "`<tr><td>Веб-сервер хоста</td><td>${s.http_ok?(s.http_code+' за '+s.http_ms+' мс'):'нет ответа'}</td></tr>`+"
    "`<tr><td>Диск прибора</td><td>${Math.round(s.disk_kb)} КБ, ${s.wlock?'только чтение':'чтение и запись'}</td></tr>`+"
    "`<tr><td>Поворот экрана</td><td>${s.rotation}°</td></tr>`+"
    "`<tr><td>Прошивка</td><td>${s.fw}</td></tr>`+"
    "`<tr><td>Аптайм / память</td><td>${s.up} с / ${Math.round(s.heap/1024)} КБ</td></tr>`+"
    "`<tr><td>USB-сеть</td><td>${s.link? 'поднята':'нет'} (разрывов ${s.reconnects})</td></tr>`;"
    "}catch(e){document.getElementById('t').innerHTML="
    "'<tr><td>нет связи с прибором</td><td>'+e+'</td></tr>';}}"
    "hostTime();up();setInterval(up,2000);setInterval(hostTime,60000);</script></body></html>";

/* Страница настроек: то же, что на экране SETTINGS, но с возможностью править.
   Меняется: поворот экрана, блокировка записи на диск, цель пинга. */
static const char SETUP_HTML_HEAD[] =
    "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>inkmetrics — настройки</title><style>"
    "body{font:14px/1.5 system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}"
    "h1{font-size:18px;margin:0 0 12px}form{max-width:420px}"
    "label{display:block;margin:14px 0 6px;color:#bbb}"
    "select,input[type=text]{width:100%;padding:8px;background:#1b1b1b;color:#eee;"
    "border:1px solid #444;border-radius:6px}"
    "button{margin-top:18px;padding:10px 16px;background:#2a6;border:0;border-radius:6px;"
    "color:#041;font-weight:600;cursor:pointer}a{color:#fc6}"
    ".ok{color:#7fd}.hint{color:#888;font-size:13px}"
    "</style></head><body><h1>Настройки прибора</h1>";

/* Список вариантов для двух крупных чисел сводного экрана. Набор одинаковый, разница
   только в том, что выбрано; подписи берём из settings.c, чтобы страница и лог
   называли одно и то же одинаково. */
static int slot_options(char *out, size_t len, uint8_t cur)
{
    int n = 0;
    for (uint8_t k = 0; k < SLOT_KIND_MAX && n < (int)len - 1; k++) {
        n += snprintf(out + n, len - (size_t)n, "<option value=\"%u\"%s>%s</option>",
                      (unsigned)k, k == cur ? " selected" : "", settings_slot_name(k));
    }
    return n;
}

static esp_err_t setup_get(httpd_req_t *req)
{
    static char page[3584];
    const settings_t *cfg = settings_get();
    char opts1[512], opts2[512];
    slot_options(opts1, sizeof(opts1), cfg->slot[0]);
    slot_options(opts2, sizeof(opts2), cfg->slot[1]);
    uint32_t disk_kb = 0;
    bool wlock = false;
    msc_info(NULL, &wlock, NULL, NULL);
    msc_info(NULL, NULL, NULL, NULL);
    {
        uint32_t sectors = 0;
        msc_info(&sectors, NULL, NULL, NULL);
        disk_kb = sectors * 512u / 1024u;
    }
    int n = snprintf(page, sizeof(page),
        "%s<form method=\"post\" action=\"/api/settings\">"
        "<label>Поворот экрана</label><select name=\"rot\">"
        "<option value=\"0\"%s>0° — как есть</option>"
        "<option value=\"90\"%s>90°</option>"
        "<option value=\"180\"%s>180° — вверх ногами</option>"
        "<option value=\"270\"%s>270°</option></select>"
        "<label>Крупное число слева</label><select name=\"slot1\">%s</select>"
        "<label>Крупное число справа</label><select name=\"slot2\">%s</select>"
        "<label>Цель пинга в интернете</label>"
        "<input type=\"text\" name=\"ping\" maxlength=\"31\" value=\"%s\">"
        "<label><input type=\"checkbox\" name=\"wlock\"%s> Диск прибора только для чтения</label>"
        "<button type=\"submit\">Сохранить</button></form>"
        "<p class=\"hint\">Диск: %u КБ, сейчас %s. Прошивка %s.</p>"
        "<p class=\"hint\">Крупные числа — то, что видно на сводном экране издалека."
        " «GPU: вторая, иначе первая» значит: вторая карта, а если она одна — первая.</p>"
        "<p class=\"hint\">Кнопками прибора тоже можно: на экране SETUP короткое BOOT — поворот,"
        " удержание BOOT — блокировка диска. PWR листает страницы.</p>"
        "<p><a href=\"/\">Состояние</a></p></body></html>",
        SETUP_HTML_HEAD,
        cfg->rotation == 0 ? " selected" : "",
        cfg->rotation == 90 ? " selected" : "",
        cfg->rotation == 180 ? " selected" : "",
        cfg->rotation == 270 ? " selected" : "",
        opts1,
        opts2,
        cfg->ping_target,
        cfg->disk_write_lock ? " checked" : "",
        (unsigned)disk_kb,
        wlock ? "только чтение" : "чтение и запись",
        fw_version_str());
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, n);
}

/* Разбор значения из тела формы (application/x-www-form-urlencoded). */
static void form_value(const char *body, const char *key, char *out, size_t len)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    size_t i = 0;
    if (!p) {
        out[0] = 0;
        return;
    }
    p += strlen(pat);
    while (*p && *p != '&' && i + 1 < len) {
        char c = *p++;
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && p[0] && p[1]) {
            char hex[3] = { p[0], p[1], 0 };
            c = (char)strtol(hex, NULL, 16);
            p += 2;
        }
        out[i++] = c;
    }
    out[i] = 0;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char body[256];
    char val[SETTINGS_PING_LEN];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }
    int r = httpd_req_recv(req, body, total);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    body[r] = 0;

    form_value(body, "rot", val, sizeof(val));
    if (val[0]) {
        settings_set_rotation((uint8_t)atoi(val));
    }
    form_value(body, "ping", val, sizeof(val));
    if (val[0]) {
        settings_set_ping_target(val);
    }
    form_value(body, "wlock", val, sizeof(val));
    settings_set_disk_write_lock(strcmp(val, "on") == 0 || strcmp(val, "1") == 0);
    form_value(body, "slot1", val, sizeof(val));
    if (val[0]) {
        settings_set_slot(0, (uint8_t)atoi(val));
    }
    form_value(body, "slot2", val, sizeof(val));
    if (val[0]) {
        settings_set_slot(1, (uint8_t)atoi(val));
    }

    ESP_LOGI(TAG, "настройки сохранены: поворот %u, диск %s, цель %s, экран: %s + %s",
             (unsigned)settings_get()->rotation,
             settings_get()->disk_write_lock ? "только чтение" : "чтение и запись",
             settings_get()->ping_target,
             settings_slot_name(settings_get()->slot[0]),
             settings_slot_name(settings_get()->slot[1]));
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/setup");
    return httpd_resp_send(req, "", 0);
}

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

    selfcheck_status_t sc;
    selfcheck_status(&sc);
    probe_state_t pr;
    probe_get(&pr);
    ping_inet_stat_t pi;
    ping_inet_get(&pi);
    const settings_t *cfg = settings_get();

    bool sensor_ok = false;
    float t = 0, rh = 0;
    ui_get_sensor(&sensor_ok, &t, &rh);

    /* метрики, присланные агентом хоста (POST /ingest, модуль ingest.c) */
    ingest_state_t im;
    ingest_get(&im);

    char tstr[16], dstr[16];
    timesync_time_str(tstr, sizeof(tstr));
    timesync_date_str(dstr, sizeof(dstr));
    bool time_ok = timesync_source(NULL, 0);

    /* последние отклики пинга строкой: "3 2 4 3" мс */
    char hist[48] = "";
    for (int i = 0; i < pi.hist_len && i < PING_INET_HIST; i++) {
        char one[10];
        snprintf(one, sizeof(one), "%s%u", i ? " " : "", (unsigned)pi.hist[i]);
        strlcat(hist, one, sizeof(hist));
    }

    uint32_t disk_sectors = 0;
    bool wlock = false;
    msc_info(&disk_sectors, &wlock, NULL, NULL);

    const char *mode_ru = "нет адреса";
    if (s_net_mode == NET_MODE_ROUTER) {
        mode_ru = "адрес получен (роутер или раздача с ПК)";
    } else if (s_net_mode == NET_MODE_EMERGENCY) {
        mode_ru = "аварийный (192.168.7.1, адрес выдаёт прибор)";
    }

    static char json[1900];
    int n = snprintf(json, sizeof(json),
        "{\"fw\":\"%s\",\"up\":%lld,\"heap\":%u,\"link\":%u,\"reconnects\":%u,"
        "\"mode\":\"%s\",\"mode_ru\":\"%s\",\"ip\":\"%s\",\"gw\":\"%s\","
        "\"p_target\":\"%s\",\"p_ok\":%u,\"p_avg\":%u,\"p_min\":%u,\"p_max\":%u,\"p_loss\":%u,"
        "\"p_hist\":\"%s\",\"p_ip\":\"%s\","
        "\"sensor\":%u,\"t\":%.1f,\"rh\":%.1f,"
        "\"time\":\"%s\",\"date\":\"%s\",\"time_valid\":%u,"
        "\"host\":\"%s\",\"host_name\":\"%s\",\"host_known\":%u,"
        "\"ports\":\"%s\",\"ports_checked\":%u,\"http_ok\":%u,\"http_code\":%d,\"http_ms\":%u,"
        "\"disk_kb\":%u,\"wlock\":%u,\"rotation\":%u,\"slot1\":%u,\"slot2\":%u,"
        "\"silence_s\":%u,\"fallback_left_s\":%u,\"http\":%u,"
        "\"ingest\":{\"have\":%u,\"fresh\":%u,\"age\":%u,\"count\":%u,\"host\":\"%s\","
        "\"cpu\":%.0f,\"mem\":%.0f,\"disk\":%.0f,\"gpu_count\":%d,"
        "\"gpu0\":%.0f,\"gpu0_temp\":%d,\"gpu0_mem\":%.0f,"
        "\"gpu1\":%.0f,\"gpu1_temp\":%d,\"gpu1_mem\":%.0f,"
        "\"cpu_temp\":%d,\"ping_known\":%u,\"ping_ok\":%u,\"ping_ms\":%u,\"ping_target\":\"%s\","
        "\"up_h\":%.1f,\"tcp\":%d,\"smart\":\"%s\"}}",
        fw_version_str(), esp_timer_get_time() / 1000000,
        (unsigned)esp_get_free_heap_size(), s_link_up ? 1u : 0u, (unsigned)s_reconnects,
        net_mode_text(), mode_ru, net_ip_str(), net_gw_str(),
        pi.target ? pi.target : "-", pi.ok ? 1u : 0u, (unsigned)pi.avg_ms, (unsigned)pi.min_ms,
        (unsigned)pi.max_ms, (unsigned)pi.loss_pct, hist, pi.ip,
        (unsigned)(sensor_ok ? 1 : 0), (double)t, (double)rh,
        tstr, dstr, (unsigned)(time_ok ? 1 : 0),
        sc.host, selfcheck_host_name(), (unsigned)sc.host_known,
        pr.ports, (unsigned)(pr.checked ? 1 : 0), pr.http_ok ? 1u : 0u, pr.http_code,
        (unsigned)pr.http_ms,
        (unsigned)(disk_sectors * 512u / 1024u), wlock ? 1u : 0u, (unsigned)cfg->rotation,
        (unsigned)cfg->slot[0], (unsigned)cfg->slot[1],
        (unsigned)sc.silence_s, (unsigned)sc.fallback_left_s, (unsigned)sc.http_hits,
        (unsigned)(im.have ? 1 : 0), (unsigned)(im.fresh ? 1 : 0), (unsigned)im.age_s,
        (unsigned)im.count, im.host,
        (double)im.cpu_pct, (double)im.mem_pct, (double)im.disk_pct,
        im.gpu_count,
        (double)im.gpu_pct[0], im.gpu_temp_c[0], (double)im.gpu_mem_pct[0],
        (double)im.gpu_pct[1], im.gpu_temp_c[1], (double)im.gpu_mem_pct[1],
        im.cpu_temp_c, (unsigned)(im.ping_known ? 1 : 0), (unsigned)im.ping_ok,
        (unsigned)im.ping_ms, im.ping_target, (double)im.up_h,
        (int)im.tcp_est, im.smart);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
}

/* «Проси адрес заново» — кнопка на странице прибора: нужна, когда раздачу на ПК
   включили позже (скриптом с диска прибора), а USB передёргивать не хочется. */
/* Подсказка времени от браузера хоста: страница открыта на хосте — берём его часы.
   Нужна потому, что на стоковой Windows служба w32time не отдаёт NTP (проверено
   16.09: UDP 123 не отвечает), а время на экране нужно. */
static esp_err_t renew_get(httpd_req_t *req)
{
    selfcheck_http_hit();
    s_renew_wanted = true;
    return httpd_resp_send(req, "ok: прошу адрес заново\n", HTTPD_RESP_USE_STRLEN);
}

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

/* ------------------------------------------------------- метрики хоста (агент) */
/* POST /ingest — сюда агент на хосте (agent.ps1) раз в минуту шлёт плоский JSON с тем,
   что по сети не узнать: загрузка CPU, память, диск, температуры двух карт. Разбор — в
   ingest.c; показ — сводный экран прибора (температуры и проценты) и страница прибора. */
static char s_ingest_body[INGEST_BODY_LEN];

static esp_err_t ingest_post(httpd_req_t *req)
{
    selfcheck_http_hit();          /* агент постучался — это признак жизни хоста */
    note_host_from_request(req);

    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(s_ingest_body)) {
        ESP_LOGW(TAG, "POST /ingest: негодная длина тела (%d)", total);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, s_ingest_body + got, total - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;              /* хост медлит — ждём остаток тела */
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        got += r;
    }
    s_ingest_body[got] = 0;
    ingest_apply(s_ingest_body);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
}

static void start_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_get };
    httpd_uri_t uri_setup = { .uri = "/setup", .method = HTTP_GET, .handler = setup_get };
    httpd_uri_t uri_sset  = { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post };
    httpd_uri_t uri_state = { .uri = "/api/state", .method = HTTP_GET, .handler = state_get };
    httpd_uri_t uri_boot  = { .uri = "/api/boot", .method = HTTP_GET, .handler = boot_get };
    httpd_uri_t uri_time  = { .uri = "/api/host-time", .method = HTTP_GET, .handler = hosttime_get };
    httpd_uri_t uri_renew = { .uri = "/api/renew", .method = HTTP_GET, .handler = renew_get };
    httpd_uri_t uri_ing   = { .uri = "/ingest", .method = HTTP_POST, .handler = ingest_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_setup));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_sset));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_state));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_boot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_time));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_renew));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_ing));
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

/* Адрес прибора. Порядок (решения 16.09, см. docs/addressing.md и docs/plan-usb-installer.md):
     1. просим адрес у роутера (обычный DHCP-клиент) — это основной способ;
     2. адреса нет за NET_DHCP_TIMEOUT_MS — аварийный режим: свой 192.168.7.1 и DHCP-сервер
        для хоста, чтобы прибор остался доступным;
     3. в аварийном режиме раз в NET_DHCP_RETRY_MS пробуем снова: раздачу (ICS) на ПК могли
        включить уже после загрузки прибора (в том числе скриптом с его же диска). Повтор
        делаем только если страницу прибора не открывали NET_RETRY_IF_IDLE_S секунд — на время
        попытки аварийный адрес 192.168.7.1 пропадает. */
/* Хост настроен на другую подсеть — значит адрес даёт он нам, а не мы ему. Так бывает
   после включения раздачи на ПК: его адаптер прибора становится 192.168.137.1, и наш
   192.168.7.1 в этой сети не виден ни ему, ни нам. Самоназначенный 169.254.x подсказкой
   НЕ считаем: это просто «на ПК адреса нет», и обслуживать его должны мы. */
static bool host_in_other_subnet(const char *ip)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!ip || sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    if (a == 169 && b == 254) {
        return false;                     /* APIPA */
    }
    if (a == 192 && b == 168 && c == 7) {
        return false;                     /* наша аварийная подсеть */
    }
    return true;
}

static bool lease_present(void)
{
    esp_netif_ip_info_t ip = {0};
    return s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

static bool wait_lease(int ms)
{
    for (int i = 0; i < ms / 500; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (lease_present()) {
            return true;
        }
    }
    return false;
}

static void enter_router_mode(void)
{
    esp_netif_ip_info_t ip = {0};
    if (s_netif) {
        esp_netif_get_ip_info(s_netif, &ip);
    }
    s_net_mode = NET_MODE_ROUTER;
    net_info_refresh();
    ESP_LOGI(TAG, "адрес получен: " IPSTR " (шлюз " IPSTR ")", IP2STR(&ip.ip), IP2STR(&ip.gw));
    diag_step("адрес получен: " IPSTR ", шлюз " IPSTR, IP2STR(&ip.ip), IP2STR(&ip.gw));
    ping_inet_enable(true);              /* есть маршрут — можно пинговать интернет */
}

static void enter_emergency_mode(void)
{
    esp_netif_ip_info_t st = {0};
    esp_netif_str_to_ip4(USB_NET_IP, &st.ip);
    esp_netif_str_to_ip4(USB_NET_GW, &st.gw);
    esp_netif_str_to_ip4(USB_NET_MASK, &st.netmask);
    /* Порядок обязателен: esp_netif_set_ip_info() отказывает
       (ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED), пока DHCP-сервер не остановлен —
       esp_netif_lwip.c:1981. Остановка «не запущенного» сервера ошибку не даёт. */
    esp_netif_dhcps_stop(s_netif);
    esp_err_t perr = esp_netif_set_ip_info(s_netif, &st);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "аварийный адрес не встал: %s", esp_err_to_name(perr));
        diag_step("аварийный адрес НЕ встал: %s", esp_err_to_name(perr));
    }
    esp_err_t err = esp_netif_dhcps_start(s_netif);
    s_net_mode = NET_MODE_EMERGENCY;
    net_info_refresh();
    ESP_LOGW(TAG, "аварийный режим: прибор " USB_NET_IP ", DHCP-сервер для хоста (%s)",
             esp_err_to_name(err));
    diag_step("аварийный режим: " USB_NET_IP " + DHCP-сервер — %s", esp_err_to_name(err));
    ping_inet_enable(false);             /* маршрута в сеть нет */
}

/* Повторная попытка: снимаем аварийную настройку и снова просим адрес по DHCP. */
static bool retry_dhcp(void)
{
    esp_netif_dhcps_stop(s_netif);
    esp_netif_ip_info_t zero = {0};
    esp_netif_set_ip_info(s_netif, &zero);
    esp_netif_dhcpc_start(s_netif);
    if (wait_lease(NET_DHCP_RETRY_WINDOW_MS)) {
        enter_router_mode();
        return true;
    }
    esp_netif_dhcpc_stop(s_netif);
    enter_emergency_mode();
    return false;
}

static void net_addr_task(void *arg)
{
    (void)arg;

    if (wait_lease(NET_DHCP_TIMEOUT_MS)) {
        enter_router_mode();
    } else {
        ESP_LOGW(TAG, "адрес от роутера не получен за %d с — аварийный режим",
                 NET_DHCP_TIMEOUT_MS / 1000);
        diag_step("DHCP-адрес не получен за %d с → аварийный режим", NET_DHCP_TIMEOUT_MS / 1000);
        esp_netif_dhcpc_stop(s_netif);
        enter_emergency_mode();
    }

    /* Сторож: адрес потеряли или раздачу на ПК включили позже — прибор должен подхватить
       её сам, без передёргивания USB (иначе он навсегда останется на 192.168.7.1). */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(NET_WATCH_MS));

        if (s_net_mode == NET_MODE_ROUTER) {
            if (lease_present() && !s_ip_conflict) {
                continue;
            }
            ESP_LOGW(TAG, "адрес пропал или конфликтует — прошу заново");
            diag_step("адрес пропал/конфликт → прошу заново");
            s_net_mode = NET_MODE_NONE;
            esp_netif_dhcpc_stop(s_netif);
            esp_netif_dhcpc_start(s_netif);
            if (wait_lease(NET_DHCP_RETRY_WINDOW_MS)) {
                enter_router_mode();
            } else {
                esp_netif_dhcpc_stop(s_netif);
                enter_emergency_mode();
            }
            continue;
        }

        /* АВАРИЙНЫЙ РЕЖИМ: свой адрес 192.168.7.1 и DHCP-сервер для хоста работают
           ПОСТОЯННО и ничего не гасим. Это и была причина «хост без адреса» (З-37):
           прежний повтор каждую минуту снимал сервер на 8 с, а после первой попытки он
           мог не подняться вовсе — окно, в которое хост мог получить адрес, было
           52 с из 60, а Windows просит адрес раз в несколько минут.
           В клиента возвращаемся только по делу: чужой DHCP-сервер ответил нам
           (s_dhcp_answer) либо пользователь попросил обновить адрес (s_renew_wanted). */
        /* Подсказка от хоста: он в другой подсети — просим адрес у него (не чаще раза
           в минуту, чтобы не дёргать сеть, если он адрес не даёт). */
        selfcheck_status_t hs;
        selfcheck_status(&hs);
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
        if (hs.host_known && host_in_other_subnet(hs.host) &&
            now_s - s_last_renew_s > 60u) {
            s_last_renew_s = now_s;
            s_renew_wanted = true;
            ESP_LOGI(TAG, "хост %s в другой подсети — прошу адрес заново", hs.host);
            diag_step("хост %s в другой подсети — прошу адрес заново", hs.host);
        }

        if (!s_dhcp_answer && !s_renew_wanted) {
            continue;
        }
        bool by_hand = s_renew_wanted;
        s_renew_wanted = false;
        s_dhcp_answer = false;
        ESP_LOGI(TAG, "прошу адрес заново (%s)", by_hand ? "просьба со страницы" : "ответил внешний DHCP");
        diag_step("прошу адрес заново: %s", by_hand ? "просьба со страницы" : "внешний DHCP-сервер");
        retry_dhcp();
    }
}

static void start_usb_net(void)
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    /* Прибор — DHCP-КЛИЕНТ: адрес, шлюз и DNS выдаёт роутер локальной сети
       (запросы уходят через мост Windows на ПК). Аварийный статический адрес
       включается в net_addr_task, если адреса нет. */
    /* Флаги принципиальны (нашли 17.09 по «чёрному ящику»):
       ESP_NETIF_INHERENT_DEFAULT_ETH() даёт DHCP_CLIENT, но мы флаги перезаписываем —
       и остаётся только AUTOUP. Без флага ESP_NETIF_DHCP_SERVER esp_netif при создании
       netif НЕ создаёт объект DHCP-сервера (esp_netif_lwip.c: dhcps_new() только под этим
       флагом), поэтому esp_netif_dhcps_start() всегда отвечал
       ESP_ERR_ESP_NETIF_DHCPS_START_FAILED («DHCP server cannot be started» в логе) —
       хост не получал адрес, отсюда и «нет пинга», и недоступная страница.
       Оба флага (CLIENT и SERVER) IDF запрещает («DHCP server and client cannot be
       configured together»), поэтому ставим SERVER, а DHCP-клиента включаем вызовом
       esp_netif_dhcpc_start(): он флага не требует и работает (esp_netif_lwip.c:1602). */
    base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
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
    settings_init();               /* настройки: поворот экрана, блокировка диска, цель пинга */
    msc_init();                    /* диск хоста: раздел msc отдаётся как флешка */

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
    ping_inet_init(settings_get()->ping_target, PING_TARGET_IP);  /* цель — из настроек */
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
