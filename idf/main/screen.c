/*
 * Отрисовка экранов прибора (200x200) — ДВА экрана по макету v5 (docs/screens-v5.md):
 * сводный и SETUP. Раскладка считается, а не подбирается на глаз: блоки встают на
 * равные промежутки, между блоками тонкие линии, крайние поля 1 px.
 *
 * Правила, из которых всё выведено (замечания пользователя 17.09):
 *   * подписи одним кеглем (6x11) — CPU/RAM/DISK и GPU0/GPU1 не спорят между собой;
 *   * крупным остаётся только то, ради чего прибор стоит на столе: температуры карт
 *     (36x44) и проценты (18x30);
 *   * рамка сводного экрана — состояние ХОСТА по интернету: ONLINE-<мс> (интернет есть,
 *     задержка от агента), OFFLINE- (на пинг не ответили), NO DATA (агент молчит);
 *   * «н/д» показываем прочерком, а не нулём: ноль и «неизвестно» — разные вещи;
 *   * экранов два: сводный и SETUP, переключение коротким нажатием PWR.
 *
 * Шрифты и метрики чернил — display.h/fonts.h (генерирует tools/make_font.py).
 */
#include "screen.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "settings.h"      /* slot_kind_t — что показывать в двух крупных числах */

#define MARGIN     1     /* крайние поля */
#define FRAME_PAD  3     /* сколько пустого вокруг надписи в рамке (экран SETUP) */
#define STATUS_PAD 2     /* то же для строки состояния сводного экрана: рамки нет */
#define STATUS_TOP 5     /* отступ строки состояния от верхнего края (просьба 23.09):
                            без него слово липло к краю панели */
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

/* Заголовок экрана SETUP — верхний блок. Рамки вокруг него нет (просьба пользователя
   23.09; вокруг строки состояния сводного экрана её убрали тем же днём), поэтому блок
   остался той же высоты, а слово просто стоит по центру: раскладка SETUP не поехала. */
static void title_line(const char *text, int y, int h)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", text);
    upper_utf8(buf);
    display_text_center(DISP_F_BIG, y + (h - display_text_h(DISP_F_BIG)) / 2, buf);
}

/* Строка состояния хоста — верхний блок сводного экрана. Рамки вокруг неё нет
   (просьба пользователя 23.09: рамка вокруг пинга не нужна, а освободившееся место ушло
   в шрифт). Слово — кеглем ping, он в 1.5 раза крупнее прежнего big (чернила 21 против 14),
   задержка и счёт — кеглем txt9, в 1.5 раза крупнее small (9 против 6):
   «PING» 72 px + « - 15MS - 4/4» 117 px = 189 px из 200. Четырёхзначная задержка в 9 px
   уже не влезает (72 + 15 × 9 = 207), поэтому расшифровка переходит на мелкий шрифт:
   строка никогда не обрезается, меняется только её кегль. */
static int status_height(void)
{
    return display_text_h(DISP_F_PING) + 2 * STATUS_PAD;
}

static void status_line(const char *word, const char *detail, int y)
{
    char w[16], d[24];
    snprintf(w, sizeof(w), "%s", word);
    snprintf(d, sizeof(d), "%s", detail);
    upper_utf8(w);
    upper_utf8(d);

    const int h_word = display_text_h(DISP_F_PING);
    const int w_word = display_text_w(DISP_F_PING, w);
    int font = DISP_F_TXT9;
    int w_det = display_text_w(font, d);
    if (w_word + w_det > DISP_W) {
        font = DISP_F_SMALL;
        w_det = display_text_w(font, d);
    }
    int x = (DISP_W - (w_word + w_det)) / 2;
    if (x < 0) {
        x = 0;
    }
    display_text_f(DISP_F_PING, x, y + STATUS_PAD, w);
    if (d[0]) {
        display_text_f(font, x + w_word,
                       y + STATUS_PAD + (h_word - display_text_h(font)) / 2, d);
    }
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

/* Что выведено в крупном числе сводного экрана. Выбирается на /setup (slot1, slot2):
   на одном ПК есть температуры карт, на другом нет видеокарты вовсе, поэтому набор
   отдан пользователю. Подпись (CPU/GPU0/…) печатается над числом из того же выбора,
   иначе число на экране не опознать. */
typedef struct {
    const char *label;    /* пустая строка — слот выключен и не рисуется */
    char        num[12];  /* «47», «100» или «--»; с запасом, чтобы формат не усекался */
    const char *suffix;   /* «%» или «°» */
    bool        have;     /* данные есть (когда их нет, значка тоже нет) */
} slot_view_t;

#define TEMP_PAD     4   /* внутренние поля вокруг числа: без них числа слипаются */
#define SUFFIX_DEG_W 12  /* видимая ширина «°» средним шрифтом */
#define SUFFIX_PCT_W 16  /* «%» шире градуса */

/* Подпись слота и карта, к которой он относится: -1 — слот не про видеокарту.
   SLOT_GPU_LAST_PCT значит «вторая карта, а если она одна — первая»: так вторая цифра
   не пустует на машине с одной картой и показывает именно вторую на двухкарточной. */
static void slot_kind_resolve(const screen_state_t *st, uint8_t kind,
                              const char **label, int *gpu_ix, bool *want_temp)
{
    *label = "";
    *gpu_ix = -1;
    *want_temp = false;
    switch (kind) {
    case SLOT_CPU_PCT:      *label = "CPU";  break;
    case SLOT_RAM_PCT:      *label = "RAM";  break;
    case SLOT_DISK_PCT:     *label = "DISK"; break;
    case SLOT_GPU0_PCT:     *label = "GPU0"; *gpu_ix = 0; break;
    case SLOT_GPU1_PCT:     *label = "GPU1"; *gpu_ix = 1; break;
    case SLOT_GPU_LAST_PCT:
        *gpu_ix = (st->gpu_count > 1) ? 1 : 0;
        *label = (*gpu_ix == 1) ? "GPU1" : "GPU0";
        break;
    case SLOT_GPU0_TEMP:    *label = "GPU0"; *gpu_ix = 0; *want_temp = true; break;
    case SLOT_GPU1_TEMP:    *label = "GPU1"; *gpu_ix = 1; *want_temp = true; break;
    default:                break;               /* SLOT_OFF — пусто */
    }
}

static void slot_build(const screen_state_t *st, uint8_t kind, slot_view_t *v)
{
    const char *label = "";
    int gpu_ix = -1;
    bool want_temp = false;
    slot_kind_resolve(st, kind, &label, &gpu_ix, &want_temp);

    v->label = label;
    v->suffix = want_temp ? "°" : "%";
    v->have = false;

    if (!label[0]) {
        v->num[0] = 0;
        return;                                   /* слот выключен */
    }
    /* Округление делаем сами и зажимаем диапазон: у формата не должно оставаться
       варианта выдать больше знаков, чем ждёт раскладка (сборка IDF считает
       предупреждения об усечении ошибками). */
    if (gpu_ix >= 0) {
        if (gpu_ix < st->gpu_count) {             /* карты нет или агент молчит — прочерк */
            if (want_temp) {
                int t = st->gpu_temp_c[gpu_ix];
                if (t >= 0) {
                    if (t > 999) {
                        t = 999;
                    }
                    snprintf(v->num, sizeof(v->num), "%d", t);
                    v->have = true;
                }
            } else if (st->gpu_pct[gpu_ix] >= 0.0f) {
                int pc = (int)(st->gpu_pct[gpu_ix] + 0.5f);
                if (pc > 100) {
                    pc = 100;
                }
                snprintf(v->num, sizeof(v->num), "%d", pc);
                v->have = true;
            }
        }
    } else {
        float p = (kind == SLOT_RAM_PCT) ? st->mem_pct
                : (kind == SLOT_DISK_PCT) ? st->disk_pct : st->cpu_pct;
        if (p >= 0.0f) {
            int pc = (int)(p + 0.5f);
            if (pc > 100) {
                pc = 100;
            }
            snprintf(v->num, sizeof(v->num), "%d", pc);
            v->have = true;
        }
    }
    if (!v->have) {
        snprintf(v->num, sizeof(v->num), "--");
    }
}

/* Число в своём полуэкране. Крупным шрифтом — только два знака: «100%» это 4 клетки по
   36 px = 144 px, два таких числа 288 px при 200 доступных. Поэтому если в строке есть
   трёхзначное число, оба рисуются средним шрифтом и по вертикали встают в тот же блок
   (высота блока не меняется — раскладка не прыгает при 100 %). */
static void draw_slot(int x0, int x1, int y, int h_huge, int h_mid,
                      const slot_view_t *v, bool huge)
{
    if (!v->label[0]) {
        return;
    }
    if (huge) {
        int w_num = display_text_w(DISP_F_HUGE, v->num);
        int w_suf = v->have ? (v->suffix[0] == '%' ? SUFFIX_PCT_W : SUFFIX_DEG_W) : 0;
        int x = x0 + TEMP_PAD + ((x1 - x0) - 2 * TEMP_PAD - (w_num + w_suf)) / 2;
        display_text_f(DISP_F_HUGE, x, y, v->num);
        if (v->have) {
            display_text_f(DISP_F_MID, x + w_num + 2, y + 2, v->suffix);
        }
    } else {
        char all[16];
        snprintf(all, sizeof(all), "%s%s", v->num, v->have ? v->suffix : "");
        int w = display_text_w(DISP_F_MID, all);
        display_text_f(DISP_F_MID, x0 + ((x1 - x0) - w) / 2, y + (h_huge - h_mid) / 2, all);
    }
}


static void show_summary(const screen_state_t *st)
{
    const int h_stat  = status_height();
    const int h_small = display_text_h(DISP_F_SMALL);
    const int h_mid   = display_text_h(DISP_F_MID);
    const int h_big   = display_text_h(DISP_F_BIG);
    const int h_huge  = display_text_h(DISP_F_HUGE);

    /* строки: состояние, пустая строка-отступ, подписи крупных чисел, сами числа,
       разделитель, подписи процентов, проценты, аптайм, датчик */
    const int heights[] = { h_stat, h_small, h_small, h_huge, 1, h_small, h_mid, h_big, h_small };
    const int n = (int)(sizeof(heights) / sizeof(heights[0]));
    const int gap = gap_for(sum_heights(heights, n), n);

    int y = 0;

    /* 1. строка состояния — про интернет ХОСТА: это главное, что панель сообщает.
       Онлайн = у хоста есть интернет, офлайн = интернета нет; агент молчит — NO DATA
       (про интернет хоста прибор ничего не знает и делать вид, что знает, не должен). */
    char word[16], detail[24];
    detail[0] = '\0';
    if (st->agent_ok) {
        if (st->host_online) {
            /* Задержку присылает агент: четыре пинга выбранного на /setup узла, показываем
               среднее по ответившим. Больше 9999 мс не показываем — места в рамке нет. */
            unsigned ms = st->host_ping_ms > 9999u ? 9999u : (unsigned)st->host_ping_ms;
            snprintf(word, sizeof(word), "PING");
            if (st->host_ping_got == 0xFF) {
                snprintf(detail, sizeof(detail), " - %ums", ms);
            } else if (st->host_ping_got == 0) {
                /* пинг не ответил, но узел отозвался на 443 — так и пишем, а не «0/4» */
                snprintf(detail, sizeof(detail), " - %ums - TCP", ms);
            } else {
                snprintf(detail, sizeof(detail), " - %ums - %u/4", ms, (unsigned)st->host_ping_got);
            }
        } else {
            snprintf(word, sizeof(word), "OFFLINE");
            if (st->host_ping_got != 0xFF) {
                snprintf(detail, sizeof(detail), " - %u/4", (unsigned)st->host_ping_got);
            }
        }
    } else {
        snprintf(word, sizeof(word), "NO DATA");
    }
    status_line(word, detail, y + STATUS_TOP);   /* на 5 px ниже верхнего края */
    y += h_stat + gap;

    /* мелкие подписи CPU/GPU — на строку ниже состояния (просьба пользователя 23.09):
       слово стало крупнее, и вплотную к нему подписи читались как его хвост */
    y += h_small + gap;

    /* 2. подписи крупных чисел и 3. сами числа — самое крупное на экране.
       Что показывать, выбрано на /setup (slot1, slot2); по умолчанию CPU % и вторая
       карта, а на машине с одной картой — первая (SLOT_GPU_LAST_PCT). */
    slot_view_t sv[2];
    slot_build(st, st->slot1, &sv[0]);
    slot_build(st, st->slot2, &sv[1]);
    const bool sv_huge = strlen(sv[0].num) <= 2 && strlen(sv[1].num) <= 2;

    for (int i = 0; i < 2; i++) {
        if (sv[i].label[0]) {
            display_text_center_x(DISP_F_SMALL, i * DISP_W / 2, (i + 1) * DISP_W / 2,
                                  y, sv[i].label);
        }
    }
    y += h_small + gap;

    draw_slot(0, DISP_W / 2, y, h_huge, h_mid, &sv[0], sv_huge);
    draw_slot(DISP_W / 2, DISP_W, y, h_huge, h_mid, &sv[1], sv_huge);
    y += h_huge + gap;

    /* 4. разделитель: выше — крупные числа, ниже — загрузки */
    rule(y);
    y += 1 + gap;

    /* 5. подписи и 6. проценты CPU/RAM/DISK по трём колонкам. Три знака («100%») в
       среднем шрифте — 72 px при колонке 66,7 px, поэтому два соседних трёхзначных
       числа слиплись бы на 5 px. Если такое есть, вся строка идёт крупным шрифтом
       (48 px на число) и по вертикали встаёт на то же место. */
    {
        static const char *labels[3] = { "CPU", "RAM", "DISK" };
        const float vals[3] = { st->cpu_pct, st->mem_pct, st->disk_pct };
        char txt[3][8];
        bool wide = false;
        for (int i = 0; i < 3; i++) {
            pct_text(txt[i], sizeof(txt[i]), vals[i]);
            if (strlen(txt[i]) > 3) {
                wide = true;
            }
        }
        for (int i = 0; i < 3; i++) {
            int x0 = i * DISP_W / 3;
            int x1 = (i + 1) * DISP_W / 3;
            display_text_center_x(DISP_F_SMALL, x0, x1, y, labels[i]);
            if (wide) {
                display_text_center_x(DISP_F_BIG, x0, x1,
                                      y + h_small + gap + (h_mid - h_big) / 2, txt[i]);
            } else {
                display_text_center_x(DISP_F_MID, x0, x1, y + h_small + gap, txt[i]);
            }
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

    const int heights[] = { h_frame, h_big, h_big, h_big, h_big, h_big, 1, h_small, h_small, h_small };
    const int n = (int)(sizeof(heights) / sizeof(heights[0]));
    const int gap = gap_for(sum_heights(heights, n), n);

    int y = 0;

    title_line("SETUP", y, h_frame);
    y += h_frame + gap;

    char buf[28];
    setup_row("WEB", st->dev_ip ? st->dev_ip : "-", y, h_big, h_small);
    y += h_big + gap;

    /* Что стоит в двух крупных числах сводного экрана — подписи те же, что на нём
       сами (для SLOT_GPU_LAST_PCT это GPU1 на двухкарточной машине и GPU0 на одно-). */
    {
        char show[20];
        const char *l1 = "", *l2 = "";
        int ix = -1;
        bool t = false;
        slot_kind_resolve(st, st->slot1, &l1, &ix, &t);
        slot_kind_resolve(st, st->slot2, &l2, &ix, &t);
        (void)ix;
        (void)t;
        if (l1[0] && l2[0]) {
            snprintf(show, sizeof(show), "%s %s", l1, l2);
        } else if (l1[0] || l2[0]) {
            snprintf(show, sizeof(show), "%s", l1[0] ? l1 : l2);
        } else {
            snprintf(show, sizeof(show), "-");
        }
        setup_row("SHOW", show, y, h_big, h_small);
        y += h_big + gap;
    }

    snprintf(buf, sizeof(buf), "%u", (unsigned)st->rotation);
    setup_row("ROTATE", buf, y, h_big, h_small);
    y += h_big + gap;

    setup_row("DISK WRITE", st->disk_write_lock ? "OFF" : "ON", y, h_big, h_small);
    y += h_big + gap;

    setup_row("PING NODE", st->ping_target && st->ping_target[0] ? st->ping_target : "-",
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
