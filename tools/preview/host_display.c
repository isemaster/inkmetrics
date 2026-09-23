/*
 * Стенд для проверки раскладки экрана БЕЗ прибора.
 *
 * Зачем: макет, нарисованный в графическом редакторе, и то, что реально нарисует
 * прошивка, — разные вещи. Здесь компилируется НАСТОЯЩИЙ idf/main/screen.c (тот же
 * код, что уходит в прибор) и настоящие таблицы глифов из idf/main/fonts.h; вместо
 * панели — кадровый буфер в памяти, который сбрасывается в файл PGM.
 *
 * Сборка и запуск (в WSL, где есть gcc):
 *   bash tools/preview/build.sh
 *
 * Что проверяется: помещается ли кадр в 200x200, где проходят границы блоков, нет ли
 * наложений и обрезки. Раскладка при этом считается ровно тем кодом, что на приборе.
 */
#include "display.h"
#include "fonts.h"

#include <stdio.h>
#include <string.h>

#define FB_LEN (DISP_W * DISP_H / 8)

static uint8_t s_fb[FB_LEN];        /* 1 = белый, 0 = чёрный — как в прошивке */
static const char *s_out = "screen.pgm";

void host_set_output(const char *path)
{
    s_out = path;
}

static void fb_px(int x, int y, bool black)
{
    if (x < 0 || y < 0 || x >= DISP_W || y >= DISP_H) {
        return;
    }
    int idx = y * (DISP_W / 8) + (x >> 3);
    uint8_t bit = (uint8_t)(7 - (x & 7));
    if (black) {
        s_fb[idx] &= (uint8_t)~(1u << bit);
    } else {
        s_fb[idx] |= (uint8_t)(1u << bit);
    }
}

static bool fb_get(int x, int y)
{
    int idx = y * (DISP_W / 8) + (x >> 3);
    uint8_t bit = (uint8_t)(7 - (x & 7));
    return (s_fb[idx] & (1u << bit)) == 0;      /* бит снят = чёрный */
}

void display_clear(void)
{
    memset(s_fb, 0xFF, FB_LEN);
}

void display_px(int x, int y, bool black) { fb_px(x, y, black); }

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

/* ------------------------------------------------------------- шрифты (как в display.cpp) */
typedef struct {
    const uint16_t *cp;
    const uint8_t  *bits;
    int count, cell_w, cell_h, bytes, ink_top, ink_h;
} font_ref_t;

static const font_ref_t FONT_LIST[DISP_FONTS] = {
    { font_small_cp, (const uint8_t *)font_small_bits, FONT_SMALL_COUNT, FONT_SMALL_W,
      FONT_SMALL_H, FONT_SMALL_BYTES, FONT_SMALL_INK_TOP, FONT_SMALL_INK_H },
    { font_mid_cp,   (const uint8_t *)font_mid_bits,   FONT_MID_COUNT,   FONT_MID_W,
      FONT_MID_H,   FONT_MID_BYTES,   FONT_MID_INK_TOP,   FONT_MID_INK_H },
    { font_big_cp,   (const uint8_t *)font_big_bits,   FONT_BIG_COUNT,   FONT_BIG_W,
      FONT_BIG_H,   FONT_BIG_BYTES,   FONT_BIG_INK_TOP,   FONT_BIG_INK_H },
    { font_huge_cp,  (const uint8_t *)font_huge_bits,  FONT_HUGE_COUNT,  FONT_HUGE_W,
      FONT_HUGE_H,  FONT_HUGE_BYTES,  FONT_HUGE_INK_TOP,  FONT_HUGE_INK_H },
    { font_ping_cp,  (const uint8_t *)font_ping_bits,  FONT_PING_COUNT,  FONT_PING_W,
      FONT_PING_H,  FONT_PING_BYTES,  FONT_PING_INK_TOP,  FONT_PING_INK_H },
    { font_txt9_cp,  (const uint8_t *)font_txt9_bits,  FONT_TXT9_COUNT,  FONT_TXT9_W,
      FONT_TXT9_H,  FONT_TXT9_BYTES,  FONT_TXT9_INK_TOP,  FONT_TXT9_INK_H },
};

static const font_ref_t *font_ref(int font)
{
    if (font < 0 || font >= DISP_FONTS) {
        font = DISP_F_SMALL;
    }
    return &FONT_LIST[font];
}

static const uint8_t *font_lookup(const font_ref_t *f, uint16_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t cur = f->cp[mid];
        if (cur == cp) {
            return f->bits + (size_t)mid * (size_t)f->bytes;
        }
        if (cur < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

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

int display_utf8_len(const char *utf8)
{
    int n = 0;
    uint16_t cp = 0;
    while (*utf8) {
        if (utf8_next(&utf8, &cp) == 0) {
            break;
        }
        n++;
    }
    return n;
}

void display_text_f(int font, int x, int y, const char *utf8)
{
    const font_ref_t *f = font_ref(font);
    int y0 = y - f->ink_top;
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
                        fb_px(x + gx, y0 + gy, true);
                    }
                }
            }
        }
        x += f->cell_w;
    }
}

int display_text_w(int font, const char *utf8)
{
    return display_utf8_len(utf8) * font_ref(font)->cell_w;
}

int display_text_h(int font) { return font_ref(font)->ink_h; }
int display_text_adv(int font) { return font_ref(font)->cell_w; }

void display_text_center_x(int font, int x0, int x1, int y, const char *utf8)
{
    display_text_f(font, x0 + ((x1 - x0) - display_text_w(font, utf8)) / 2, y, utf8);
}

void display_text_center(int font, int y, const char *utf8)
{
    display_text_center_x(font, 0, DISP_W, y, utf8);
}

void display_text_right(int font, int y, const char *utf8)
{
    display_text_f(font, DISP_W - 1 - display_text_w(font, utf8), y, utf8);
}

void display_text(int x, int y, const char *utf8) { display_text_f(DISP_F_SMALL, x, y, utf8); }
void display_text_big(int x, int y, const char *utf8) { display_text_f(DISP_F_BIG, x, y, utf8); }
int display_text_advance(void) { return display_text_adv(DISP_F_SMALL); }
int display_text_big_advance(void) { return display_text_adv(DISP_F_BIG); }

void display_invert(void)
{
    for (int i = 0; i < FB_LEN; i++) {
        s_fb[i] = (uint8_t)~s_fb[i];
    }
}

void display_force_full(void) { }

void display_set_rotation(uint16_t deg) { (void)deg; }

/* Панели нет: кадр уходит в файл PGM (P2, 0 = чёрный, 255 = белый). */
void display_show(bool force_full)
{
    (void)force_full;
    FILE *f = fopen(s_out, "w");
    if (!f) {
        return;
    }
    fprintf(f, "P2\n%d %d\n255\n", DISP_W, DISP_H);
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            fprintf(f, "%d ", fb_get(x, y) ? 0 : 255);
        }
        fprintf(f, "\n");
    }
    fclose(f);
}
