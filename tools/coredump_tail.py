#!/usr/bin/env python
"""
Разбор ХВОСТА дампа паники — когда файл с платы целиком не перетащить.

Дамп на плате лежит в разделе coredump (0x7F0000, 64 КБ). Первые 4 байта области —
размер дампа; служебные ноты (кто упал, из-за чего, где) лежат В КОНЦЕ, поэтому
достаточно последних ~768 байт.

Как взять хвост на другом ПК (одна короткая команда):

    python -m esptool --chip esp32s3 --port COM20 --before no-reset --after no-reset \
        read-flash 0x7F0000 0x10000 core.bin
    python -c "import struct;d=open('core.bin','rb').read();n=struct.unpack_from('<I',d,0)[0];print('SIZE',n,'VER',hex(struct.unpack_from('<I',d,4)[0]));print(d[max(0,n-768):n].hex())"

Затем здесь:

    python tools/coredump_tail.py --hex <вставленная строка>
    python tools/coredump_tail.py --file core.bin          # если файл всё-таки скопирован

Формат ноты ELF: namesz(4) descsz(4) type(4) имя(доп. до 4) данные(доп. до 4).
"""
from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

NOTE_NAMES = (
    b"ESP_CORE_DUMP_INFO",
    b"ESP_EXTRA_INFO",
    b"ESP_PANIC_DETAILS_WDT",
    b"ESP_PANIC_DETAILS",
    b"CORE",
)

CAUSE = {
    0: "нет причины", 1: "сброс", 2: "ошибка инструкции", 3: "load/store",
    4: "причина 4 (адрес)", 5: "причина 5 (адрес)", 6: "нелегальная инструкция",
    9: "ошибка выравнивания", 20: "инструкция читает память", 24: "инструкция пишет память",
    28: "деление на ноль", 29: "ошибка деления", 33: "ошибка окна регистров",
}


def align4(n: int) -> int:
    return (n + 3) & ~3


def show_note(pos: int, data: bytes) -> None:
    """pos — позиция имени ноты; заголовок лежит за 12 байт до неё."""
    if pos < 12:
        return
    namesz, descsz, ntype = struct.unpack_from("<III", data, pos - 12)
    if not (0 < namesz <= 64 and 0 < descsz <= 4096):
        return
    name = data[pos:pos + namesz].split(b"\x00")[0].decode("ascii", "replace")
    if name.encode() not in NOTE_NAMES:
        return
    dpos = pos + align4(namesz)
    desc = data[dpos:dpos + descsz]
    print("=" * 72)
    print(f"нота «{name}» тип {ntype}, данных {len(desc)} Б")
    if name == "ESP_CORE_DUMP_INFO":
        ver = struct.unpack_from("<I", desc, 0)[0]
        sha = desc[4:].split(b"\x00")[0].decode("ascii", "replace")
        print(f"  версия coredump: {ver}")
        print(f"  SHA прошивки:    {sha}  ← сверять с APP_SHA256.txt архива сборки")
    elif name in ("ESP_PANIC_DETAILS", "ESP_PANIC_DETAILS_WDT"):
        print(f"  ТЕКСТ: {desc.decode('utf-8', 'replace').strip()}")
        m = re.search(r"PC 0x([0-9a-fA-F]+)", desc.decode("utf-8", "replace"))
        if m:
            print(f"  ПРИЧИНА — вызов по адресу 0x{m.group(1)}: искать по .map/addr2line той сборки")
    elif name == "ESP_EXTRA_INFO":
        tcb, cc_idx, cc, cv_idx, cv = struct.unpack_from("<5I", desc, 0)
        print(f"  ПРИЧИНА ИСКЛЮЧЕНИЯ: {cc} ({CAUSE.get(cc, 'см. таблицу Xtensa')})")
        print(f"  адрес (excvaddr):   0x{cv:08X}, TCB задачи: 0x{tcb:08X}")
        epc = []
        for i in range((len(desc) - 24) // 8):
            idx, val = struct.unpack_from("<2I", desc, 20 + i * 8)
            if 177 <= idx <= 183:
                epc.append(f"EPC{idx - 176}=0x{val:08x}")
        if epc:
            print("  PC по уровням прерываний: " + " ".join(epc))
    else:
        pc, ps = struct.unpack_from("<2I", desc, 0)
        print(f"  pc=0x{pc:08X} ps=0x{ps:08X}")


def main() -> int:
    if "--hex" in sys.argv:
        raw = sys.argv[sys.argv.index("--hex") + 1]
        data = bytes.fromhex(re.sub(r"[^0-9a-fA-F]", "", raw))
        print(f"вставлено {len(data)} байт")
    elif "--file" in sys.argv:
        data = Path(sys.argv[sys.argv.index("--file") + 1]).read_bytes()
        if len(data) > 8 and data[:4] != b"\x7fELF":
            n = struct.unpack_from("<I", data, 0)[0]
            print(f"размер дампа по шапке IDF: {n} Б (файл {len(data)} Б)")
        print(f"файл {len(data)} Б")
    else:
        print(__doc__)
        return 2

    found = False
    for name in NOTE_NAMES:
        start = 0
        while True:
            pos = data.find(name, start)
            if pos < 0:
                break
            start = pos + 1
            show_note(pos, data)
            found = True
    if not found:
        print("ни одной знакомой ноты в этом куске нет — взять хвост подлиннее")
        return 1
    print("=" * 72)
    print("печать понятного человеку текста (если он есть в дампе):")
    for s in re.findall(rb"[ -~]{8,}", data):
        t = s.decode("ascii", "replace")
        if any(k in t for k in ("abort", "assert", "Assert", "CORRUPT", "stack", "wdt", "WDT",
                                "PC 0x", "Guru", "panic", "Panic", "task")):
            print("   ", t)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
