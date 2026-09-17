/*
 * Точки входа стенда: собираем состояние прибора, как это делает ui.c, и просим
 * screen.c нарисовать оба экрана. Никакой своей раскладки — рисует тот самый код,
 * который уходит в прошивку.
 *
 * Сценарии (файлы рядом):
 *   summary.pgm  — рабочий вид: ONLINE, две карты 78° и 79°, CPU 37%, RAM 61%, диск 82%,
 *                  аптайм 3 дня 4:12, датчик 24.3 C / 41 %;
 *   setup.pgm    — экран SETUP: адрес веб-кабинета, поворот 90, запись включена;
 *   offline.pgm  — у хоста пропал интернет: агент на связи, метрики свежие, рамка OFFLINE;
 *   nodata.pgm   — агент молчит: NO DATA и прочерки вместо чисел.
 */
#include <string.h>

#include "screen.h"

void host_set_output(const char *path);

static void base(screen_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->dev_ip = "192.168.7.1";
    st->up_s = 97440;                 /* 1 д 03:04 */
    st->fw = "0.5.0";
    st->ping_target = "YA.RU";
    st->rotation = 90;
    st->disk_write_lock = false;
    st->sensor_ok = true;
    st->t_c = 24.3f;
    st->rh = 41.0f;
}

/* Живые метрики хоста: две карты, проценты, аптайм — то же в обоих «рабочих» кадрах. */
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
    /* 1. интернет у хоста есть — ONLINE */
    screen_state_t st;
    base(&st);
    live_metrics(&st);
    st.host_net_known = true;
    st.host_online = true;
    st.host_ping_ms = 15;
    st.host_ping_target = "8.8.8.8";
    host_set_output("summary.pgm");
    screen_show(&st, 0);

    /* 2. SETUP */
    host_set_output("setup.pgm");
    screen_show(&st, 1);

    /* 3. интернета у хоста нет — OFFLINE, метрики при этом живые */
    screen_state_t off = st;
    off.host_online = false;
    off.host_ping_ms = 0;
    host_set_output("offline.pgm");
    screen_show(&off, 0);

    /* 4. агент молчит — NO DATA, числа стёрты (значения пришли, но им нельзя верить) */
    screen_state_t none;
    base(&none);
    none.agent_have = true;
    none.agent_ok = false;
    none.agent_age_s = 340;
    none.host_net_known = true;
    none.host_online = false;
    none.cpu_pct = none.mem_pct = none.disk_pct = -1.0f;
    none.gpu_count = 0;
    none.gpu_temp_c[0] = none.gpu_temp_c[1] = -1;
    none.gpu_pct[0] = none.gpu_pct[1] = -1.0f;
    none.hup_ok = false;
    host_set_output("nodata.pgm");
    screen_show(&none, 0);
    return 0;
}
