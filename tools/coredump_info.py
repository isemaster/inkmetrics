#!/usr/bin/env python
"""
Разбор дампа паники (раздел coredump, 0x7F0000) БЕЗ IDF, GDB и ELF.

Зачем: строки «Guru Meditation» идут через ROM-консоль и в «чёрный ящик» не попадают,
а esp-coredump требует xtensa-gdb и ELF именно той сборки, которая была прошита (сборку
легко потерять). При этом сам дамп — обычный ELF, и IDF кладёт в него ноты
(`ESP_EXTRA_INFO`) с причиной исключения, адресом и регистрами. Этого достаточно,
чтобы понять МЕСТО падения: PC и указатель стека (SP) находятся по .map прошивки.

    python tools/coredump_info.py firmware/backup/core.bin

Что печатает: тип ELF, ноты, SHA прошивки (какая сборка дала дамп), причину исключения,
виртуальный адрес, EPC (PC) по уровням прерываний и регистры a0..a15.
Причину (exc_cause) читать по таблице Xtensa: 0 — нет, 2 — инструкция, 3 — load/store,
4/5 — адрес, 6 — нелегальная инструкция, 9 — выравнивание, 20 — инструкция с чтением
памяти, 28/29 — деление, 33 — окна регистров.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EM_XTENSA = 94
PT_NOTE = 4
PT_LOAD = 1

CAUSE = {
    0: "IllegalInstruction (при abort() причина не выставляется — 0)",
    1: "Syscall", 2: "InstructionFetchError", 3: "LoadStoreError", 4: "Level1Interrupt",
    5: "Alloca", 6: "IntegerDivideByZero (деление на ноль)", 7: "PCValue", 8: "Privileged",
    9: "LoadStoreAlignment (невыровненный доступ)",
    12: "InstrPDAddrError", 13: "LoadStorePIFDataError", 14: "InstrPIFAddrError",
    15: "LoadStorePIFAddrError", 16: "InstTLBMiss", 17: "InstTLBMultiHit", 18: "InstFetchPrivilege",
    20: "InstrFetchProhibited", 24: "LoadStoreTLBMiss", 25: "LoadStoreTLBMultihit",
    26: "LoadStorePrivilege",
    28: "LoadProhibited (чтение по недопустимому адресу)",
    29: "StoreProhibited (запись по недопустимому адресу)",
    32: "Cp0Dis", 33: "Cp1Dis",
}


def find_elf(data: bytes) -> tuple[int, str]:
    """Найти ELF внутри дампа: IDF кладёт перед ним свою 24-байтную шапку.

    Шапка (из $IDF/components/espcoredump/src/core_dump_common.h): размер данных,
    версия формата, контрольная сумма. Сам ELF начинается сразу после неё.
    """
    if data[:4] == b"\x7fELF":
        return 0, "ELF в самом начале (без шапки IDF)"
    off = data.find(b"\x7fELF", 0, 256)
    if off < 0:
        return -1, "ELF не найден"
    size, version, crc = struct.unpack_from("<3I", data, 0)[:3]
    return off, (f"шапка IDF: размер {size} Б, версия 0x{version:08X}, crc 0x{crc:08X};"
                 f" ELF со смещения {off}")


def notes(data: bytes) -> list[tuple[str, int, bytes]]:
    """Разобрать PT_NOTE-сегменты: (имя, тип, данные)."""
    elf_off, desc_off = find_elf(data)
    if elf_off < 0:
        raise SystemExit("ELF в дампе не найден — область пуста (0xFF) или дамп побит")
    print(desc_off)
    data = data[elf_off:]
    if len(data) < 52:
        raise SystemExit("файл короче ELF-заголовка")
    is64 = data[4] == 2
    endian = "<" if data[5] == 1 else ">"
    machine = struct.unpack_from(endian + "H", data, 18)[0]
    print(f"ELF{64 if is64 else 32}, machine={machine} "
          f"({'Xtensa' if machine == EM_XTENSA else 'НЕ Xtensa'}), little-endian={data[5] == 1}")

    if is64:
        e_phoff = struct.unpack_from(endian + "Q", data, 32)[0]
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 54)
    else:
        e_phoff = struct.unpack_from(endian + "I", data, 28)[0]
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 42)
    print(f"заголовков программ: {e_phnum} по {e_phentsize} Б, начало 0x{e_phoff:X}")

    out = []
    loads = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if off + e_phentsize > len(data):
            break
        p_type = struct.unpack_from(endian + "I", data, off)[0]
        if is64:
            p_offset, p_vaddr = struct.unpack_from(endian + "QQ", data, off + 8)
            p_filesz = struct.unpack_from(endian + "Q", data, off + 32)[0]
        else:
            p_offset, p_vaddr, _, p_filesz = struct.unpack_from(endian + "IIII", data, off + 4)
        seg = data[p_offset:p_offset + p_filesz]
        if p_type == PT_LOAD:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == PT_NOTE:
            pos = 0
            while pos + 12 <= len(seg):
                n_namesz, n_descsz, n_type = struct.unpack_from(endian + "III", seg, pos)
                pos += 12
                name = seg[pos:pos + n_namesz].split(b"\x00")[0].decode("ascii", "replace")
                pos += (n_namesz + 3) & ~3
                desc = seg[pos:pos + n_descsz]
                pos += (n_descsz + 3) & ~3
                if not name and not desc:
                    break
                out.append((name, n_type, desc))
    print(f"сегментов PT_LOAD: {len(loads)}, нот: {len(out)}")
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = Path(sys.argv[1])
    data = path.read_bytes()
    print(f"дамп: {path} ({len(data)} Б)")
    if find_elf(data)[0] < 0:
        print("дамп не найден — в области нет ELF (прошивка не падала, дамп стёрт или пусто)")
        return 1

    for name, ntype, desc in notes(data):
        print("=" * 72)
        printable = bytes(b for b in desc if 32 <= b < 127)
        print(f"нота «{name}» тип {ntype}, данных {len(desc)} Б")
        if name == "ESP_CORE_DUMP_INFO":
            # struct { uint32 version; char app_elf_sha256[]; }
            ver = struct.unpack_from("<I", desc, 0)[0]
            sha = desc[4:].split(b"\x00")[0].decode("ascii", "replace")
            print(f"  версия coredump: {ver}")
            print(f"  SHA прошивки:    {sha}  ← видно, какая сборка дала дамп")
        elif name == "ESP_PANIC_DETAILS":
            # текстовая строка из g_panic_abort_details, например:
            #   abort() was called at PC 0x400d1234 on core 0
            print(f"  ТЕКСТ: {desc.decode('utf-8', 'replace').strip()}")
            print("  (в адресе PC ищи по .map прошивки ту сборку, что дала дамп)")
        elif name == "ESP_EXTRA_INFO":
            # xtensa_extra_info_t (packed): tcb, {idx,val} exccause, {idx,val} excvaddr,
            # extra_regs[16] (пары индекс/значение), isr_context
            tcb, cc_idx, cc, cv_idx, cv = struct.unpack_from("<5I", desc, 0)
            isr = struct.unpack_from("<I", desc, len(desc) - 4)[0]
            print(f"  ПРИЧИНА ИСКЛЮЧЕНИЯ: {cc} ({CAUSE.get(cc, 'см. таблицу Xtensa')})")
            print(f"  адрес (excvaddr):   0x{cv:08X}")
            print(f"  TCB упавшей задачи: 0x{tcb:08X}, контекст прерывания: {isr}")
            epc = []
            for i in range(16):
                idx, val = struct.unpack_from("<2I", desc, 20 + i * 8)
                if 177 <= idx <= 183:
                    epc.append(f"EPC{idx - 176}=0x{val:08x}")
            if epc:
                print("  PC по уровням прерываний: " + " ".join(epc))
        elif name == "ESP_PANIC_DETAILS_WDT":
            print(f"  ТЕКСТ: {desc.decode('utf-8', 'replace').strip()}")
        elif name == "CORE":
            pc, ps = struct.unpack_from("<2I", desc, 0)
            print(f"  pc=0x{pc:08X} ps=0x{ps:08X}"
                  + ("   (пусто — регистры этой задачи не снимались)" if pc == 0 else ""))
        else:
            print("  первые слова:", " ".join(f"{w:08x}" for w in struct.unpack_from("<8I", desc, 0)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
