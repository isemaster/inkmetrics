#!/usr/bin/env python
"""
Отладочный терминал для платы inkmetrics.

  python tools/serial_test.py                 # перезагрузка + все команды по очереди
  python tools/serial_test.py --boot          # только перезагрузка и лог старта
  python tools/serial_test.py --cmd STAT --cmd DIAG --cmd "WAIT 0" --cmd TEST
  python tools/serial_test.py --listen 10     # просто слушать порт 10 с

Перезагрузка делается через esptool (жёсткий сброс), потому что у USB-Serial/JTAG
на ESP32-S3 нет обычного DTR/RTS-сброса.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

PY = sys.executable


def find_port() -> str:
    for p in list_ports.comports():
        if "303A" in (p.hwid or "").upper():
            return p.device
    for p in list_ports.comports():
        if p.description and "USB Serial" in p.description:
            return p.device
    return ""


def reset(port: str) -> None:
    subprocess.run([PY, "-m", "esptool", "--chip", "esp32s3", "--port", port,
                    "--after", "hard-reset", "chip-id"],
                   capture_output=True, text=True, timeout=60)


def listen(ser: serial.Serial, seconds: float, prefix: str = "плата") -> list[str]:
    out, t0 = [], time.time()
    while time.time() - t0 < seconds:
        line = ser.readline().decode("utf-8", "replace").strip()
        if line:
            print(f"  {prefix}: {line}")
            out.append(line)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--boot", action="store_true")
    ap.add_argument("--listen", type=float, default=0.0)
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("плата не найдена")
        return 2
    print(f"порт: {port}")

    if not args.no_reset:
        reset(port)

    with serial.Serial(port, 115200, timeout=0.3) as ser:
        ser.dtr = True
        ser.rts = True
        print("--- лог старта (3 с) ---")
        listen(ser, 3.0)
        if args.boot:
            return 0

        cmds = args.cmd or ["PING", "DIAG", "STAT", "ENV?", "TEST", "WAIT 0", "TEST",
                            "WAIT 1", "CLR", "STAT", "DIAG"]
        for c in cmds:
            print(f"--- > {c}")
            ser.write((c + "\n").encode("ascii"))
            ser.flush()
            listen(ser, 4.0)

        if args.listen:
            print(f"--- слушаю ещё {args.listen} с")
            listen(ser, args.listen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
