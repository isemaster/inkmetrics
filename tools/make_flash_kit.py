#!/usr/bin/env python
"""
Сборка комплекта для прошивки прибора на ДРУГОМ компьютере (идёт туда одной папкой).

Что кладём:
  * flash-kit/idf/       — образы прошивки + диск хоста (setup-disk.img) + flash.bat;
  * flash-kit/idf/pc-setup/ — скрипты для ПК (те же, что прибор отдаёт на своём диске):
                           SETUP.CMD, AGENT.PS1, ICS.PS1, NETCHECK.PS1, READRU.TXT, READMEEN.TXT;
  * WHAT-TO-DO.txt       — что делать на том ПК (по-русски);
  * объединённый образ   — одним файлом, если удобнее шить одной строкой.

Зачем: прошивка уезжает на второй ПК, и там не должно быть ни сборки, ни IDF — только
esptool (python) и этот комплект. Комплект собирается из уже собранной прошивки, поэтому
перед запуском сделайте сборку (см. idf/README.md) и образ диска (tools/make_setup_disk.py).

    python tools/make_flash_kit.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "idf" / "build"
KIT = ROOT / "flash-kit" / "idf"
DISK_IMAGE = ROOT / "firmware" / "media" / "setup-disk-big.img"
PC_SETUP_SRC = ROOT / "tools" / "pc_setup"

# Адрес и размер раздела msc (диска хоста) берём из idf/partitions.csv: раньше здесь стоял
# 0x670000 со старым образом на 1,44 МБ, и комплект шил диск в середину нового раздела.
sys.path.insert(0, str(ROOT / "tools"))
import partitions as flash_layout                                    # noqa: E402

MSC_OFFSET, MSC_SIZE = flash_layout.msc_partition(ROOT / "idf" / "partitions.csv")
DISK_NAME = DISK_IMAGE.name

IMAGES = [
    ("0x0", "bootloader.bin", BUILD / "bootloader" / "bootloader.bin"),
    ("0x8000", "partition-table.bin", BUILD / "partition_table" / "partition-table.bin"),
    ("0xe000", "ota_data_initial.bin", BUILD / "ota_data_initial.bin"),
    ("0x20000", "inkmetrics_idf.bin", BUILD / "inkmetrics_idf.bin"),
    (hex(MSC_OFFSET), DISK_NAME, DISK_IMAGE),
]

FLASH_BAT = """@echo off
rem inkmetrics (ESP-IDF build): firmware + host disk image.
rem Usage: flash.bat COM5
rem Put the board into bootloader first: hold BOOT, plug USB, keep 2 s, release.
rem The disk image is written in the same run: it is what the device shows the PC
rem as a 3.69 MB removable drive with instagent.cmd and the instructions.
rem Command names use UNDERSCORES on purpose: esptool 4.x accepts only that form,
rem 5.x accepts both, so this spelling works with any version (write-flash fails on 4.x).
rem ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .bat as garbage.
setlocal
if "%~1"=="" (
  echo.
  echo Usage: flash.bat COMx     ^(example: flash.bat COM5^)
  echo Ports found:
  python -m serial.tools.list_ports -v
  echo.
  pause
  exit /b 1
)

rem esptool may live in another interpreter than plain "python" - probe both
set PY=python
%PY% -c "import esptool" >nul 2>&1 || set PY=py -3
%PY% -c "import esptool" >nul 2>&1
if errorlevel 1 (
  echo esptool not found. Install it first:  pip install esptool
  pause
  exit /b 1
)

echo Flashing inkmetrics to %~1 ...
rem --after no_reset keeps the board in the bootloader after writing: the port stays the
rem same one and the download flag is cleared below BEFORE the board leaves the bootloader
rem (otherwise the command below races with the USB re-enumeration and fails, leaving a
rem freshly flashed board sitting in the bootloader).
%PY% -m esptool --chip esp32s3 --port %~1 --baud 921600 --after no_reset write_flash -z ^
  0x0      "%~dp0bootloader.bin" ^
  0x8000   "%~dp0partition-table.bin" ^
  0xe000   "%~dp0ota_data_initial.bin" ^
  0x20000  "%~dp0inkmetrics_idf.bin" ^
  __MSC__ "%~dp0__DISK__"
if errorlevel 1 (
  echo.
  echo FAILED. Check the port and that the board is in bootloader mode ^(hold BOOT, plug USB^).
  pause
  exit /b 1
)

rem Clear the sticky RTC bit (a previous /api/boot or self-check fallback sets it) and reset
rem into the application, so USB does not have to be replugged and the board actually starts.
%PY% -m esptool --chip esp32s3 --port %~1 --before no_reset --after watchdog_reset ^
  write_mem 0x6000812C 0x00
if errorlevel 1 (
  echo.
  echo WARNING: could not clear the download flag. If the board does not start, unplug USB
  echo and plug it in again - it will boot normally.
)

echo.
echo DONE. The device should start: the screen shows the summary page (ONLINE / OFFLINE /
echo NO DATA), the PC gets a new disk and a network adapter. On that disk: instagent.cmd
echo installs the agent, README-RU.txt explains the rest.
pause
"""

WHAT_TO_DO = """inkmetrics — прошивка прибора на этом компьютере
=====================================================

Что нужно на этом ПК
--------------------
1. Python 3 (любой свежий) и в нём esptool:
       pip install esptool
   Подходит любая версия esptool: команды в flash.bat записаны так, чтобы работали и на
   4.x, и на 5.x. Драйвер USB у ESP32-S3 системный, ставить ничего не нужно.
2. Больше ничего: ни ESP-IDF, ни arduino-cli, ни исходников. Всё нужное — в папке idf.

Порядок
-------
1. Перевести прибор в режим загрузчика: зажать BOOT, не отпуская подключить USB,
   подержать ~2 секунды, отпустить. Порт определится сам (обычно COM5 и выше).
2. Запустить:      idf\\flash.bat COM5          (подставьте свой порт)
   Список портов:  python -m serial.tools.list_ports -v
   Одним запуском пишутся и прошивка, и диск прибора: файл setup-disk-big.img лежит рядом
   и уходит в свой раздел тем же flash.bat.
Если что-то не так
-------------------
* "esptool not found"  — не установлен esptool:  pip install esptool
* "Could not open COM5" или порт не виден — отключите USB и подключите снова; порт может
  смениться, посмотрите список портов заново.
* Прибор после заливки не поднялся (экран пуст, нет диска и сетевой карты) — отключите USB
  и подключите снова: прибор запустится в обычном режиме. flash.bat сам снимает флаг
  загрузчика до сброса, поэтому такой случай — редкость.
* Диск виден, но instagent.cmd не ставится — запускать от имени администратора.

3. После прошивки прибор запустится сам: на экране появится сводный экран (рамка
   ONLINE / OFFLINE / NO DATA), компьютер увидит новый диск и сетевую карту.
   Если хочется проверить файлы до прошивки:  certutil -hashfile inkmetrics_idf.bin SHA256
   и сверить с SHA256SUMS.txt в этой же папке.

Что происходит дальше (важно)
-----------------------------
Прибор отдаёт два устройства: сетевую карту и диск на 3,69 МБ. На диске лежат
скрипты настройки — прибор приносит их с собой, ставить ничего заранее не нужно:

    instagent.cmd  двойной клик: ставит адрес 192.168.7.2 на адаптере прибора, задачу
                   планировщика "inkmetrics agent" и запускает агента (нужны права админа)
    deinstall.cmd  удаление: снимает задачу, возвращает адрес в DHCP, удаляет папку
    README-RU.txt  подробная инструкция по-русски
    README-EN.txt  то же по-английски

Те же файлы лежат в этой папке в pc-setup\\ — можно запускать прямо отсюда, не с диска.

После установки агента
---------------------
Адрес прибора фиксированный: 192.168.7.1 (он же на экране прибора, экран SETUP, строка WEB).
Страница состояния:  http://192.168.7.1/
Настройки прибора:   http://<адрес>/setup   (поворот экрана, блокировка записи на диск,
                                              цель пинга в интернете)

Если прибор «появляется и теряется» или диск не виден
-----------------------------------------------------
Читаем «чёрный ящик» прибора — он переживает и перезагрузки, и перепрошивку:

  1) прибор в режим загрузчика: зажать BOOT, подключить USB, подержать ~2 с;
  2) python -m esptool --chip esp32s3 --port COMx --before no-reset --after no-reset ^
         read-flash 0x7E0000 0x8000 diag.bin
  3) python diag\\idf_diag.py --file diag.bin

В выводе: счётчик загрузок, причина сброса (4 — паника, 5 — interrupt-wdt, 6 — task-wdt,
1 — включение питания), журнал загрузок и последние строки лога. При панике в логе есть
задача, адрес и стек — этого хватает, чтобы назвать причину.

Проверьте ещё железо: другой USB-порт (лучше прямо на системном блоке, не через хаб и не
через переднюю панель), другой кабель. Если частота мигания порта загрузчика (BOOT зажат)
тоже раз в секунду — дело не в прошивке.

Если метрик нет или прибор ведёт себя странно
---------------------------------------------
Запустите pc-setup\\CHECK.CMD: он проверит прибор, диск, агента, задачу, адрес и метрики.
Подробная инструкция по сети на ПК (адрес на линии прибора, что делать при сбоях) лежит
рядом: pc-setup\\GUIDE-RU.txt, а разбор агента — pc-setup\\README-RU.txt.

Если метрик нет (агент не заработал)
------------------------------------
Запустите CHECK.CMD — он лежит и на диске прибора, и в этой папке (pc-setup\\CHECK.CMD).
Проверка идёт по порядку: прибор на USB, состав диска прибора, установленный агент и его
журнал, задания планировщика, адрес прибора и доставка метрик. Каждая строка получает
вердикт [OK] / [FAIL] / [WARN], в конце печатаются шаги «что делать». Отчёт сохраняется в
%TEMP%\INKMETRICS-CHECK-REPORT.TXT (путь печатается) — его удобно переслать целиком.

Если на экране прибора прочерки и NO DATA
-----------------------------------------
Метрики не приходят: агент на этом ПК не запущен или не видит прибор. Запустите
CHECK.CMD (он делает это за вас) или, вручную, `instagent.cmd`.

Что показывает прибор (две страницы, короткое нажатие PWR переключает)
----------------------------------------------------------------
Сводный: рамка ONLINE (у этого ПК есть интернет) или OFFLINE (интернета нет) или NO DATA
(агент молчит); ниже два крупных числа — что именно, выбирается на странице настроек
прибора (по умолчанию загрузка CPU и загрузка второй карты, можно поставить температуру
карт), затем проценты CPU / RAM / DISK, аптайм ПК и температура/влажность самого прибора.
На SETUP — адрес веб-кабинета (192.168.7.1), что стоит в крупных числах, поворот экрана,
режим диска, цель пинга и версия прошивки.

> Прошивка 0.6.0-idf. Плата Waveshare ESP32-S3-ePaper-1.54 (8 МБ флеша).
"""


def run(cmd: list[str]) -> int:
    res = subprocess.run(cmd, capture_output=True, text=True)
    out = (res.stdout or "") + (res.stderr or "")
    if res.returncode != 0:
        print("не вышло: " + " ".join(cmd))
        print(out.strip()[-400:])
    return res.returncode


def main() -> int:
    missing = [(a, str(p)) for a, _, p in IMAGES if not p.exists()]
    if missing:
        for addr, path in missing:
            print(f"нет файла для {addr}: {path}")
        print("сначала: сборка прошивки и python tools/make_setup_disk.py")
        return 1

    KIT.mkdir(parents=True, exist_ok=True)
    # старый маленький образ (1,44 МБ: раздел msc был 0x170000) в комплекте только путает —
    # теперь диск это setup-disk-big.img на 3,69 МБ, адрес в flash.bat
    stale = KIT / "setup-disk.img"
    if stale.exists():
        stale.unlink()
        print("  убран устаревший setup-disk.img (диск теперь setup-disk-big.img)")
    for _, name, src in IMAGES:
        shutil.copy2(src, KIT / name)
        print(f"  {name:<22} {src.stat().st_size:>9} Б")

    # скрипты для ПК: четыре файла с диска прибора (agent-kit) + диагностика из проекта
    pc_dst = KIT / "pc-setup"
    pc_dst.mkdir(exist_ok=True)
    kit_files = ["instagent.cmd", "deinstall.cmd", "README-RU.txt", "README-EN.txt"]
    for name in kit_files:
        src = ROOT / "agent-kit" / name
        if not src.exists():
            print(f"нет файла для pc-setup: {src} — соберите: python tools/make_agent_kit.py")
            return 1
        shutil.copy2(src, pc_dst / name)
    for name, src in [
        ("CHECK.CMD", PC_SETUP_SRC / "check.cmd"),
        ("CHECK.PS1", PC_SETUP_SRC / "check.ps1"),
        ("FIXDISK.PS1", ROOT / "tools" / "reset_disk_node.ps1"),    # диск прибора не появился
        ("FIXUSB.PS1", ROOT / "tools" / "fix_usb_net.ps1"),        # ремонт USB-сети: адрес без шлюза
        ("GUIDE-RU.txt", ROOT / "docs" / "pc-setup-bridge.md"),   # инструкция по сети на ПК
    ]:
        if not src.exists():
            print(f"нет файла для pc-setup: {src}")
            return 1
        shutil.copy2(src, pc_dst / name)
    # лишние файлы (от прежних наборов) убираем: в комплекте должен быть ровно этот набор
    keep = set(kit_files) | {"CHECK.CMD", "CHECK.PS1", "FIXDISK.PS1", "FIXUSB.PS1", "GUIDE-RU.txt"}
    for p in pc_dst.iterdir():
        if p.is_file() and p.name not in keep:
            p.unlink()
            print(f"  убран устаревший {p.name}")
    print(f"  pc-setup/              {len(list(pc_dst.iterdir()))} файлов")

    (KIT / "flash.bat").write_bytes(
        FLASH_BAT.replace("__MSC__", hex(MSC_OFFSET)).replace("__DISK__", DISK_NAME)
        .replace("\n", "\r\n").encode("ascii"))
    (KIT / "START-HERE.txt").write_bytes("\ufeff".encode("utf-8") + WHAT_TO_DO.replace("\n", "\r\n").encode("utf-8"))
    old_what = KIT / "WHAT-TO-DO.txt"
    if old_what.exists():
        old_what.unlink()        # переименовали в START-HERE.txt
    print("  flash.bat, START-HERE.txt")

    # разбор «чёрного ящика» — прямо на том ПК (нужен только esptool, pyserial идёт с ним)
    diag_dst = KIT / "diag"
    diag_dst.mkdir(exist_ok=True)
    for name, src in [("idf_diag.py", ROOT / "tools" / "idf_diag.py"),
                      ("diag_test_make.py", ROOT / "tools" / "diag_test_make.py")]:
        if src.exists():
            shutil.copy2(src, diag_dst / name)
    print(f"  diag/                  {len(list(diag_dst.iterdir()))} файлов")

    # объединённый образ одним файлом — ТОЛЬКО по ключу --merged: он весит ~8 МБ
    # (заполняется до всего флеша), а для прошивки хватает flash.bat
    merged = KIT / "inkmetrics-idf-0x0.bin"
    if "--merged" in sys.argv:
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge-bin",
               "-o", str(merged), "--flash-mode", "dio", "--flash-freq", "80m",
               "--flash-size", "8MB"]
        for addr, name, _ in IMAGES:
            cmd += [addr, str(KIT / name)]
        if run(cmd) == 0 and merged.exists():
            print(f"  объединённый образ     {merged.stat().st_size} Б")
        else:
            print("  объединённый образ не собрался (не критично: можно шить flash.bat)")
    elif merged.exists():
        merged.unlink()          # старый, от прошлой сборки, только путает
        print("  объединённый образ убран (нужен — запустите с ключом --merged)")

    # контрольные суммы + архив для переноса на другой ПК
    import hashlib
    import zipfile
    lines = []
    for p in sorted(KIT.rglob("*")):
        if p.is_file() and p.name != "SHA256SUMS.txt":
            rel = p.relative_to(KIT).as_posix()
            lines.append(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {rel}")
    (KIT / "SHA256SUMS.txt").write_bytes("\n".join(lines).encode("utf-8") + b"\n")

    stamp = time.strftime("%Y-%m-%d")
    zip_path = ROOT / f"flash-kit-inkmetrics-{stamp}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(KIT.rglob("*")):
            if p.is_file():
                z.write(p, Path("inkmetrics-kit") / p.relative_to(KIT))
    print(f"  архив для переноса   {zip_path.name} ({zip_path.stat().st_size} Б)")

    print(f"комплект готов: {KIT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
