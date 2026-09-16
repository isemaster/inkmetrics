/*
 * Экран прибора: обёртка над драйвером панели Waveshare + наши растровые шрифты.
 *
 * C++ здесь только потому, что драйвер Waveshare — класс; наружу отдаём обычные
 * C-функции (display.h), чтобы main.c остался на C.
 */
#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "epaper_driver_bsp.h"
#include "fonts.h"

#define EPD_PWR_GPIO   6      /* активный низкий: LOW = панель включена (см. docs/pins.md) */
#define EPD_CS        11
#define EPD_DC        10
#define EPD_RST        9
#define EPD_BUSY       8
#define EPD_MOSI      13
#define EPD_SCLK      12
#define FB_LEN      5000     /* 200*200/8 — ровно как в Arduino-версии */
#define DISP_FULL_EVERY 10   /* полное обновление каждое 10-е (стирает «чернила») */

static const char *TAG = "display";
static epaper_driver_display *s_drv;
static uint8_t *s_fb;
static bool s_ready;
static bool s_have_base;     /* базовый образ панели (0x26) записан */
static int  s_since_full;    /* сколько частичных обновлений после последнего полного */

static void fb_px(int x, int y, bool black)
{
    if (!s_fb || x < 0 || y < 0 || x >= DISP_W || y >= DISP_H) {
        return;
    }
    uint16_t idx = (uint16_t)(y * (DISP_W / 8) + (x >> 3));
    uint8_t bit = (uint8_t)(7 - (x & 7));
    if (black) {
        s_fb[idx] &= (uint8_t)~(1u << bit);
    } else {
        s_fb[idx] |= (uint8_t)(1u << bit);
    }
}

/* --------------------------------------------------------------- шрифты */
typedef struct {
    const void *table;
    int count;
    int cell_w;
    int cell_h;
    int bytes;
    bool big;
} font_ref_t;

static const font_ref_t FONT_SMALL = { font_small, FONT_SMALL_COUNT, FONT_SMALL_W,
                                       FONT_SMALL_H, FONT_SMALL_BYTES, false };
static const font_ref_t FONT_BIG   = { font_big, FONT_BIG_COUNT, FONT_BIG_W,
                                       FONT_BIG_H, FONT_BIG_BYTES, true };

/* В таблицах глифы отсортированы по коду — ищем двоичным поиском. */
static const uint8_t *font_lookup(const font_ref_t *f, uint16_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t cur;
        if (f->big) {
            cur = ((const font_big_glyph_t *)f->table)[mid].cp;
        } else {
            cur = ((const font_small_glyph_t *)f->table)[mid].cp;
        }
        if (cur == cp) {
            return f->big ? ((const font_big_glyph_t *)f->table)[mid].bits
                          : ((const font_small_glyph_t *)f->table)[mid].bits;
        }
        if (cur < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

/* UTF-8 → код символа; возвращает число прочитанных байт (0 — мусор). */
static int utf8_next(const char **s, uint16_t *cp)
{
    const unsigned char *p = (const unsigned char *)*s;
    if (!*p) {
        return 0;
    }
    if (p[0] < 0x80) {
        *cp = p[0];
        *s += 1;
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = (uint16_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
        *s += 2;
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = (uint16_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
        *s += 3;
        return 3;
    }
    *cp = 0;
    *s += 1;
    return 1;
}

static void draw_text(const font_ref_t *f, int x, int y, const char *utf8)
{
    while (*utf8) {
        uint16_t cp = 0;
        if (utf8_next(&utf8, &cp) == 0) {
            break;
        }
        const uint8_t *bits = font_lookup(f, cp);
        if (bits) {
            for (int gy = 0; gy < f->cell_h; gy++) {
                for (int gx = 0; gx < f->cell_w; gx++) {
                    int i = gy * f->cell_w + gx;
                    if (bits[i / 8] & (1u << (7 - (i % 8)))) {
                        fb_px(x + gx, y + gy, true);
                    }
                }
            }
        }
        x += f->cell_w;
    }
}

int display_utf8_len(const char *utf8)
{
    int n = 0;
    while (*utf8) {
        uint16_t cp = 0;
        if (utf8_next(&utf8, &cp) == 0) {
            break;
        }
        n++;
    }
    return n;
}

/* --------------------------------------------------------------- интерфейс */
esp_err_t display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << EPD_PWR_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level((gpio_num_t)EPD_PWR_GPIO, 0);   /* питание панели включено */
    vTaskDelay(pdMS_TO_TICKS(20));

    custom_lcd_spi_t cfg = {};
    cfg.cs = EPD_CS;
    cfg.dc = EPD_DC;
    cfg.rst = EPD_RST;
    cfg.busy = EPD_BUSY;
    cfg.mosi = EPD_MOSI;
    cfg.scl = EPD_SCLK;
    cfg.spi_host = SPI2_HOST;
    cfg.buffer_len = FB_LEN;

    s_drv = new epaper_driver_display(DISP_W, DISP_H, cfg);
    if (!s_drv) {
        ESP_LOGE(TAG, "не удалось создать драйвер панели");
        return ESP_ERR_NO_MEM;
    }
    s_fb = s_drv->EPD_Buffer();
    if (!s_fb) {
        ESP_LOGE(TAG, "нет кадрового буфера");
        return ESP_ERR_NO_MEM;
    }
    s_drv->EPD_Init();
    /* EPD_Clear() + полное обновление здесь убраны: именно они давали белое поле,
       которое висело до первой перерисовки. Панель начнём рисовать первым же
       кадром (ui_task), и это будет полное обновление с базовым образом. */
    memset(s_fb, 0xFF, FB_LEN);       /* 1 = белый — в буфере, панель не трогаем */
    s_ready = true;
    ESP_LOGI(TAG, "панель поднята (%dx%d)", DISP_W, DISP_H);
    return ESP_OK;
}

bool display_ready(void)
{
    return s_ready;
}

void display_clear(void)
{
    if (!s_fb) {
        return;
    }
    memset(s_fb, 0xFF, FB_LEN);       /* 1 = белый */
}

/* Полное обновление: кадр уходит и в «текущий» (0x24), и в «базовый» (0x26) образ
   панели — именно с ним сравнивает контроллер при частичном обновлении. Волновая
   форма при этом полная, отсюда инверсия и вспышки. */
static void refresh_full(void)
{
    s_drv->EPD_DisplayPartBaseImage();
    s_drv->EPD_Init_Partial();        /* дальше — быстрая частичная форма */
    s_have_base = true;
    s_since_full = 0;
}

void display_show(bool force_full)
{
    if (!s_ready || !s_drv) {
        return;
    }
    if (force_full || !s_have_base || s_since_full >= DISP_FULL_EVERY) {
        refresh_full();
        return;
    }
    s_drv->EPD_DisplayPart();         /* частичное: без инверсии и вспышек */
    s_since_full++;
}

void display_invert(void)
{
    if (!s_fb) {
        return;
    }
    for (int i = 0; i < FB_LEN; i++) {
        s_fb[i] = (uint8_t)~s_fb[i];
    }
}

void display_px(int x, int y, bool black)
{
    fb_px(x, y, black);
}

void display_hline(int x, int y, int len, bool black)
{
    for (int i = 0; i < len; i++) {
        fb_px(x + i, y, black);
    }
}

void display_rect(int x, int y, int w, int h, bool filled, bool black)
{
    if (filled) {
        for (int j = 0; j < h; j++) {
            display_hline(x, y + j, w, black);
        }
        return;
    }
    display_hline(x, y, w, black);
    display_hline(x, y + h - 1, w, black);
    for (int j = 0; j < h; j++) {
        fb_px(x, y + j, black);
        fb_px(x + w - 1, y + j, black);
    }
}

void display_text(int x, int y, const char *utf8)
{
    draw_text(&FONT_SMALL, x, y, utf8);
}

void display_text_big(int x, int y, const char *utf8)
{
    draw_text(&FONT_BIG, x, y, utf8);
}

int display_text_advance(void)
{
    return FONT_SMALL.cell_w;
}

int display_text_big_advance(void)
{
    return FONT_BIG.cell_w;
}
