#!/usr/bin/env python
"""
Сборка содержимого диска, который прибор отдаёт хосту (флешка внутри прибора).

Зачем: на диске должен лежать скрипт настройки ПК — «воткнул прибор, запустил
SETUP.CMD с появившегося диска, прибор в сети и с интернетом» (замысел пользователя
16.09, см. docs/plan-usb-installer.md).

Как: берём проверенную раскладку FAT12 из `tools/make_msc_image.py` (дискета 1,44 МБ,
2880 секторов, superfloppy — Windows монтирует только такой размер без таблицы
разделов) и кладём в неё файлы настройки. Имена — короткие 8.3 (FAT12 без длинных
имён), поэтому READMERU/READMEEN, а не README-RU.

Что получается: `firmware/media/setup-disk.img` (1,44 МБ), его заливает в раздел `msc`
инструмент `tools/try_flash.py`.

    python tools/make_setup_disk.py
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import make_msc_image as msc   # noqa: E402  (раскладка FAT12 — берём оттуда)

ROOT = os.path.dirname(HERE)
PC_SETUP = os.path.join(HERE, "pc_setup")
OUT = os.path.join(ROOT, "firmware", "media", "setup-disk.img")

# SETUP.CMD печатает по-русски, а cmd.exe читает .bat/.cmd в системной кодировке (cp866).
SETUP_CMD = """@echo off
title inkmetrics - настройка компьютера
echo.
echo   inkmetrics: настраиваю этот компьютер для прибора
echo.
echo   1. Сейчас появится запрос прав администратора - нажмите "Да".
echo   2. Скрипт поставит агента и включит раздачу интернета для прибора.
echo   3. Адрес страницы смотрите на экране прибора (строка ADDR).
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath powershell -Verb RunAs -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0AGENT.PS1')"
echo.
echo   Готово. Нажмите любую клавишу, чтобы закрыть окно.
pause >nul
"""


def read(path: str, bom: bool = False) -> bytes:
    with open(path, "rb") as fh:
        data = fh.read()
    if bom and not data.startswith(b"\xef\xbb\xbf"):
        data = b"\xef\xbb\xbf" + data      # Блокнот надёжнее читает UTF-8 с BOM
    return data


def main() -> int:
    files: list[tuple[str, bytes]] = [
        ("SETUP.CMD", SETUP_CMD.encode("cp866")),
        ("AGENT.PS1", read(os.path.join(PC_SETUP, "agent_install.ps1"))),
        ("ICS.PS1", read(os.path.join(HERE, "ics_enable.ps1"))),
        ("NETCHECK.PS1", read(os.path.join(HERE, "net_check2.ps1"))),
        ("READRU.TXT", read(os.path.join(PC_SETUP, "README-RU.txt"), bom=True)),
        ("READMEEN.TXT", read(os.path.join(PC_SETUP, "README-EN.txt"), bom=True)),
    ]

    img = msc.build_image(files)
    print(f"диск: {len(img)} байт ({len(img) // 512} секторов = дискета 1,44 МБ)")

    listed = dict(msc.verify_image(img))
    ok = True
    for name, content in files:
        size = listed.get(name)
        state = "OK" if size == len(content) else f"ОШИБКА (в образе {size}, ожидалось {len(content)})"
        if size != len(content):
            ok = False
        print(f"  {name:>13}: {len(content):>6} байт — {state}")
    for name in listed:
        if name not in dict(files):
            print(f"  лишний файл в образе: {name}")
            ok = False
    if not ok:
        raise SystemExit("образ собран неверно")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as fh:
        fh.write(img)
    print("записан", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
