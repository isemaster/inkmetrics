#!/usr/bin/env python
"""
Прошивка IDF-сборки с ожиданием порта.

Раскладку образов берём из `flash_args` в папке сборки (её пишет IDF), а не из
своего списка: у нашей прошивки приложение на 0x20000, у примеров IDF — на 0x10000,
и жёсткий список ломается на чужой сборке. Если `flash_args` нет — старый список.

  python tools/try_flash.py D:/inkmetrics/idf/build
  python tools/try_flash.py C:/Users/user/AppData/Local/Temp/eink-ncm/build   # A/B пример
  python tools/try_flash.py D:/inkmetrics/build-diag --tries 30
"""
from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

from serial.tools import list_ports

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

DEFAULT_IMAGES = [
    ("0x0", "bootloader/bootloader.bin"),
    ("0x8000", "partition_table/partition-table.bin"),
    ("0xe000", "ota_data_initial.bin"),
    ("0x20000", "inkmetrics_idf.bin"),
]

# Диск хоста (раздел msc, см. partitions.csv): образ собирает tools/make_setup_disk.py.
# На диске лежат SETUP.CMD и скрипты настройки ПК — прибор приносит их с собой.
# Отключается ключом --no-disk (если на диске есть чужие файлы, их не затираем).
MSC_DISK_OFFSET = 0x670000
MSC_DISK_IMAGE = ROOT / "firmware" / "media" / "setup-disk.img"


def disk_image() -> Path | None:
    if "--no-disk" in sys.argv:
        return None
    if not MSC_DISK_IMAGE.exists():
        print(f"образа диска нет ({MSC_DISK_IMAGE}) — соберите: python tools/make_setup_disk.py",
              flush=True)
        return None
    return MSC_DISK_IMAGE


def layout(build: str) -> tuple[list[str], list[tuple[str, str]]]:
    """(флаги esptool, [(адрес, файл)]) — из flash_args, если он есть."""
    fa = Path(build) / "flash_args"
    if not fa.exists():
        return [], list(DEFAULT_IMAGES)
    flags: list[str] = []
    images: list[tuple[str, str]] = []
    for line in fa.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("--"):
            # в flash_args IDF пишет подчёркивания (--flash_mode), а esptool CLI ждёт дефисы
            flags += line.replace("_", "-").split()
        else:
            addr, rel = line.split(None, 1)
            images.append((addr, rel.strip()))
    return flags, images


def ports() -> list[str]:
    return [p.device for p in list_ports.comports() if "303A" in (p.hwid or "").upper()]


# RTC_CNTL_OPTION1_REG: DR_REG_RTCCNTL_BASE (0x60008000) + 0x12C, бит 0 = FORCE_DOWNLOAD_BOOT
RTC_CNTL_OPTION1_REG = 0x6000812C


def reset_to_app(port: str) -> None:
    """
    Снять «липкий» RTC-бит FORCE_DOWNLOAD_BOOT и сбросить чип в приложение.

    Бит ставит либо GET /api/boot, либо автоматический уход в загрузчик (idf/main/selfcheck.c).
    Пока бит установлен, ROM уходит в загрузчик при КАЖДОМ сбросе — то есть после записи
    плата снова оказалась бы в загрузчике, пока не передёрнешь USB. Пишем 0 прямо в
    регистр через загрузчик и сбрасываем чип.
    """
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
           "--before", "no-reset", "--after", "watchdog-reset",
           "write-mem", hex(RTC_CNTL_OPTION1_REG), "0x00"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    out = ((res.stdout or "") + (res.stderr or "")).strip()
    if res.returncode == 0:
        print("RTC-бит загрузчика снят, чип сброшен в приложение", flush=True)
        return
    print("снять RTC-бит не вышло (" + out.splitlines()[-1] + "), просто сбрасываю чип", flush=True)
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
                    "--before", "no-reset", "--after", "watchdog-reset", "chip-id"],
                   capture_output=True, text=True)


def archive_build(build: str) -> None:
    """
    Сохранить ELF и .map сборки перед заливкой: coredump и адреса паники
    расшифровываются ТОЛЬКО по ELF именно той сборки, что была прошита.

    Грабля, из-за которой это появилось: дважды подряд ELF прошитой сборки
    перезаписывался следующей сборкой, и дамп паники становился бесполезен.
    """
    src = Path(build)
    if not src.is_dir():
        return
    try:
        rev = subprocess.run(["git", "-C", str(src), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or "nogit"
    except Exception:
        rev = "nogit"
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dest = ROOT / "firmware" / "idf-builds" / f"{stamp}-{rev}"
    dest.mkdir(parents=True, exist_ok=True)
    copied = []
    for name in ("inkmetrics_idf.bin", "inkmetrics_idf.elf", "inkmetrics_idf.map", "flash_args"):
        p = src / name
        if p.exists():
            shutil.copy2(p, dest / name)
            copied.append(name)
    (dest / "GIT_REV.txt").write_text(rev + "\n", encoding="utf-8")
    binp = dest / "inkmetrics_idf.bin"
    if binp.exists():
        h = hashlib.sha256(binp.read_bytes()).hexdigest()
        (dest / "APP_SHA256.txt").write_text(h + "\n", encoding="utf-8")
    print(f"сборка сохранена в {dest} ({', '.join(copied)})", flush=True)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    build = sys.argv[1].rstrip("/")
    tries = 20
    if "--tries" in sys.argv:
        tries = int(sys.argv[sys.argv.index("--tries") + 1])

    flags, images = layout(build)
    disk = disk_image()
    if disk:
        images = images + [(hex(MSC_DISK_OFFSET), str(disk))]
        print(f"диск хоста: {disk.name} ({disk.stat().st_size} Б) → {hex(MSC_DISK_OFFSET)}", flush=True)
    print("образы: " + ", ".join(f"{a} {f}" for a, f in images), flush=True)
    archive_build(build)      # ELF и .map — рядом, иначе дамп паники не расшифровать

    for attempt in range(1, tries + 1):
        ps = ports()
        if not ps:
            print(f"[{attempt}] портов нет, ждём…", flush=True)
            time.sleep(1.5)
            continue
        port = ps[0]
        print(f"[{attempt}] порт {port}, прошиваю", flush=True)
        # --after no-reset: чип остаётся в загрузчике, чтобы следующим шагом снять
        # RTC-бит (иначе он снова уйдёт в загрузчик и приложение не запустится)
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
               "--before", "default-reset", "--after", "no-reset",
               "--baud", "921600", "write-flash"] + flags + ["-z"]
        for addr, rel in images:
            cmd += [addr, f"{build}/{rel}"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        out = (res.stdout or "") + (res.stderr or "")
        if res.returncode == 0 and "Hash of data verified" in out:
            print("ПРОШИТО УСПЕШНО", flush=True)
            reset_to_app(port)
            return 0
        lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
        print("   не вышло: " + " | ".join(lines[-2:]), flush=True)
        time.sleep(1.5)
    print("НЕ УДАЛОСЬ поймать порт — нужен BOOT + передёрнуть USB", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
