/*
 * Описатели USB-устройства для сетевого режима ECM/RNDIS.
 *
 * Зачем свой файл: esp_tinyusb (2.3.0) подставляет описатели по умолчанию только
 * для классов CDC, MSC, MTP и NCM — для ECM/RNDIS конфигурационный описатель обязан
 * задать пользователь (см. descriptors_control.c: «Full-speed configuration
 * descriptor must be provided for this device»). Без него задача стека падает на
 * старте, а наружу это выглядит только как ESP_ERR_TIMEOUT из
 * tinyusb_driver_install — искать причину пришлось через «чёрный ящик».
 *
 * Состав конфигурации — как в примере TinyUSB net_lwip_webserver: один интерфейс
 * RNDIS (IAD + CDC control + CDC data). Windows понимает его своим драйвером
 * RNDIS, который уже есть в системе; ECM для Windows не годится, поэтому вместо
 * двух конфигураций (RNDIS + ECM) отдаём одну — RNDIS. Две конфигурации через
 * esp_tinyusb всё равно не выставить: он поддерживает ровно одну.
 */
#include "usb_desc.h"

#define USB_VID 0x303A            /* Espressif */
#define USB_PID 0x4020            /* 0x4000 | бит ECM_RNDIS по раскладке ID TinyUSB */

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_COUNT
};

enum {
    ITF_NUM_RNDIS = 0,
    ITF_NUM_DATA = 1,
    ITF_NUM_TOTAL = 2
};

#define EPNUM_NOTIF 0x81
#define EPNUM_OUT   0x02
#define EPNUM_IN    0x82

const tusb_desc_device_t usb_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* Класс устройства — «misc / IAD», как требует устройство с ассоциацией интерфейсов */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t usb_desc_fs_config[] = {
    /* номер конфигурации, число интерфейсов, индекс строки, длина, атрибуты, ток (мА) */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0,
                          TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN, 0x00, 100),
    /* номер интерфейса, индекс строки, EP уведомлений и его размер, EP out/in и размер */
    TUD_RNDIS_DESCRIPTOR(ITF_NUM_RNDIS, STRID_INTERFACE,
                         EPNUM_NOTIF, 8, EPNUM_OUT, EPNUM_IN, 64),
};

static const char s_langid[] = {0x09, 0x04};   /* English (US) */

const char *usb_desc_strings[] = {
    s_langid,
    "inkmetrics",           /* производитель */
    "inkmetrics e-paper",   /* продукт */
    "1",                     /* серийный номер */
    "inkmetrics net",       /* интерфейс RNDIS */
};

const int usb_desc_string_count = STRID_COUNT;
