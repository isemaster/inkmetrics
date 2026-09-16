/*
 * «Чёрный ящик» прошивки на ESP-IDF.
 *
 * Зачем: у платы нет UART-адаптера, консоль на USB-Serial/JTAG запрещена вместе
 * с TinyUSB (общий USB-PHY), а USB-сеть отдаёт логи только когда уже поднялась.
 * Итог: при падении инициализации USB логов не видно вообще; дамп паники
 * (coredump) не помогает — в нём нет ни строк ESP_LOGE из задачи TinyUSB,
 * ни кодов esp_err.
 *
 * Как работает:
 *   1. ОЗУ не используем вовсе: каждая запись идёт прямо во флеш по адресу
 *      0x7E0000 (не через API разделов — он может вернуть NULL, и ящик молча
 *      перестанет писать).
 *   2. Пишем только дописыванием, кусками по 4 байта: каждый адрес пишется один
 *      раз, поэтому стирать между записями не нужно. Область стирается один раз
 *      в diag_init().
 *   3. В лог попадают метки стадий (diag_step), строки уровня WARN/ERROR
 *      (хук esp_log_set_vprintf) и начало загрузки с причиной сброса.
 *
 * Почему так, а не буфер в ОЗУ: если прошивка зависнет до записи, буфер в ОЗУ
 * не сохранится ничем — а поток во флеш остаётся даже при зависании.
 *
 * Чтение с хоста — tools/idf_diag.py (esptool read-flash 0x7E0000 0x4000).
 */
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DIAG_MAGIC  0x31474144u   /* "DAG1" */
#define DIAG_HDR    0x40          /* заголовок (4 слова) + запас */
#define DIAG_ERASE  (8 * 1024)    /* сколько стираем и сколько отводим под лог */
#define SECTOR      4096

static uint32_t s_off = DIAG_HDR; /* куда пишем следующую порцию */
static uint32_t s_boots;          /* номер этой загрузки */
static bool s_ready;
static bool s_full;
static vprintf_like_t s_prev_vprintf;

/* Дописывание во флеш: длина выравнивается до 4 байт.
   Хвост добиваем ПРОБЕЛАМИ, а не 0xFF: 0xFF остаётся признаком нестёртой флеша,
   и читалка (tools/idf_diag.py) по нему понимает, где кончается лог. С 0xFF в
   хвосте каждая отдельная запись обрывала чтение, и метки после первой строки
   не были видны (лог казался пустым на 66 байт). */
static void flash_append(const char *data, size_t len)
{
    if (!s_ready || s_full || len == 0) {
        return;
    }
    if (s_off + len + 4 > DIAG_ERASE) {
        s_full = true;                /* дальше писать некуда — молча останавливаемся */
        return;
    }
    uint8_t buf[256];
    while (len > 0) {
        size_t chunk = len > (sizeof(buf) - 4) ? (sizeof(buf) - 4) : len;
        size_t aligned = (chunk + 3u) & ~((size_t)3u);
        memset(buf, ' ', aligned);
        memcpy(buf, data, chunk);
        if (esp_flash_write(esp_flash_default_chip, buf, DIAG_FLASH_ADDR + s_off, aligned) != ESP_OK) {
            return;
        }
        s_off += (uint32_t)aligned;
        data += chunk;
        len -= chunk;
    }
}

void diag_step(const char *fmt, ...)
{
    char line[192];
    int n = snprintf(line, sizeof(line), "STEP ");
    if (n < 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m > 0) {
        n += m;
        line[n] = '\r';
        line[n + 1] = '\n';
        flash_append(line, (size_t)n + 2);
    }
}

void diag_flush(void)
{
    /* всё пишется потоком, буфера нет — функция оставлена для совместимости */
}

static int diag_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int n = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);

    char tmp[256];
    int m = vsnprintf(tmp, sizeof(tmp), fmt, copy);
    va_end(copy);

    /* строку уровня WARN/ERROR сохраняем сразу: следующей может уже не быть */
    if (m > 0 && (tmp[0] == 'E' || tmp[0] == 'W') && tmp[1] == ' ') {
        flash_append(tmp, m < (int)sizeof(tmp) ? (size_t)m : sizeof(tmp) - 1);
        flash_append("\r\n", 2);
    }
    return n;
}

esp_err_t diag_init(const char *fw_version)
{
    /* номер загрузки берём из прошлого заголовка — до стирания области */
    uint32_t prev[2] = {0, 0};
    esp_flash_read(esp_flash_default_chip, prev, DIAG_FLASH_ADDR, sizeof(prev));
    if (prev[0] == DIAG_MAGIC) {
        s_boots = prev[1];
    }
    s_boots++;

    /* Стираем по одному сектору с паузой между ними. Так нельзя стирать одним
       куском: esp_flash_erase_region держит кэш и прерывания выключенными на весь
       диапазон, а interrupt-watchdog IDF (300 мс по умолчанию) на этом и убивал
       прошивку — она уходила в цикл перезагрузок (наблюдали 169 загрузок подряд
       с причиной сброса interrupt-wdt, и «чёрный ящик» оставался пустым). */
    esp_err_t err = ESP_OK;
    for (uint32_t off = 0; off < DIAG_ERASE; off += SECTOR) {
        err = esp_flash_erase_region(esp_flash_default_chip, DIAG_FLASH_ADDR + off, SECTOR);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_ready = true;
    s_off = DIAG_HDR;

    uint32_t head[4] = {
        DIAG_MAGIC, s_boots, (uint32_t)esp_reset_reason(),
        (uint32_t)(esp_timer_get_time() / 1000)
    };
    esp_flash_write(esp_flash_default_chip, head, DIAG_FLASH_ADDR, sizeof(head));

    char line[160];
    int n = snprintf(line, sizeof(line), "===== inkmetrics %s: загрузка #%u, сброс %d\r\n",
                     fw_version ? fw_version : "?", (unsigned)s_boots, (int)esp_reset_reason());
    flash_append(line, (size_t)(n > 0 ? n : 0));

    s_prev_vprintf = esp_log_set_vprintf(diag_vprintf);
    return ESP_OK;
}
