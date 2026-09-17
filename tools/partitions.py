#!/usr/bin/env python
"""
Раскладка флеша прибора — единственный источник истины `idf/partitions.csv`.

Зачем отдельным модулем: адрес и размер раздела `msc` (диск, который прибор отдаёт хосту)
были прописаны руками в трёх инструментах. Когда диск перестал быть «дискетой 1,44 МБ» и
раздел переехал с `0x670000` (0x170000) на `0x430000` (0x3B0000, место взято у второго слота
OTA), правки дошли не до всех: `tools/try_flash.py` и `tools/make_flash_kit.py` продолжали
писать старый маленький образ по старому адресу — то есть в середину нового раздела, с
затиранием половины диска. Теперь адрес и размер берутся из `partitions.csv` везде, и
разъехаться снова не могут.
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CSV = ROOT / "idf" / "partitions.csv"


def read_layout(csv_path: Path | str = CSV) -> dict[str, tuple[int, int]]:
    """{имя раздела: (адрес, размер)} из partitions.csv (адреса — байты)."""
    out: dict[str, tuple[int, int]] = {}
    for line in Path(csv_path).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 5:
            continue
        name, _type, _subtype, offset, size = parts[:5]
        try:
            out[name] = (int(offset, 16), int(size, 16))
        except ValueError:
            continue
    return out


def msc_partition(csv_path: Path | str = CSV) -> tuple[int, int]:
    """(адрес, размер) раздела `msc` — диска, который прибор отдаёт хосту."""
    layout = read_layout(csv_path)
    if "msc" not in layout:
        raise SystemExit(f"раздел 'msc' не найден в {csv_path}")
    return layout["msc"]


if __name__ == "__main__":
    for name, (off, size) in read_layout().items():
        print(f"{name:<9} 0x{off:06X}  0x{size:06X}  ({size / 1048576:.2f} МБ)")
