#!/usr/bin/env python
"""Проверка читалки «чёрного ящика» без платы: собирает синтетический дамп области diag."""
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
region = 16 * 1024
out = pathlib.Path(__file__).with_name("diag-test.bin")
out.write_bytes(hdr + text + b"\xff" * (region - 64 - len(text)))
print("тестовый дамп:", out)
