#!/usr/bin/env python
"""
Сборка содержимого диска, который прибор отдаёт хосту (флешка внутри прибора).

Замысел (упрощённый, 17.09.2026): прибор работает **монитором** — интернета у него нет,
раздачи (ICS) в схеме нет, адреса фиксированные: прибор всегда `192.168.7.1`, компьютер
получает `192.168.7.2` на адаптере прибора (без шлюза). Поэтому на диске нужен только
набор агента, ровно четыре файла:

| Файл на диске | Откуда | Зачем |
|---|---|---|
| instagent.cmd | agent-kit/instagent.cmd | установка агента (двойной клик) |
| deinstall.cmd | agent-kit/deinstall.cmd | удаление агента |
| README-RU.txt | agent-kit/README-RU.txt | инструкция по-русски |
| README-EN.txt | agent-kit/README-EN.txt | то же по-английски |

Собирает это `tools/make_agent_kit.py` — образ берёт готовые файлы из `agent-kit/`, поэтому
на диске и в папке лежит одно и то же (проверяется побайтово).

Раскладка: MBR + раздел FAT16 на весь раздел `msc` (7552 сектора = 3,69 МБ), сборку делает
`tools/make_disk_image.py`. Так Windows заводит обычный съёмный диск с буквой; «дискета»
1,44 МБ (2880 секторов) приводила к драйверу гибких дисков — буква A:, «Дискета».

Длинные имена: раньше диск нёс только короткие имена 8.3 (SETUP.CMD, READRU.TXT). Теперь
`make_disk_image.py` пишет ещё и записи длинных имён (LFN), поэтому на диске видны
`instagent.cmd` и `README-RU.txt` — те же имена, что в папке `agent-kit`. Короткие имена
(INSTAG~1.CMD и т.п.) остаются как запасной путь, если что-то длинное имя не прочитает.

Размер образа обязан совпадать с MSC_SECTORS в idf/main/msc.c и с разделом `msc`
в idf/partitions.csv — иначе прибор будет отдавать хосту не то, что лежит на диске.

    python tools/make_setup_disk.py
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import make_disk_image as mdi          # noqa: E402  (сборка FAT16 + побайтовая проверка)

ROOT = os.path.dirname(HERE)
KIT = os.path.join(ROOT, "agent-kit")
OUT = os.path.join(ROOT, "firmware", "media", "setup-disk-big.img")
SECTORS = 7552                         # = MSC_SECTORS в idf/main/msc.c (0x3B0000 / 512)

# Ровно эти четыре файла, под своими именами (как в agent-kit/)
WANTED = ["instagent.cmd", "deinstall.cmd", "README-RU.txt", "README-EN.txt"]


def main() -> int:
    files: list[tuple] = []
    for name in WANTED:
        path = os.path.join(KIT, name)
        if not os.path.exists(path):
            raise SystemExit(f"нет файла {path} — соберите папку: python tools/make_agent_kit.py")
        with open(path, "rb") as fh:
            files.append((name, fh.read(), os.path.getmtime(path)))

    img = mdi.build(SECTORS, files)
    print(f"диск: {len(img)} байт ({len(img) // 512} секторов = {len(img) / 1048576:.2f} МБ)")

    # проверка 1: побайтово сверяем то, что попало в образ, с файлами набора
    listed = dict(mdi.read_root_entries(img))
    ok = True
    for name, content, _ts in files:
        size = listed.get(name)
        back = mdi.read_file(img, name)
        if back is None:
            state = "ОШИБКА (файла в образе нет)"
        elif back != content or size != len(content):
            state = f"ОШИБКА (в образе {size} байт, читается {len(back)})"
        else:
            state = "OK (байт в байт)"
        if not state.startswith("OK"):
            ok = False
        print(f"  {name:>15}: {len(content):>6} байт — {state}")

    # проверка 2: на диске ровно эти файлы и ровно под этими именами
    print("  в каталоге образа: " + ", ".join(sorted(listed.keys())))
    extra = [n for n in listed if n not in WANTED]
    missing = [n for n in WANTED if n not in listed]
    if extra:
        print(f"  ОШИБКА: лишние файлы в образе: {', '.join(extra)}")
        ok = False
    if missing:
        print(f"  ОШИБКА: нет файлов в образе: {', '.join(missing)}")
        ok = False
    if len(listed) != len(WANTED):
        print(f"  ОШИБКА: в образе {len(listed)} файлов, ожидалось {len(WANTED)}")
        ok = False
    if not ok:
        raise SystemExit("образ собран неверно")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as fh:
        fh.write(img)
    print("записан", os.path.abspath(OUT))
    print("залить в раздел msc: python tools/flash_msc_image.py --port COMx "
          "(адрес берётся из idf/partitions.csv)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
