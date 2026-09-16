/*
 * Диск хоста (USB MSC): прибор отдаёт раздел `msc` как флешку с файлами настройки ПК.
 * См. docs/plan-usb-installer.md.
 */
#ifndef MSC_H
#define MSC_H

#include <stdbool.h>
#include <stdint.h>

void msc_init(void);
/* Сведения о диске для веб-страницы: сколько секторов, заблокирована ли запись,
   сколько было чтений/записей от хоста. Любой указатель можно передать NULL. */
void msc_info(uint32_t *sectors, bool *write_locked, uint32_t *reads, uint32_t *writes);

#endif /* MSC_H */
