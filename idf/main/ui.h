/* Экран прибора: инициализация панели, датчика и кнопок + задача отрисовки. */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

void ui_init(void);
/* Данные из сетевого слоя (main.c) — обновлять по мере поступления. */
void ui_set_net_info(bool link_up, uint32_t rx_frames, uint32_t tx_frames);

#endif /* UI_H */
