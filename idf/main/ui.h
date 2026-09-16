/* Экран прибора: инициализация панели, датчика и кнопок + задача отрисовки. */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

void ui_init(void);
/* Данные из сетевого слоя (main.c) — обновлять в главном цикле. */
void ui_set_net_info(bool link_up, uint32_t rx_frames, uint32_t tx_frames,
                     uint32_t rx_bytes, uint32_t tx_bytes, uint32_t reconnects);

#endif /* UI_H */
