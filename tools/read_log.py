#!/usr/bin/env python
"""
Чтение логов платы из USB-Serial/JTAG (и из CDC-порта прошивки).

Питфол, который стоил часа отладки: у USB-Serial/JTAG линии DTR/RTS управляют
EN и GPIO0 (как кнопки RESET и BOOT). Открыть порт с DTR=0/RTS=0 — это буквально
«зажми BOOT и дёрни сброс», то есть команда «уйти в загрузчик». После такого
открытия приложение не бежит, и логов, разумеется, нет.

  python tools/read_log.py COM3 15      # читать 15 секунд
  python tools/read_log.py COM3 15 --reset   # сначала сбросить чип (RTS-импульс)
"""
from __future__ import annotations

import sys
import time

import serial


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    do_reset = "--reset" in sys.argv

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.5
    # «чип работает»: GPIO0 высокий (BOOT отпущен), EN высокий (нет сброса)
    s.dtr = True
    s.rts = True
    s.open()
    try:
        if do_reset:
            s.rts = False
            time.sleep(0.12)
            s.rts = True
            time.sleep(0.05)
        time.sleep(0.3)
        s.reset_input_buffer()
        print(f"--- логи {port}, {secs:.0f} с ---", flush=True)
        t0 = time.time()
        lines = 0
        while time.time() - t0 < secs:
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            if line:
                print(line, flush=True)
                lines += 1
        print(f"--- строк: {lines} ---")
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
