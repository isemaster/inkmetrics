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
DISK_IMAGE = ROOT / "firmware" / "media" / "setup-disk.img"
PC_SETUP_SRC = ROOT / "tools" / "pc_setup"

IMAGES = [
    ("0x0", "bootloader.bin", BUILD / "bootloader" / "bootloader.bin"),
    ("0x8000", "partition-table.bin", BUILD / "partition_table" / "partition-table.bin"),
    ("0xe000", "ota_data_initial.bin", BUILD / "ota_data_initial.bin"),
    ("0x20000", "inkmetrics_idf.bin", BUILD / "inkmetrics_idf.bin"),
    ("0x670000", "setup-disk.img", DISK_IMAGE),
]

FLASH_BAT = """@echo off
rem inkmetrics (ESP-IDF build): firmware + host disk image.
rem Usage: flash.bat COM5
rem Put the board into bootloader first: hold BOOT, plug USB, keep 2 s, release.
rem The disk image (setup-disk.img) contains SETUP.CMD - the device brings the PC
rem setup scripts with it (see pc-setup folder for copies).
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
%PY% -m esptool --chip esp32s3 --port %~1 --baud 921600 write-flash -z ^
  0x0      "%~dp0bootloader.bin" ^
  0x8000   "%~dp0partition-table.bin" ^
  0xe000   "%~dp0ota_data_initial.bin" ^
  0x20000  "%~dp0inkmetrics_idf.bin" ^
  0x670000 "%~dp0setup-disk.img"
if errorlevel 1 (
  echo.
  echo FAILED. Check the port and that the board is in bootloader mode ^(hold BOOT, plug USB^).
  pause
  exit /b 1
)

rem Clear the sticky RTC bit (a previous /api/boot or self-check fallback sets it) and reset
rem into the application, so USB does not have to be replugged.
%PY% -m esptool --chip esp32s3 --port %~1 --before no-reset --after no-reset ^
  write-mem 0x6000812C 0x00

echo.
echo DONE. The device should start: screen shows DEVICE page, the PC gets a new disk
echo and a network adapter. Then run pc-setup\\SETUP.CMD on this PC.
pause
"""

WHAT_TO_DO = """inkmetrics — прошивка прибора на этом компьютере
=====================================================

Что нужно на этом ПК
--------------------
1. Python 3 (любой свежий) и в нём esptool:
       pip install esptool
2. Больше ничего: ни ESP-IDF, ни arduino-cli, ни исходников. Всё нужное — в папке idf.

Порядок
-------
1. Перевести прибор в режим загрузчика: зажать BOOT, не отпуская подключить USB,
   подержать ~2 секунды, отпустить. Порт определится сам (обычно COM5 и выше).
2. Запустить:      idf\\flash.bat COM5          (подставьте свой порт)
   Список портов:  python -m serial.tools.list_ports -v
3. После прошивки прибор запустится сам: на экране появится страница DEVICE,
   компьютер увидит новый диск и сетевую карту.

Что происходит дальше (важно)
-----------------------------
Прибор отдаёт два устройства: сетевую карту и диск на 1,44 МБ. На диске лежат
скрипты настройки — прибор приносит их с собой, ставить ничего заранее не нужно:

    SETUP.CMD      двойной клик, подтвердить права администратора (один раз на ПК)
    AGENT.PS1      ставит агента (C:\\ProgramData\\inkmetrics) и задание планировщика
    ICS.PS1        раздача интернета на адаптере прибора: -Off, -DryRun
    NETCHECK.PS1   диагностика: адаптеры, мост, прибор, страница, раздача
    READRU.TXT     инструкция по-русски
    READMEEN.TXT   инструкция по-английски

Те же файлы лежат в этой папке в pc-setup\\ — можно запускать прямо отсюда, не с диска.

После SETUP.CMD
---------------
Адрес страницы прибора виден на его экране, строка ADDR (обычно 192.168.137.x).
Страница состояния:  http://<адрес>/
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

Если интернета у прибора нет
----------------------------
Запустите pc-setup\\NETCHECK.PS1 и посмотрите строки про раздачу и адаптер прибора.
Подробная инструкция по сети на ПК (раздача или мост, что делать при сбоях) лежит рядом:
pc-setup\\GUIDE-RU.txt.

Если прибор показывает NET EMERGENCY и адрес 192.168.7.1
--------------------------------------------------------
Значит раздача ещё не включена. Прибор сам повторит попытку получить адрес в течение
минуты — перезагружать его не нужно.

> Прошивка 0.3.2-idf. Плата Waveshare ESP32-S3-ePaper-1.54 (8 МБ флеша).
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
    for _, name, src in IMAGES:
        shutil.copy2(src, KIT / name)
        print(f"  {name:<22} {src.stat().st_size:>9} Б")

    # скрипты для ПК: те же, что уедут на диске прибора
    pc_dst = KIT / "pc-setup"
    pc_dst.mkdir(exist_ok=True)
    for name, src in [
        ("AGENT.PS1", PC_SETUP_SRC / "agent_install.ps1"),
        ("ICS.PS1", ROOT / "tools" / "ics_enable.ps1"),
        ("NETCHECK.PS1", ROOT / "tools" / "net_check2.ps1"),
        ("READRU.TXT", PC_SETUP_SRC / "README-RU.txt"),
        ("READMEEN.TXT", PC_SETUP_SRC / "README-EN.txt"),
        ("GUIDE-RU.txt", ROOT / "docs" / "pc-setup-bridge.md"),   # полная инструкция по сети на ПК
    ]:
        shutil.copy2(src, pc_dst / name)
    # SETUP.CMD берём из образа диска, чтобы файлы не разъехались
    disk = DISK_IMAGE.read_bytes()
    setup_cmd = extract_from_image(disk, "SETUP.CMD")
    if setup_cmd is None:
        print("в образе диска нет SETUP.CMD — образ собран не тем инструментом?")
        return 1
    (pc_dst / "SETUP.CMD").write_bytes(setup_cmd)
    print(f"  pc-setup/              {len(list(pc_dst.iterdir()))} файлов")

    (KIT / "flash.bat").write_bytes(FLASH_BAT.replace("\n", "\r\n").encode("ascii"))
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


def extract_from_image(img: bytes, name: str) -> bytes | None:
    """Достать файл из образа FAT12 (та же логика, что в проверке make_setup_disk)."""
    sys.path.insert(0, str(ROOT / "tools"))
    import make_msc_image as msc  # noqa: E402

    sector, data_start = 512, msc.DATA_START
    root_start = (msc.RESERVED_SECTORS + msc.NUM_FATS * msc.FAT_SECTORS) * sector
    short = name.upper().partition(".")
    short = short[0].ljust(8) + short[2].ljust(3)
    for e in range(msc.ROOT_ENTRIES):
        off = root_start + e * 32
        entry = img[off:off + 11].decode("ascii", "replace")
        if entry == short:
            first = int.from_bytes(img[off + 26:off + 28], "little")
            size = int.from_bytes(img[off + 28:off + 32], "little")
            data = bytearray()
            cl = first
            while 2 <= cl < 0xFF8:
                data += img[(data_start + (cl - 2)) * sector:(data_start + (cl - 1)) * sector]
                cl = msc._fat12_get(img, cl)
            return bytes(data[:size])
    return None


if __name__ == "__main__":
    raise SystemExit(main())
