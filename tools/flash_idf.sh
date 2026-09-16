#!/usr/bin/env bash
# Прошивка IDF-версии прибора (проект idf/).
#
#   bash tools/flash_idf.sh            # собрать (если нужно) и прошить
#   bash tools/flash_idf.sh --no-build
#
# Особенности:
#   1. Порт приложения (CDC) занят, если открыта страница прибора в браузере или
#      работает агент — их нужно закрыть: порт у USB-CDC один.
#   2. Чтобы попасть в загрузчик, скрипт отправляет плате команду DFU (её умеет
#      Arduino-версия прошивки; IDF-версия уходит в загрузчик удержанием BOOT).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="/c/Python314/python.exe"
BUILD="$ROOT/idf/build"
IDF_PY="$PY"  # сборка идёт через обёртку C:/esp/idf_build.py

DO_BUILD=1
[ "${1:-}" = "--no-build" ] && DO_BUILD=0

list_ports() {
  $PY - <<'PY' | tr -d '\r'
from serial.tools import list_ports
for p in list_ports.comports():
    if "303A" in (p.hwid or "").upper():
        print(p.device)
PY
}

port_free() {
  $PY - "$1" <<'PY' >/dev/null 2>&1
import sys, serial
s = serial.Serial()
s.port = sys.argv[1]; s.baudrate = 115200; s.timeout = 0.3
s.dtr = False; s.rts = False
s.open(); s.close()
PY
}

in_bootloader() {
  $PY -m esptool --chip esp32s3 --port "$1" --before no-reset --after no-reset chip-id >/dev/null 2>&1
}

send_dfu() {
  $PY - "$1" <<'PY' || true
import sys, time, serial
port = sys.argv[1]

def once():
    s = serial.Serial()
    s.port = port; s.baudrate = 115200; s.timeout = 0.6; s.write_timeout = 2
    s.dtr = False; s.rts = False
    s.open()
    try:
        time.sleep(0.3); s.dtr = True; s.rts = True; time.sleep(0.3)
        s.reset_input_buffer(); s.reset_output_buffer()
        s.write(b"\n"); time.sleep(0.2); s.write(b"DFU\n"); s.flush()
        t0 = time.time()
        while time.time() - t0 < 1.5:
            line = s.readline().decode("utf-8", "replace").strip()
            if line:
                print("   плата:", line)
                if "OK DFU" in line:
                    return True
    finally:
        s.close()
    return False

for i in range(4):
    if once():
        print("   плата ушла в загрузчик")
        break
    print(f"   попытка {i+1} не удалась, повтор…")
    time.sleep(1)
PY
}

if [ "$DO_BUILD" = 1 ]; then
  echo "[idf] сборка"
  $PY C:/esp/idf_build.py -C "$(cygpath -w "$ROOT/idf")" build 2>&1 | grep -E "error|FAILED|Project build complete|binary size" | tail -4
fi

[ -f "$BUILD/inkmetrics_idf.bin" ] || { echo "[idf] нет сборки: сначала соберите проект"; exit 4; }

PORTS="$(list_ports)"
if [ -z "$PORTS" ]; then
  echo "[idf] плата не видна. Проверьте кабель. Если плата молчит — зажмите BOOT и передёрните USB."
  exit 2
fi
echo "[idf] порты: $(echo $PORTS | tr '\n' ' ')"

BOOT_PORT=""
APP_PORT=""
for p in $PORTS; do
  if in_bootloader "$p"; then BOOT_PORT="$p"; else APP_PORT="${APP_PORT:-$p}"; fi
done

if [ -z "$BOOT_PORT" ]; then
  if [ -n "$APP_PORT" ] && port_free "$APP_PORT"; then
    echo "[idf] чип в приложении ($APP_PORT) — отправляю DFU"
    send_dfu "$APP_PORT"
  else
    echo "[idf] Порт $APP_PORT занят другой программой (страница прибора в браузере,"
    echo "[idf] агент inkmetrics.py или терминал). Нажмите «Отключить» на странице"
    echo "[idf] либо закройте вкладку и запустите скрипт снова."
    echo "[idf] Запасной путь: зажать BOOT и передёрнуть USB — плата сразу в загрузчике."
    exit 3
  fi
  for i in $(seq 1 15); do
    sleep 1
    for p in $(list_ports); do
      if in_bootloader "$p"; then BOOT_PORT="$p"; break 2; fi
    done
  done
fi

[ -n "$BOOT_PORT" ] || { echo "[idf] загрузчик не поднялся: зажмите BOOT и передёрните USB"; exit 3; }
echo "[idf] порт загрузчика: $BOOT_PORT"

$PY -m esptool --chip esp32s3 --port "$BOOT_PORT" --before no-reset --after hard-reset --baud 921600 write-flash -z \
  0x0     "$(cygpath -w "$BUILD/bootloader/bootloader.bin")" \
  0x8000  "$(cygpath -w "$BUILD/partition_table/partition-table.bin")" \
  0xe000  "$(cygpath -w "$BUILD/ota_data_initial.bin")" \
  0x20000 "$(cygpath -w "$BUILD/inkmetrics_idf.bin")" 2>&1 | grep -E "Wrote|Hash|ERROR|error" | tail -5

echo "[idf] прошито. Через ~10 с хост должен получить адрес от прибора."
echo "[idf] Проверка:  powershell -c \"Get-NetIPAddress -AddressFamily IPv4 | ? {\$_.IPAddress -like '192.168.7.*'}\""
echo "[idf] Страница:  http://192.168.7.1/"
