"""Заливка прошивки inkmetrics (ESP-IDF) на прибор из режима загрузчика.

Зачем скрипт: в IDF-прошивке USB занят сетью (TinyUSB RNDIS), поэтому COM-порта в
рабочем режиме НЕТ — он появляется только когда чип в режиме загрузчика (кнопка BOOT
зажата при подключении USB). Скрипт сам находит этот порт, заливает четыре файла
сборки по нужным адресам и проверяет по HTTP, что прибор поднял новую версию.

Запускать питоном, в котором есть esptool (в системе это окружение IDF):
  C:/Users/user/.espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe

  python tools/flash_ingest.py --list          # что видно на USB прямо сейчас
  python tools/flash_ingest.py                 # найти порт загрузчика и залить
  python tools/flash_ingest.py --port COM5     # залить в конкретный порт
  python tools/flash_ingest.py --no-verify     # только залить, без проверки по HTTP

Признаки порта загрузчика: VID 303A, PID 1001 (USB JTAG/serial debug unit) либо
описание «USB Serial» / «Последовательный интерфейс USB». Адреса файлов важны:
у IDF-версии приложение лежит на 0x20000 (две копии под OTA).
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request

VID_ESPRESSIF = 0x303A
PID_BOOTLOADER = 0x1001

DEVICE_URL = "http://192.168.7.1/api/state"
BUILD_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "idf", "build")

# адрес в флеше → файл сборки
IMAGES = [
    ("0x0",     "bootloader/bootloader.bin"),
    ("0x8000",  "partition_table/partition-table.bin"),
    ("0xe000",  "ota_data_initial.bin"),
    ("0x20000", "inkmetrics_idf.bin"),
]


def list_ports():
    from serial.tools import list_ports as lp
    return list(lp.comports())


def find_bootloader_port():
    """Порт ROM-загрузчика. Если такого нет — возвращаем None (прибор ещё в приложении)."""
    for p in list_ports():
        if p.vid == VID_ESPRESSIF and p.pid == PID_BOOTLOADER:
            return p.device
    for p in list_ports():
        desc = (p.description or "") + " " + (p.product or "")
        if p.vid == VID_ESPRESSIF or "JTAG" in desc or "USB Serial" in desc:
            return p.device
    return None


def show_ports():
    ports = list_ports()
    if not ports:
        print("COM-портов нет вообще.")
        return
    for p in ports:
        vidpid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "-"
        print(f"  {p.device:8} {vidpid:10} {p.description}")
    print("\nПорт загрузчика:", find_bootloader_port() or "НЕ НАЙДЕН")
    print("Если не найден: зажать BOOT, передёрнуть USB, подержать ~2 с, отпустить.")


def flash(port):
    missing = [f for _, f in IMAGES if not os.path.exists(os.path.join(BUILD_DIR, f))]
    if missing:
        print("Нет файлов сборки:", ", ".join(missing))
        print("Собери:  python C:/esp/idf_build.py -C", os.path.join(BUILD_DIR, ".."), "build")
        return 1

    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
           "--baud", "921600", "--before", "default_reset", "--after", "hard_reset",
           "write_flash", "-z"]
    for addr, name in IMAGES:
        cmd += [addr, os.path.join(BUILD_DIR, name)]

    print("[flash] " + " ".join(cmd))
    rc = subprocess.run(cmd, cwd=BUILD_DIR).returncode
    print("[flash] esptool вернул", rc)
    return rc


def verify(timeout_s=120):
    """Ждём, пока прибор поднимется и отдаст версию 0.4.0+ с полем ingest."""
    print("[verify] жду прибор по", DEVICE_URL)
    deadline = time.time() + timeout_s
    last = ""
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(DEVICE_URL, timeout=4) as r:
                st = json.loads(r.read().decode("utf-8", "replace"))
            fw = st.get("fw", "?")
            ing = st.get("ingest")
            print(f"[verify] fw={fw} ingest={'есть' if ing else 'НЕТ'} "
                  f"have={ing.get('have') if ing else '-'} age={ing.get('age') if ing else '-'} с")
            if fw.startswith("0.4.") and isinstance(ing, dict):
                print("[verify] OK: прошивка с POST /ingest работает")
                return 0
            last = f"fw={fw}"
        except Exception as e:                 # прибор ещё загружается или сеть не встала
            last = str(e)
        time.sleep(3)
    print("[verify] НЕ ДОЖДАЛСЯ:", last)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="COM-порт (иначе ищем порт загрузчика сами)")
    ap.add_argument("--list", action="store_true", help="только показать порты")
    ap.add_argument("--no-verify", action="store_true", help="не проверять по HTTP")
    args = ap.parse_args()

    if args.list:
        show_ports()
        return 0

    port = args.port or find_bootloader_port()
    if not port:
        print("Порт загрузчика не найден — прибор в рабочем режиме.")
        show_ports()
        print("\nНужно: зажать BOOT, не отпуская передёрнуть USB, подержать ~2 с, отпустить.")
        return 2
    print("[flash] порт:", port)

    rc = flash(port)
    if rc != 0:
        return rc
    if args.no_verify:
        return 0
    return verify()


if __name__ == "__main__":
    raise SystemExit(main())
