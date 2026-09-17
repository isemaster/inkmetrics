/*
 * Точки входа стенда: собираем состояние прибора, как это делает ui.c, и просим
 * screen.c нарисовать оба экрана. Никакой своей раскладки — рисует тот самый код,
 * который уходит в прошивку.
 *
 * Сценарии (файлы рядом):
 *   summary.pgm  — рабочий вид: две карты 78° и 79°, CPU 37%, RAM 61%, диск 82%,
 *                  аптайм 3 дня 4:12, датчик 24.3 C / 41 %;
 *   setup.pgm    — экран SETUP: адрес веб-кабинета, поворот 90, запись включена;
 *   stale.pgm    — агент молчит: в рамке «НЕТ СВЯЗИ», вместо чисел прочерки.
 */
#include <string.h>

#include "screen.h"

void host_set_output(const char *path);

static void base(screen_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->dev_ip = "192.168.7.1";
    st->up_s = 97440;                 /* 1 д 03:04 */
    st->fw = "0.4.2";
    st->ping_target = "YA.RU";
    st->rotation = 90;
    st->disk_write_lock = false;
    st->sensor_ok = true;
    st->t_c = 24.3f;
    st->rh = 41.0f;
}

int main(void)
{
    /* 1. сводный экран в работе */
    screen_state_t st;
    base(&st);
    st.agent_have = true;
    st.agent_ok = true;
    st.agent_age_s = 12;
    st.cpu_pct = 37.0f;
    st.mem_pct = 61.0f;
    st.disk_pct = 82.0f;
    st.gpu_count = 2;
    st.gpu_temp_c[0] = 78;
    st.gpu_temp_c[1] = 79;
    st.gpu_pct[0] = 91.0f;
    st.gpu_pct[1] = 4.0f;
    st.hup_ok = true;
    st.hup_h = 76.2f;                 /* 3 д 04:12 */
    host_set_output("summary.pgm");
    screen_show(&st, 0);

    /* 2. SETUP */
    host_set_output("setup.pgm");
    screen_show(&st, 1);

    /* 3. агент молчит */
    screen_state_t off;
    base(&off);
    off.agent_have = true;
    off.agent_ok = false;
    off.agent_age_s = 340;
    off.cpu_pct = off.mem_pct = off.disk_pct = -1.0f;
    off.gpu_count = 2;
    off.gpu_temp_c[0] = off.gpu_temp_c[1] = -1;
    off.gpu_pct[0] = off.gpu_pct[1] = -1.0f;
    off.hup_ok = false;
    host_set_output("stale.pgm");
    screen_show(&off, 0);
    return 0;
}
