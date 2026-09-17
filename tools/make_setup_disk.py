#!/usr/bin/env python
"""
Сборка содержимого диска, который прибор отдаёт хосту (флешка внутри прибора).

Зачем: на диске лежит настройка ПК — «воткнул прибор, запустил SETUP.CMD с появившегося
диска, и прибор в сети, с интернетом, а метрики уже идут на прибор» (замысел 16.09,
см. docs/plan-usb-installer.md).

Раскладка: MBR + раздел FAT16 на весь раздел `msc` (7552 сектора = 3,69 МБ), сборку делает
tools/make_disk_image.py. Так Windows заводит обычный съёмный диск с буквой; «дискета»
1,44 МБ (2880 секторов) и том без таблицы разделов приводили к драйверу гибких дисков —
буква A:, «Дискета» (разбор в docs/ingest-2026-09-17.md).

Имена файлов — короткие 8.3 (FAT без длинных имён): READRU/READMEEN, METRICS, MINSTALL.

Что кладём:

| Файл | Откуда | Зачем |
|---|---|---|
| SETUP.CMD | текст ниже, кодировка cp866 | точка входа: запускается с диска, поднимает права |
| MINSTALL.PS1 | tools/pc_setup/metrics_install.ps1 | ставит агента метрик в планировщик |
| METRICS.PS1 | tools/pc_setup/metrics_agent.ps1 | сам агент: метрики на POST /ingest |
| AGENT.PS1 | tools/pc_setup/agent_install.ps1 | настройка раздачи интернета (ICS) |
| ICS.PS1 | tools/ics_enable.ps1 | включение ICS на адаптере прибора |
| NETCHECK.PS1 | tools/net_check2.ps1 | диагностика сети, если что-то не встало |
| CHECK.CMD | tools/pc_setup/check.cmd | точка входа проверки: двойной клик, в конце пауза |
| CHECK.PS1 | tools/pc_setup/check.ps1 | сами проверки: прибор, диск, агент, задачи, метрики |
| FIXDISK.PS1 | tools/reset_disk_node.ps1 | ремонт: диск прибора не появился — снять устаревший узел и пересобрать шину |
| READRU.TXT, READMEEN.TXT | tools/pc_setup/README-*.txt | описание для человека |

Размер образа обязан совпадать с MSC_SECTORS в idf/main/msc.c и с разделом `msc`
в idf/partitions.csv — иначе прибор будет отдавать хосту не то, что лежит на диске.

    python tools/make_setup_disk.py
"""
from __future__ import annotations

import os
import pathlib
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import make_disk_image as mdi          # noqa: E402  (сборка FAT16 + проверка)

ROOT = os.path.dirname(HERE)
PC_SETUP = os.path.join(HERE, "pc_setup")
OUT = os.path.join(ROOT, "firmware", "media", "setup-disk-big.img")
SECTORS = 7552                         # = MSC_SECTORS в idf/main/msc.c (0x3B0000 / 512)

# SETUP.CMD печатает по-русски, а cmd.exe читает .cmd в системной кодировке (cp866),
# поэтому файл кодируется в cp866, а не в UTF-8.
SETUP_CMD = """@echo off
title inkmetrics - настройка компьютера
echo.
echo   inkmetrics: настраиваю этот компьютер для прибора
echo.
echo   1. Сейчас появится запрос прав администратора - нажмите "Да".
echo   2. Скрипт поставит агента метрик и включит раздачу интернета для прибора.
echo   3. Метрики смотрите на экране прибора: страница HOST SYS (кнопка PWR),
echo      либо на странице прибора, строка "Метрики хоста".
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath powershell -Verb RunAs -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0MINSTALL.PS1')"
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
        ("MINSTALL.PS1", read(os.path.join(PC_SETUP, "metrics_install.ps1"))),
        ("METRICS.PS1", read(os.path.join(PC_SETUP, "metrics_agent.ps1"))),
        ("AGENT.PS1", read(os.path.join(PC_SETUP, "agent_install.ps1"))),
        ("ICS.PS1", read(os.path.join(HERE, "ics_enable.ps1"))),
        ("NETCHECK.PS1", read(os.path.join(HERE, "net_check2.ps1"))),
        ("CHECK.PS1", read(os.path.join(PC_SETUP, "check.ps1"))),
        ("CHECK.CMD", read(os.path.join(PC_SETUP, "check.cmd"))),
        ("FIXDISK.PS1", read(os.path.join(HERE, "reset_disk_node.ps1"))),
        ("READRU.TXT", read(os.path.join(PC_SETUP, "README-RU.txt"), bom=True)),
        ("READMEEN.TXT", read(os.path.join(PC_SETUP, "README-EN.txt"), bom=True)),
    ]

    img = mdi.build(SECTORS, files)
    print(f"диск: {len(img)} байт ({len(img) // 512} секторов = {len(img) / 1048576:.2f} МБ)")

    # проверка: побайтово сверяем то, что реально попало в образ, с исходниками
    # (размера из записи каталога мало — файл может лежать обрезанным)
    listed = dict(mdi.read_root_entries(img))
    ok = True
    for name, content in files:
        size = listed.get(name)
        back = mdi.read_file(img, name)
        if back is None:
            state = "ОШИБКА (файла в образе нет)"
        elif back != content or size != len(content):
            state = f"ОШИБКА (в образе {size} байт, читается {len(back)})"
        else:
            state = "OK (байт в байт)"
        if not state.startswith("OK"):
            ok = False
        print(f"  {name:>11}: {len(content):>6} байт — {state}")
    for name in listed:
        if name not in dict(files):
            print(f"  лишний файл в образе: {name}")
            ok = False
    if not ok:
        raise SystemExit("образ собран неверно")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as fh:
        fh.write(img)
    print("записан", os.path.abspath(OUT))

    # Тот же SETUP.CMD — отдельным файлом рядом с образом. Его копирует сборщик комплекта
    # (tools/make_flash_kit.py) в pc-setup/: так не нужна читалка FAT16, а файл на диске и в
    # комплекте обязан совпадать байт в байт.
    setup_path = os.path.join(os.path.dirname(OUT), "SETUP.CMD")
    with open(setup_path, "wb") as fh:
        fh.write(SETUP_CMD.encode("cp866"))
    print("SETUP.CMD выписан отдельно:", os.path.abspath(setup_path))

    print("залить в раздел msc: python tools/flash_msc_image.py --port COMx "
          "(адрес берётся из idf/partitions.csv)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
