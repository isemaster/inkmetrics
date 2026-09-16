#!/usr/bin/env python
"""
Чтение «чёрного ящика» прошивки (раздел diag) с платы.

Зачем: в IDF-прошивке консоли нет (USB-CDC и USB-Serial/JTAG отключены — общий
PHY с TinyUSB, UART наружу не выведен). Логи прошивки (см. idf/main/diag.c)
складываются в раздел `diag` и читаются отсюда через esptool.

Как: плату перевести в режим загрузчика — зажать BOOT, не отпуская передёрнуть
USB (или отпустить BOOT через пару секунд), затем:

    python tools/idf_diag.py                 # сам найдёт порт ROM
    python tools/idf_diag.py COM3            # если порт задан явно
    python tools/idf_diag.py --raw out.bin   # сохранить сырой дамп без разбора

Смещение раздела берётся из idf/partitions.csv — если таблица менялась,
скрипт подхватит новое автоматически.
"""
from __future__ import annotations

import re
import struct
import subprocess
import sys
import time
from pathlib import Path

from serial.tools import list_ports

PART = Path(__file__).resolve().parent.parent / "idf" / "partitions.csv"
MAGIC = 0x31474144  # "DAG1"
JOURNAL_MAGIC = 0x4D524A44  # "DJRM" — журнал загрузок (idf/main/diag.c)
JOURNAL_OFF = 0x4000        # сразу за стираемой областью лога
JOURNAL_LEN = 0x1000        # один сектор: 256 записей по 16 байт

REASONS = {0: "неизвестно", 1: "power-on", 3: "software", 4: "panic", 5: "interrupt-wdt",
           6: "task-wdt", 7: "watchdog", 8: "deep-sleep", 9: "brownout", 10: "sdio",
           11: "usb", 12: "jtag", 13: "efuse", 15: "cpu-lockup"}


def print_journal(data: bytes) -> None:
    """Журнал загрузок: последовательность причин сброса, копится между загрузками.

    Нужен, когда прошивка перезагружается в цикле: лог каждой загрузки стирается,
    а здесь видно, ЧЕМ заканчиваются загрузки одна за другой (4,4,4 — паника;
    5,5,5 — interrupt-wdt), и на какой по счёту записи цикл начался.
    """
    if len(data) < JOURNAL_OFF + 16:
        print("журнал загрузок: дамп короче 0x4000 — не читался")
        return
    entries = []
    for off in range(JOURNAL_OFF, min(len(data), JOURNAL_OFF + JOURNAL_LEN), 16):
        magic, boots, reason, uptime = struct.unpack_from("<4I", data, off)
        if magic != JOURNAL_MAGIC:
            break
        entries.append((boots, reason, uptime))
    print("=" * 72)
    if not entries:
        print("ЖУРНАЛ ЗАГРУЗОК пуст — прошивка с журналом ещё не стартовала")
        return
    print(f"ЖУРНАЛ ЗАГРУЗОК: {len(entries)} записей (не стирается, копится между загрузками)")
    for boots, reason, uptime in entries[-16:]:
        print(f"  загрузка #{boots:<5} причина сброса {reason:<3} {REASONS.get(reason, '?'):<14}"
              f" (ящик поднялся через {uptime} мс)")
    counts = {}
    for _, reason, _ in entries:
        counts[reason] = counts.get(reason, 0) + 1
    seq = ", ".join(f"{REASONS.get(r, r)} ×{n}" for r, n in sorted(counts.items(), key=lambda x: -x[1]))
    print(f"  итог: {seq}")
    if len(entries) >= 3 and len(counts) == 1:
        print("  ВЫВОД: все загрузки кончаются одинаково — это не разовый сбой, а цикл.")
    print("=" * 72)


def part_info(name: str = "diag") -> tuple[int, int]:
    """Смещение и размер раздела из partitions.csv."""
    for line in PART.read_text(encoding="utf-8").splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(",")]
        if f and f[0] == name:
            return int(f[3], 0), int(f[4], 0)
    raise SystemExit(f"раздел {name} не найден в {PART}")


def rom_port() -> str | None:
    for p in list_ports.comports():
        if "303A" in (p.hwid or "").upper():
            return p.device
    return None


def wait_port(tries: int = 20, delay: float = 2.0) -> str | None:
    for i in range(tries):
        p = rom_port()
        if p:
            return p
        print(f"  [{i + 1}/{tries}] порта ROM нет — зажми BOOT и передёрни USB", flush=True)
        time.sleep(delay)
    return None


def main() -> int:
    # позиционные аргументы = имя порта; значения опций (--file X и т. п.) в них не попадают
    value_opts = {"--raw", "--file", "--wait-port"}
    args: list[str] = []
    i = 1
    while i < len(sys.argv):
        a = sys.argv[i]
        if a in value_opts:
            i += 2
            continue
        if a.startswith("--"):
            i += 1
            continue
        args.append(a)
        i += 1

    raw_out = None
    if "--raw" in sys.argv:
        raw_out = sys.argv[sys.argv.index("--raw") + 1]

    offset, size = part_info()
    size = min(size, 0x8000)          # 0x4000 — лог, следующие 0x1000 — журнал загрузок

    given = None
    if "--file" in sys.argv:                      # разобрать уже снятый дамп
        given = Path(sys.argv[sys.argv.index("--file") + 1])

    if given:
        dump = given
        print(f"разбираю дамп {dump}")
    else:
        port = args[0] if args else rom_port()
        if not port:
            sec = 40
            if "--wait-port" in sys.argv:
                sec = int(sys.argv[sys.argv.index("--wait-port") + 1])
            print(f"Раздел diag: 0x{offset:X}, {size // 1024} КБ — жду порт ROM до {sec} с")
            port = wait_port(tries=max(1, sec // 2))
            if not port:
                print("Не дождался порта. Зажми BOOT и передёрни USB (питание снимать "
                      "не обязательно, если BOOT уже зажат).")
                return 1

        dump = Path(raw_out) if raw_out else Path(__file__).resolve().parent / "diag-raw.bin"
        print(f"порт {port}: читаю 0x{offset:X} ({size} Б)")
        res = subprocess.run(
            [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
             "--before", "no-reset", "--after", "no-reset",
             "read-flash", hex(offset), hex(size), str(dump)],
            capture_output=True, text=True,
        )
        out = (res.stdout or "") + (res.stderr or "")
        if res.returncode != 0 or not dump.exists():
            print("чтение не удалось:\n" + "\n".join(out.strip().splitlines()[-6:]))
            return 1

    data = dump.read_bytes()
    if len(data) < 64:
        print("дамп пустой")
        return 1
    magic, boots, reason, uptime = struct.unpack_from("<4I", data)
    if magic != MAGIC:
        print(f"заголовка нет (0x{magic:08X}) — область ещё не заполнялась "
              f"(прошивка не дошла до diag_init); сырые данные: {dump}")
        return 1

    # прошивка пишет потоком: текст идёт с 0x40 до первого нестёртого байта (0xFF).
    # Но если прошивка добивала хвост записи байтами 0xFF (старые сборки), лог
    # оказывается «разорванным» на куски — поэтому дополнительно собираем все
    # текстовые острова и печатаем их.
    body = data[64:]
    end = body.find(b"\xff")
    text = body[:end if end >= 0 else len(body)]

    reasons = REASONS
    print("=" * 72)
    print(f"загрузка #{boots}, причина сброса: {reasons.get(reason, reason)}, "
          f"лог на {len(text)} Б (область {len(data) // 1024} КБ)")
    print("=" * 72)
    text_str = text.decode("utf-8", "replace")
    sys.stdout.write(text_str)
    if not text_str.endswith("\n"):
        print()
    print("=" * 72)

    # хвост после добивки 0xFF: текстовые острова, которые «первый блок» не покрыл
    tail_islands = []
    for m in re.finditer(rb"[^\xff]{8,}", body[len(text):]):
        chunk = m.group()
        if any(b < 0x20 and b not in (0x0A, 0x0D) for b in chunk):
            continue                        # мусорные байты — не текст
        tail_islands.append((len(text) + m.start(), chunk.decode("utf-8", "replace")))
    if tail_islands:
        print("ХВОСТ ЛОГА (после байтов добивки):")
        for off, chunk in tail_islands:
            sys.stdout.write("  " + chunk.replace("\r", "").replace("\n", "\n  ").rstrip() + "\n")
        print("=" * 72)

    # главное: метки стадий, состояние USB-блока и строки ошибок
    key = [ln for ln in text_str.splitlines()
           if ln.startswith("E (") or "USB[" in ln or "usb_new_phy" in ln]
    if key:
        print("ГЛАВНОЕ:")
        for ln in key:
            print("  " + ln.strip())
        print("=" * 72)
    print_journal(data)

    steps = [ln for ln in text_str.splitlines() if ln.startswith("STEP ")]
    print(f"меток стадий: {len(steps)}" + (f", последняя — {steps[-1][5:].strip()}" if steps else ""))
    print(f"сырой дамп: {dump}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
