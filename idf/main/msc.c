/*
 * Диск хоста: прибор отдаёт его как обычную флешку (USB MSC), рядом с сетевой картой.
 *
 * Зачем (замысел пользователя 16.09, см. docs/plan-usb-installer.md): на диске лежит
 * `setup.cmd`, который настраивает ПК — включает раздачу интернета на адаптере прибора.
 * Тогда порядок на любом ПК один: воткнул прибор → появился диск → запустил `setup.cmd`.
 *
 * Устройство диска:
 *   * данные берём из раздела `msc` во флеше прибора (то же место, что диск Arduino-версии);
 *   * отдаём ровно 2880 секторов (1,44 МБ) — Windows не монтирует том без таблицы
 *     разделов больше этого (проверено, см. MEMORY.md, таблица ошибок);
 *   * диск на чтение И на запись; блокировка записи включается в настройках прибора
 *     (экран SETTINGS / веб-страница `/setup`) — при блокировке хост видит диск
 *     защищённым от записи (TinyUSB сообщает это сам, см. tud_msc_is_writable_cb).
 */
#include "msc.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "settings.h"
#include "tusb.h"

#define MSC_PART_LABEL "msc"
#define MSC_SECTORS    2880u          /* 1,44 МБ = 80*2*18 */
#define MSC_SECTOR     512u

static const char *TAG = "msc";
static const esp_partition_t *s_part;
static volatile uint32_t s_reads, s_writes;
static volatile bool s_write_block_logged;

void msc_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      MSC_PART_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "раздел %s не найден — диска у хоста не будет", MSC_PART_LABEL);
        return;
    }
    if (s_part->size < MSC_SECTORS * MSC_SECTOR) {
        ESP_LOGW(TAG, "раздел %s мал (%u Б), отдаю сколько есть", MSC_PART_LABEL,
                 (unsigned)s_part->size);
    }
    ESP_LOGI(TAG, "диск хоста: раздел %s, смещение 0x%lx, отдаю %u секторов (%u КБ)",
             MSC_PART_LABEL, (unsigned long)s_part->address,
             (unsigned)MSC_SECTORS, (unsigned)(MSC_SECTORS * MSC_SECTOR / 1024));
}

void msc_info(uint32_t *sectors, bool *write_locked, uint32_t *reads, uint32_t *writes)
{
    if (sectors) {
        *sectors = MSC_SECTORS;
    }
    if (write_locked) {
        *write_locked = settings_get()->disk_write_lock;
    }
    if (reads) {
        *reads = s_reads;
    }
    if (writes) {
        *writes = s_writes;
    }
}

/* ------------------------------------------------------------------ TinyUSB MSC */

/* Размер диска: постоянный, поэтому отдаём константой. */
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = MSC_SECTORS;
    *block_size = MSC_SECTOR;
}

/* Можно ли писать: при включённой блокировке говорим «нет» — тогда и Windows, и Linux
   показывают диск защищённым от записи, а не «ошибка записи» посреди копирования. */
bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    bool locked = settings_get()->disk_write_lock;
    if (locked && !s_write_block_logged) {
        s_write_block_logged = true;
        ESP_LOGW(TAG, "диск заблокирован на запись (настройка прибора)");
    }
    if (!locked) {
        s_write_block_logged = false;
    }
    return !locked;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!s_part || lba >= MSC_SECTORS) {
        return -1;
    }
    esp_err_t err = esp_partition_read(s_part, (size_t)lba * MSC_SECTOR + offset, buffer, bufsize);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "чтение диска: %s", esp_err_to_name(err));
        return -1;
    }
    s_reads++;
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!s_part || lba >= MSC_SECTORS) {
        return -1;
    }
    if (settings_get()->disk_write_lock) {
        return -1;                     /* блокировка записи: настройка прибора */
    }
    esp_err_t err = esp_partition_write(s_part, (size_t)lba * MSC_SECTOR + offset, buffer, bufsize);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "запись на диск: %s", esp_err_to_name(err));
        return -1;
    }
    s_writes++;
    return (int32_t)bufsize;
}

/* Команды, которые стек отдаёт приложению: INQUIRY и «готовность носителя». */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)lun;
    int32_t ret = -1;

    switch (scsi_cmd[0]) {
    case SCSI_CMD_TEST_UNIT_READY:
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        ret = 0;                        /* диск на месте и всегда готов */
        break;

    case SCSI_CMD_INQUIRY: {
        if (bufsize < sizeof(scsi_inquiry_resp_t)) {
            return -1;
        }
        scsi_inquiry_resp_t *r = (scsi_inquiry_resp_t *)buffer;
        memset(r, 0, sizeof(*r));
        r->peripheral_device_type = 0x00;      /* прямой доступ к блокам */
        memcpy(r->vendor_id, "inkmetrics", 7);
        memcpy(r->product_id, "monitor disk", 12);
        memcpy(r->product_rev, "1.0", 3);
        r->response_data_format = 0x02;        /* ответ в формате SPC, без доп. данных */
        ret = (int32_t)sizeof(scsi_inquiry_resp_t);
        break;
    }

    default:
        ret = -1;                       /* остальное: MODE SENSE и прочее — не поддерживаем */
        break;
    }
    return ret;
}
