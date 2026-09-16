#!/usr/bin/env python
"""
Прошивка IDF-сборки с ожиданием порта.

Зачем: если прошивка падает в цикле перезагрузок, порт ROM появляется лишь на
доли секунды. Скрипт опрашивает порты и ждёт окно, затем прошивает и делает
watchdog-сброс (он выводит чип в приложение, не трогая USB-линии).

  python tools/try_flash.py D:/inkmetrics/idf/build
  python tools/try_flash.py D:/inkmetrics/build-diag --tries 30
"""
from __future__ import annotations

import subprocess
import sys
import time

from serial.tools import list_ports

IMAGES = [
    ("0x0", "bootloader/bootloader.bin"),
    ("0x8000", "partition_table/partition-table.bin"),
    ("0xe000", "ota_data_initial.bin"),
    ("0x20000", "inkmetrics_idf.bin"),
]


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
               "--baud", "921600", "write-flash", "-z"]
        for addr, rel in IMAGES:
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
