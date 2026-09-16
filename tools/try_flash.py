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

import subprocess
import sys
import time
from pathlib import Path

from serial.tools import list_ports

DEFAULT_IMAGES = [
    ("0x0", "bootloader/bootloader.bin"),
    ("0x8000", "partition_table/partition-table.bin"),
    ("0xe000", "ota_data_initial.bin"),
    ("0x20000", "inkmetrics_idf.bin"),
]


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


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    build = sys.argv[1].rstrip("/")
    tries = 20
    if "--tries" in sys.argv:
        tries = int(sys.argv[sys.argv.index("--tries") + 1])

    flags, images = layout(build)
    print("образы: " + ", ".join(f"{a} {f}" for a, f in images), flush=True)

    for attempt in range(1, tries + 1):
        ps = ports()
        if not ps:
            print(f"[{attempt}] портов нет, ждём…", flush=True)
            time.sleep(1.5)
            continue
        port = ps[0]
        print(f"[{attempt}] порт {port}, прошиваю", flush=True)
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
               "--before", "default-reset", "--after", "watchdog-reset",
               "--baud", "921600", "write-flash"] + flags + ["-z"]
        for addr, rel in images:
            cmd += [addr, f"{build}/{rel}"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        out = (res.stdout or "") + (res.stderr or "")
        if res.returncode == 0 and "Hash of data verified" in out:
            print("ПРОШИТО УСПЕШНО", flush=True)
            return 0
        lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
        print("   не вышло: " + " | ".join(lines[-2:]), flush=True)
        time.sleep(1.5)
    print("НЕ УДАЛОСЬ поймать порт — нужен BOOT + передёрнуть USB", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
