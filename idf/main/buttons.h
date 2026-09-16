/*
 * Две кнопки платы (BOOT — GPIO0, PWR — GPIO18, обе активны низким).
 * Логика подавления дребезга взята из Arduino-версии: подтверждение уровня 30 мс,
 * одно событие на нажатие, удержание — отдельное событие HOLD.
 */
#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdbool.h>

typedef enum {
    BTN_BOOT = 0,
    BTN_PWR  = 1,
} btn_id_t;

typedef enum {
    BTN_EV_SHORT = 0,   /* отпущено раньше 800 мс */
    BTN_EV_LONG,        /* отпущено между 800 мс и 3 с */
    BTN_EV_HOLD,        /* удержано 3 с (событие приходит один раз) */
} btn_event_t;

typedef void (*btn_cb_t)(btn_id_t btn, btn_event_t ev);

void buttons_init(btn_cb_t cb);

#endif /* BUTTONS_H */
