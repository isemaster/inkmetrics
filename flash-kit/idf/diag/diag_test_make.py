#!/usr/bin/env python
"""
Проверка читалки «чёрного ящика» без платы: собирает синтетический дамп области diag.

Дамп = 32 КБ: первые 16 КБ — лог текущей загрузки (как пишет idf/main/diag.c),
следующий сектор (0x4000...) — журнал загрузок (не стирается, копится).

Прогонять при ЛЮБОМ изменении формата лога или журнала — иначе читалка и прошивка
разъедутся незаметно (так уже было: хвост записи добивался 0xFF, и лог три сессии
подряд выглядел пустым).
"""
import pathlib
import struct

text = (
    "===== inkmetrics 0.3.0-idf: загрузка #5, сброс 3\r\n"
    "STEP app_main: ящик → ESP_OK (heap 364596)\r\n"
    "STEP boot_escape_check пройден (BOOT не удержан)\r\n"
    "STEP проба PHY: вызываю usb_new_phy\r\n"
    "STEP usb_new_phy → ESP_OK\r\n"
    "STEP USB[после PHY] GSNPSID=0x4f544000 WRAP.otg_conf=0x00000804 USJ.conf0=0x00000000\r\n"
    "E (421) tinyusb_task: Init TinyUSB stack failed\r\n"
).encode("utf-8")

hdr = struct.pack("<4I", 0x31474144, 5, 3, 4210).ljust(64, b"\xff")   # заголовок занимает до 0x40
log_region = 16 * 1024
log = hdr + text + b"\xff" * (log_region - 64 - len(text))

# журнал загрузок: три записи с разными причинами (software, power-on, паника)
journal = b"\xff" * 4096
entries = [(3, 3, 1200), (4, 1, 980), (5, 4, 640)]      # (загрузка, причина, мс до ящика)
for i, (boots, reason, uptime) in enumerate(entries):
    rec = struct.pack("<4I", 0x4D524A44, boots, reason, uptime)
    journal = journal[:i * 16] + rec + journal[(i + 1) * 16:]

out = pathlib.Path(__file__).with_name("diag-test.bin")
out.write_bytes(log + journal)
print("тестовый дамп:", out, f"({out.stat().st_size} Б: лог {log_region // 1024} КБ + журнал 4 КБ)")
