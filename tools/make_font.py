#!/usr/bin/env python
"""
Генератор растровых шрифтов для панели прибора (200x200, 1 бит на пиксель).

Зачем: в IDF-прошивке кадр рисует сам прибор, а шрифтов у него нет. Тянуть сюда
LVGL ради двух строк текста — лишние сотни килобайт; поэтому глифы, которые реально
нужны (цифры, латиница для имён хостов и IP, **кириллица** для подписей), рендерятся
один раз из системного TTF и кладутся в `idf/main/fonts.h` таблицей бит.

    python tools/make_font.py            # перегенерировать idf/main/fonts.h

Шрифты:
  * `font_small` — 6x11, Consolas: подписи и строки данных (IP, адрес, статус);
  * `font_big`   — 12x22, Consolas Bold: только заглавные и цифры — крупная надпись
                   «В СЕТИ» / «НЕТ СЕТИ», читаемая с пары метров.

Формат: для каждого символа — битовая строка по строкам (слева направо, старший бит
первым). В C рисуется так: бит i = байт i/8, маска 1 << (7 - i%8).
"""
from __future__ import annotations

import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit("нужен Pillow: C:/Python314/python.exe -m pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "idf" / "main" / "fonts.h"

FONTS = {
    # имя, файл TTF, ширина, высота, размер шрифта, набор символов
    "font_small": ("C:/Windows/Fonts/consola.ttf", 6, 11, 9, "small"),
    "font_big": ("C:/Windows/Fonts/consolab.ttf", 12, 22, 20, "big"),
}


def charset(kind: str) -> list[int]:
    """Какие символы кладём в шрифт."""
    cps = [0x20, 0xB0, 0xB7, 0x2116, 0x2190, 0x2191, 0x2192, 0x2193]  # пробел, °, ·, №, стрелки
    cps += list(range(0x21, 0x7F))                                    # вся ASCII-графика
    cps += [0x401, 0x451]                                             # Ё, ё
    cps += list(range(0x410, 0x450))                                  # А..я
    if kind == "big":
        # крупный шрифт — только то, чем пишем статус: цифры, заглавные, знаки
        keep = set(range(0x30, 0x3A)) | set(range(0x41, 0x5B)) | set(range(0x410, 0x430)) \
            | {0x20, 0x2E, 0x2C, 0x3A, 0x2D, 0x2F, 0x25, 0xB0, 0x2191, 0x2193, 0x401}
        cps = [c for c in cps if c in keep]
    return sorted(set(cps))


def render_font(ttf: str, w: int, h: int, size: int, kind: str):
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    baseline = h - descent if ascent + descent <= h else h - max(0, descent)
    # если строка выше ячейки — поднимаем базовую линию чуть вверх
    if ascent + descent > h:
        baseline = h - descent + (ascent + descent - h)
    glyphs = []
    for cp in charset(kind):
        img = Image.new("1", (w, h), 0)
        d = ImageDraw.Draw(img)
        d.text((0, baseline), chr(cp), font=font, fill=1, anchor="ls")
        bits = []
        for y in range(h):
            for x in range(w):
                bits.append(1 if img.getpixel((x, y)) else 0)
        by = bytearray((w * h + 7) // 8)
        for i, b in enumerate(bits):
            if b:
                by[i // 8] |= 1 << (7 - (i % 8))
        glyphs.append((cp, bytes(by)))
    return glyphs, ascent + descent, baseline


def preview(glyphs: list[tuple[int, bytes]], w: int, h: int, text: str) -> str:
    """ASCII-превью строки — проверяем, что символы не обрезаны и кириллица есть."""
    table = {cp: bits for cp, bits in glyphs}
    out = []
    for y in range(h):
        row = ""
        for ch in text:
            bits = table.get(ord(ch))
            if bits is None:
                row += " " * w
                continue
            for x in range(w):
                i = y * w + x
                row += "#" if bits[i // 8] & (1 << (7 - (i % 8))) else "."
        out.append(row)
    return "\n".join(out)


def main() -> int:
    parts = []
    for name, (ttf, w, h, size, kind) in FONTS.items():
        if not Path(ttf).exists():
            raise SystemExit(f"нет файла шрифта {ttf}")
        glyphs, line_h, baseline = render_font(ttf, w, h, size, kind)
        nbytes = (w * h + 7) // 8
        parts.append((name, w, h, nbytes, glyphs))
        print(f"{name}: {w}x{h}, {len(glyphs)} символов, {nbytes} Б на глиф, "
              f"высота строки {line_h}, базовая линия {baseline}")

    with OUT.open("w", encoding="utf-8", newline="\n") as f:
        f.write("/* Сгенерировано tools/make_font.py — не править руками.\n"
                " * Растровые шрифты для панели 200x200: биты по строкам, старший бит первым. */\n"
                "#pragma once\n\n#include <stdint.h>\n\n")
        for name, w, h, nbytes, glyphs in parts:
            up = name.upper()
            f.write(f"#define {up}_W {w}\n#define {up}_H {h}\n#define {up}_BYTES {nbytes}\n\n")
            f.write(f"typedef struct {{ uint16_t cp; uint8_t bits[{up}_BYTES]; }} {name}_glyph_t;\n\n")
            f.write(f"static const {name}_glyph_t {name}[] = {{\n")
            for cp, bits in glyphs:
                body = ", ".join(f"0x{b:02x}" for b in bits)
                f.write(f"    {{ 0x{cp:04x}, {{ {body} }} }},\n")
            f.write("};\n\n")
            f.write(f"#define {up}_COUNT {len(glyphs)}\n\n")
    print("записано:", OUT)

    # самопроверка: печатаем превью строки
    for name, w, h, nbytes, glyphs in parts:
        sample = "192.168.7.1 · В СЕТИ · t°C" if name == "font_small" else "В СЕТИ"
        print(f"\n--- превью {name}: «{sample}» ---")
        print(preview(glyphs, w, h, sample))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
