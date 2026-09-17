#!/usr/bin/env python
"""
Снять «чёрный ящик» с прибора — раздел `diag` (0x7E0000, 64 КБ). ТОЛЬКО ЧТЕНИЕ.

Зачем свой инструмент: `tools/try_flash.py` заливает прошивку, а здесь нужно ровно
обратное — ничего не записывать, снять лог уже работающей сборки и разобрать его.
Плата при этом должна быть в режиме загрузчика (иначе её USB занят TinyUSB и порта нет).

Как запускать:
    python tools/idf_diag_read.py               # ждёт порт 5 минут
    python tools/idf_diag_read.py --wait 300

Грабли (проверено 17.09): зажать BOOT, НЕ отпуская подключить USB, подержать ~2 с.
После чтения чип остаётся в загрузчике (`--after no-reset`) — передёрните USB,
чтобы прибор вернулся к работе. Флаги `no-reset` здесь обязательны: без них esptool
сбрасывает чип и лог теряется вместе с началом новой загрузки.
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

from serial.tools import list_ports

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

DIAG_ADDR = 0x7E0000
DIAG_SIZE = 0x10000


def ports() -> list[str]:
    return [p.device for p in list_ports.comports() if "303A" in (p.hwid or "").upper()]


def main() -> int:
    wait = 300
    if "--wait" in sys.argv:
        wait = int(sys.argv[sys.argv.index("--wait") + 1])

    print(f"жду порт прибора (зажать BOOT + передёрнуть USB), до {wait} с…", flush=True)
    t0 = time.time()
    port = None
    while time.time() - t0 < wait:
        ps = ports()
        if ps:
            port = ps[0]
            break
        time.sleep(1)
    if not port:
        print("порт не появился — плата не в режиме загрузчика", flush=True)
        return 2

    out = ROOT / "firmware" / ("diag-" + time.strftime("%Y%m%d-%H%M%S") + ".bin")
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
           "--before", "no-reset", "--after", "no-reset",
           "read-flash", hex(DIAG_ADDR), hex(DIAG_SIZE), str(out)]
    print("читаю: " + " ".join(cmd), flush=True)
    res = subprocess.run(cmd, capture_output=True, text=True)
    tail = ((res.stdout or "") + (res.stderr or "")).strip().splitlines()
    print("\n".join(tail[-6:]), flush=True)
    if res.returncode != 0 or not out.exists():
        print("чтение не удалось", flush=True)
        return 3

    print(f"сохранено: {out} ({out.stat().st_size} Б)", flush=True)
    print("=== разбор «чёрного ящика» ===", flush=True)
    subprocess.run([sys.executable, str(HERE / "idf_diag.py"), "--file", str(out)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
