/*
 * Отрисовка экранов прибора (200x200) — ДВА экрана по макету v5 (docs/screens-v5.md):
 * сводный и SETUP. Раскладка считается, а не подбирается на глаз: блоки встают на
 * равные промежутки, между блоками тонкие линии, крайние поля 1 px.
 *
 * Правила, из которых всё выведено (замечания пользователя 17.09):
 *   * подписи одним кеглем (6x11) — CPU/RAM/DISK и GPU0/GPU1 не спорят между собой;
 *   * крупным остаётся только то, ради чего прибор стоит на столе: температуры карт
 *     (40x48) и проценты (18x30);
 *   * «н/д» показываем прочерком, а не нулём: ноль и «неизвестно» — разные вещи;
 *   * экранов два: сводный и SETUP, переключение коротким нажатием PWR.
 *
 * Шрифты и метрики чернил — display.h/fonts.h (генерирует tools/make_font.py).
 */
#include "screen.h"

#include <stdio.h>
#include <string.h>

#include "display.h"

#define MARGIN     1     /* крайние поля */
#define FRAME_PAD  3     /* сколько пустого вокруг надписи в рамке */
#define GAP_MIN    3     /* меньше этого промежутки не делаем: блоки слипнутся */

/* ------------------------------------------------------------------ утилиты */

/* Привести строку к верхнему регистру: строчных букв в крупных шрифтах нет. */
static void upper_utf8(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        if (p[0] < 0x80) {
            if (p[0] >= 'a' && p[0] <= 'z') {
                p[0] = (unsigned char)(p[0] - 0x20);
            }
            p++;
        } else if ((p[0] & 0xE0) == 0xC0) {
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
            uint16_t cp = (uint16_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
            if (cp >= 0x430 && cp <= 0x44F) {
                cp = (uint16_t)(cp - 0x20);           /* а-я → А-Я */
            } else if (cp == 0x451) {
                cp = 0x401;                           /* ё → Ё */
            }
            p[0] = (uint8_t)(0xE0 | (cp >> 12));
            p[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            p[2] = (uint8_t)(0x80 | (cp & 0x3F));
            p += 3;
        } else {
            p++;
        }
    }
}

/* Промежуток между блоками: свободное место делим на равные части. Два пикселя
   оставляем снизу: нижняя строка (FW/UP на SETUP) при полном отсутствии поля
   упиралась в самый край панели. */
static int gap_for(int sum_h, int n_blocks)
{
    if (n_blocks < 2) {
        return 0;
    }
    int gap = (DISP_H - sum_h - 2) / (n_blocks - 1);
    return gap < GAP_MIN ? GAP_MIN : gap;
}

static int sum_heights(const int *h, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += h[i];
    }
    return s;
}

/* Надпись в рамке во всю ширину — верхний блок обоих экранов. */
static void frame_title(const char *text, int y, int h)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", text);
    upper_utf8(buf);
    display_rect(0, y, DISP_W, h, false, true);
    display_text_center(DISP_F_BIG, y + FRAME_PAD, buf);
}

static int frame_height(void)
{
    return display_text_h(DISP_F_BIG) + 2 * FRAME_PAD;
}

/* Тонкая линия-разделитель блока (с полями 10 px по краям). */
static void rule(int y)
{
    display_hline(10, y, DISP_W - 20, true);
}

/* Проценты: «--», если данных нет (ноль и «неизвестно» — разные вещи). */
static void pct_text(char *out, size_t len, float pct)
{
    if (pct >= 0) {
        snprintf(out, len, "%.0f%%", (double)pct);
    } else {
        snprintf(out, len, "--");
    }
}

static void uptime_text(char *out, size_t len, uint32_t up_s)
{
    snprintf(out, len, "%uD %02u:%02u", (unsigned)(up_s / 86400u),
             (unsigned)((up_s % 86400u) / 3600u), (unsigned)((up_s % 3600u) / 60u));
}

/* ------------------------------------------------------- экран 0: сводный */

/* Температура карты: цифры самым крупным шрифтом, «°» — средним, в верхнем углу.
   В крупном шрифте клетка 36 px на знак, а градус съедал бы целую клетку: две
   температуры перестали бы помещаться в 200 px. С градусом из среднего шрифта блок
   «78°» занимает 84 px, два таких — 168 px, между числами остаётся 24 px. Внутренние
   поля по 4 px: без них числа упирались друг в друга и читались как одно. */
#define TEMP_PAD 4

static void draw_temp(int x0, int x1, int y, int temp_c)
{
    char num[12];
    bool have = temp_c >= 0;
    if (have) {
        snprintf(num, sizeof(num), "%d", temp_c);
    } else {
        snprintf(num, sizeof(num), "--");
    }

    int w_num = display_text_w(DISP_F_HUGE, num);
    int w_deg = have ? 12 : 0;                    /* видимая ширина градуса */
    int x = x0 + TEMP_PAD + ((x1 - x0) - 2 * TEMP_PAD - (w_num + w_deg)) / 2;

    display_text_f(DISP_F_HUGE, x, y, num);
    if (have) {
        display_text_f(DISP_F_MID, x + w_num + 2, y + 2, "°");
    }
}

static void show_summary(const screen_state_t *st)
{
    const int h_frame = frame_height();
    const int h_small = display_text_h(DISP_F_SMALL);
    const int h_mid   = display_text_h(DISP_F_MID);
    const int h_big   = display_text_h(DISP_F_BIG);
    const int h_huge  = display_text_h(DISP_F_HUGE);

    const int heights[] = { h_frame, h_small, h_huge, 1, h_small, h_mid, h_big, h_small };
    const int n = (int)(sizeof(heights) / sizeof(heights[0]));
    const int gap = gap_for(sum_heights(heights, n), n);

    int y = 0;

    /* 1. рамка: живы ли метрики хоста (интерфейс прибора — только латиница) */
    frame_title(st->agent_ok ? "ONLINE" : "OFFLINE", y, h_frame);
    y += h_frame + gap;

    /* 2. подписи карт и 3. их температуры — самое крупное на экране */
    display_text_center_x(DISP_F_SMALL, 0, DISP_W / 2, y, "GPU0");
    display_text_center_x(DISP_F_SMALL, DISP_W / 2, DISP_W, y, "GPU1");
    y += h_small + gap;

    draw_temp(0, DISP_W / 2, y, st->gpu_count > 0 ? st->gpu_temp_c[0] : -1);
    draw_temp(DISP_W / 2, DISP_W, y, st->gpu_count > 1 ? st->gpu_temp_c[1] : -1);
    y += h_huge + gap;

    /* 4. разделитель: выше — температуры, ниже — загрузки */
    rule(y);
    y += 1 + gap;

    /* 5. подписи и 6. проценты CPU/RAM/DISK по трём колонкам */
    {
        static const char *labels[3] = { "CPU", "RAM", "DISK" };
        const float vals[3] = { st->cpu_pct, st->mem_pct, st->disk_pct };
        for (int i = 0; i < 3; i++) {
            int x0 = i * DISP_W / 3;
            int x1 = (i + 1) * DISP_W / 3;
            display_text_center_x(DISP_F_SMALL, x0, x1, y, labels[i]);
            char val[8];
            pct_text(val, sizeof(val), vals[i]);
            display_text_center_x(DISP_F_MID, x0, x1, y + h_small + gap, val);
        }
    }
    y += h_small + gap + h_mid + gap;

    /* 7. аптайм хоста */
    {
        char buf[24];
        if (st->hup_ok && st->hup_h >= 0) {
            char up[16];
            uint32_t total_min = (uint32_t)(st->hup_h * 60.0f);
            uptime_text(up, sizeof(up), total_min * 60u);
            snprintf(buf, sizeof(buf), "UPTIME %s", up);
        } else {
            snprintf(buf, sizeof(buf), "UPTIME --");
        }
        display_text_center(DISP_F_BIG, y, buf);
    }
    y += h_big + gap;

    /* 8. датчик на плате и подсказка про кнопку */
    {
        char buf[32];
        if (st->sensor_ok) {
            snprintf(buf, sizeof(buf), "TEMP %.1fC  RH %.0f%%", (double)st->t_c, (double)st->rh);
        } else {
            snprintf(buf, sizeof(buf), "TEMP N/A");
        }
        display_text_f(DISP_F_SMALL, MARGIN, y, buf);
        display_text_right(DISP_F_SMALL, y, "PWR>");
    }
}

/* --------------------------------------------------------- экран 1: SETUP */

/* Строка настроек: подпись мелким слева, значение крупным справа. */
static void setup_row(const char *label, const char *value, int y, int h_big, int h_small)
{
    char lab[20], val[28];
    snprintf(lab, sizeof(lab), "%s", label);
    snprintf(val, sizeof(val), "%s", value);
    upper_utf8(lab);
    upper_utf8(val);
    display_text_f(DISP_F_SMALL, MARGIN, y + (h_big - h_small) / 2, lab);
    display_text_right(DISP_F_BIG, y, val);
}

static void show_setup(const screen_state_t *st)
{
    const int h_frame = frame_height();
    const int h_small = display_text_h(DISP_F_SMALL);
    const int h_big   = display_text_h(DISP_F_BIG);

    const int heights[] = { h_frame, h_big, h_big, h_big, h_big, 1, h_small, h_small, h_small };
    const int n = (int)(sizeof(heights) / sizeof(heights[0]));
    const int gap = gap_for(sum_heights(heights, n), n);

    int y = 0;

    frame_title("SETUP", y, h_frame);
    y += h_frame + gap;

    char buf[28];
    setup_row("WEB", st->dev_ip ? st->dev_ip : "-", y, h_big, h_small);
    y += h_big + gap;

    snprintf(buf, sizeof(buf), "%u", (unsigned)st->rotation);
    setup_row("ROTATE", buf, y, h_big, h_small);
    y += h_big + gap;

    setup_row("DISK WRITE", st->disk_write_lock ? "OFF" : "ON", y, h_big, h_small);
    y += h_big + gap;

    setup_row("PING", st->ping_target && st->ping_target[0] ? st->ping_target : "-",
              y, h_big, h_small);
    y += h_big + gap;

    rule(y);
    y += 1 + gap;

    /* Настройка значений — в браузере: кнопок две, ими адрес или порог не набрать.
       Экран отвечает на вопрос «куда идти и что сейчас включено», а подсказки
       описывают ровно то, что кнопки делают (см. ui.c, btn_cb). */
    display_text_center(DISP_F_SMALL, y, "PWR: BACK  HOLD: REDRAW");
    y += h_small + gap;
    display_text_center(DISP_F_SMALL, y, "BOOT: ROTATE  HOLD: LOCK");
    y += h_small + gap;

    {
        char left[32], right[8];
        char up[16];
        uptime_text(up, sizeof(up), st->up_s);
        snprintf(left, sizeof(left), "FW %s  UP %s", st->fw ? st->fw : "-", up);
        snprintf(right, sizeof(right), "%d/%d", 2, (int)SCREEN_PAGES);
        display_text_f(DISP_F_SMALL, MARGIN, y, left);
        display_text_right(DISP_F_SMALL, y, right);
    }
}

void screen_show(const screen_state_t *st, int page)
{
    display_clear();
    if (page == 1) {
        show_setup(st);
    } else {
        show_summary(st);
    }
    if (st->emergency) {
        display_invert();          /* авария (нет своего адреса): инверсный вид */
    }
    display_show(st->emergency);
}
