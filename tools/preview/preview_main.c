/*
 * Точки входа стенда: собираем состояние прибора, как это делает ui.c, и просим
 * screen.c нарисовать оба экрана. Никакой своей раскладки — рисует тот самый код,
 * который уходит в прошивку.
 *
 * Сценарии (файлы рядом):
 *   summary.pgm  — умолчание прошивки: крупно CPU % (37 %) и загрузка второй карты
 *                  (4 %), ONLINE, две карты;
 *   temps.pgm    — в крупных числах температуры карт (78° 79°) — вид до настройки;
 *   wide.pgm     — трёхзначные числа: CPU/RAM/диск по 100 % (проверка правила «три
 *                  знака — средний кегль» и прежней коллизии в строке процентов);
 *   setup.pgm    — экран SETUP: адрес веб-кабинета, строка SHOW с выбранной парой,
 *                  поворот 90, запись включена;
 *   offline.pgm  — у хоста пропал интернет: агент на связи, метрики свежие, OFFLINE;
 *   nodata.pgm   — агент молчит: NO DATA и прочерки вместо чисел.
 */
#include <string.h>

#include "screen.h"
#include "settings.h"          /* slot_kind_t: что стоит в крупных числах */

void host_set_output(const char *path);

static void base(screen_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->dev_ip = "192.168.7.1";
    st->up_s = 97440;                 /* 1 д 03:04 */
    st->fw = "0.6.1";
    st->ping_target = "YA.RU";
    st->rotation = 90;
    st->disk_write_lock = false;
    st->sensor_ok = true;
    st->t_c = 24.3f;
    st->rh = 41.0f;
    /* умолчания прошивки: слева загрузка CPU, справа вторая карта (иначе первая) */
    st->slot1 = SLOT_CPU_PCT;
    st->slot2 = SLOT_GPU_LAST_PCT;
}

/* Живые метрики хоста: две карты, проценты, аптайм — то же во всех «рабочих» кадрах. */
static void live_metrics(screen_state_t *st)
{
    st->agent_have = true;
    st->agent_ok = true;
    st->agent_age_s = 12;
    st->cpu_pct = 37.0f;
    st->mem_pct = 61.0f;
    st->disk_pct = 82.0f;
    st->gpu_count = 2;
    st->gpu_temp_c[0] = 78;
    st->gpu_temp_c[1] = 79;
    st->gpu_pct[0] = 91.0f;
    st->gpu_pct[1] = 4.0f;
    st->hup_ok = true;
    st->hup_h = 76.2f;                /* 3 д 04:12 */
}

int main(void)
{
    /* 1. умолчание: CPU % и загрузка второй карты, интернет у хоста есть — ONLINE */
    screen_state_t st;
    base(&st);
    live_metrics(&st);
    st.host_net_known = true;
    st.host_online = true;
    st.host_ping_ms = 15;
    st.host_ping_got = 4;           /* ответили все четыре пинга */
    st.host_ping_target = "8.8.8.8";
    host_set_output("summary.pgm");
    screen_show(&st, 0);

    /* 2. в крупных числах температуры карт — прежний вид, проверяем что не сломали */
    screen_state_t tp = st;
    tp.slot1 = SLOT_GPU0_TEMP;
    tp.slot2 = SLOT_GPU1_TEMP;
    host_set_output("temps.pgm");
    screen_show(&tp, 0);

    /* 3. трёхзначные числа: полная загрузка CPU/RAM/диска — строка процентов идёт
       крупным шрифтом, крупные числа — средним (в крупный шрифт 100 % не влезает) */
    screen_state_t wd = st;
    wd.cpu_pct = 100.0f;
    wd.mem_pct = 100.0f;
    wd.disk_pct = 100.0f;
    wd.slot1 = SLOT_CPU_PCT;
    wd.slot2 = SLOT_RAM_PCT;
    host_set_output("wide.pgm");
    screen_show(&wd, 0);

    /* 4. SETUP: там же видно строку SHOW с выбранной парой */
    host_set_output("setup.pgm");
    screen_show(&st, 1);

    /* 5. интернета у хоста нет — OFFLINE, метрики при этом живые */
    screen_state_t off = st;
    off.host_online = false;
    off.host_ping_ms = 0;
    off.host_ping_got = 0;
    host_set_output("offline.pgm");
    screen_show(&off, 0);

    /* 6. агент молчит — NO DATA, числа стёрты (значения пришли, но им нельзя верить) */
    screen_state_t none;
    base(&none);
    none.agent_have = true;
    none.agent_ok = false;
    none.agent_age_s = 340;
    none.host_net_known = true;
    none.host_online = false;
    none.host_ping_got = 0xFF;      /* агент молчит — счёт неизвестен */
    none.cpu_pct = none.mem_pct = none.disk_pct = -1.0f;
    none.gpu_count = 0;
    none.gpu_temp_c[0] = none.gpu_temp_c[1] = -1;
    none.gpu_pct[0] = none.gpu_pct[1] = -1.0f;
    none.hup_ok = false;
    host_set_output("nodata.pgm");
    screen_show(&none, 0);
    return 0;
}
