#!/usr/bin/env python
"""
Генератор растровых шрифтов для панели прибора (200x200, 1 бит на пиксель).

Зачем: в IDF-прошивке кадр рисует сам прибор, а шрифтов у него нет. Тянуть сюда
LVGL ради двух строк текста — лишние сотни килобайт; поэтому глифы, которые реально
нужны (цифры, латиница для имён хостов и IP, **кириллица** для подписей), рендерятся
один раз из системного TTF и кладутся в `idf/main/fonts.h` таблицей бит.

    python tools/make_font.py            # перегенерировать idf/main/fonts.h

Шрифты (четыре, по макету v5 — два экрана: сводный и SETUP):
  * `font_small` — 6x11,  Consolas:      подписи, адреса, нижние строки;
  * `font_mid`   — 18x30,  Consolas Bold: проценты CPU/RAM/DISK;
  * `font_big`   — 12x22,  Consolas Bold: заголовок ONLINE/SETUP, аптайм, значения строк;
  * `font_huge`  — 36x44,  Consolas Bold: температуры GPU — самое крупное на экране.

Про клетку и чернила
--------------------
Клетка (`*_W x *_H`) — это шаг курсора, а не размер текста: у Consolas внутри клетки
всегда остаются пустые строки сверху и снизу. Чтобы раскладка экрана считалась в
пикселях, генератор измеряет **чернила** (bbox реально закрашенных пикселей) и пишет
в fonts.h два числа: `*_INK_TOP` — на сколько строк ниже верха клетки начинаются
чернила, и `*_INK_H` — их высота. Отсюда правило прошивки: `y` в функции вывода —
это верх ЧЕРНИЛ, а не верха клетки (см. display.cpp).

Ширина знака: у Consolas шаг ≈ 0.6 от кегля, поэтому ширина клетки задана под кегль
(36 px при кегле 58 = 35.2 px на знак). Кегль температуры выбран по ширине экрана:
две цифры плюс «°» дают 84 px, две карты — 168 px из 200, между числами остаётся
24 px. При клетке 40 px (кегль 64) числа сходились вплотную и читались как одно.

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

# имя, файл TTF, ширина клетки, высота клетки, кегль, набор символов
FONTS = {
    "font_small": ("C:/Windows/Fonts/consola.ttf",  6, 11,  9, "small"),
    "font_mid":   ("C:/Windows/Fonts/consolab.ttf", 18, 30, 28, "mid"),
    "font_big":   ("C:/Windows/Fonts/consolab.ttf", 12, 22, 20, "big"),
    "font_huge":  ("C:/Windows/Fonts/consolab.ttf", 36, 44, 58, "huge"),
    # строка состояния: слово в 1.5 раза крупнее big (чернила 21 против 14),
    # расшифровка — в 1.5 раза крупнее small (чернила 9 против 6); без рамки
    # вокруг пинга места хватает: PING 72 px + « - 15MS - 4/4» 117 px = 189 из 200
    "font_ping":  ("C:/Windows/Fonts/consolab.ttf", 18, 32, 31, "big"),
    "font_txt9":  ("C:/Windows/Fonts/consolab.ttf",  9, 16, 13, "big"),
}

# строка, по которой измеряем чернила шрифта (есть и цифры, и буквы, и °)
INK_SAMPLE = "0123ABC°"

# что кладём в шрифт по наборам
CYR_UPPER = list(range(0x410, 0x430))          # А..Я
PUNCT = [0x20, 0x21, 0x25, 0x28, 0x29, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
         0x3A, 0x3D, 0x3F, 0xB0, 0xB7]         # пробел ! % ( ) + , - . / : = ? ° ·
ARROWS = [0x2116, 0x2190, 0x2191, 0x2192, 0x2193]


def charset(kind: str) -> list[int]:
    """Какие символы кладём в шрифт (набор зависит от кегля — см. заголовок)."""
    if kind == "small":
        cps = PUNCT + ARROWS + list(range(0x21, 0x7F)) + [0x401, 0x451] + CYR_UPPER \
            + list(range(0x430, 0x450))
        return sorted(set(cps))
    if kind == "big":
        # крупный шрифт — статус и значения строк: цифры, заглавные, знаки
        keep = set(range(0x30, 0x3A)) | set(range(0x41, 0x5B)) | set(CYR_UPPER) \
            | {0x20, 0x2E, 0x2C, 0x3A, 0x2D, 0x2F, 0x25, 0xB0, 0x2191, 0x2193, 0x401}
        return sorted(set(cps for cps in (PUNCT + ARROWS + list(range(0x21, 0x7F))
                                          + [0x401] + CYR_UPPER) if cps in keep))
    if kind == "mid":
        # проценты: только цифры и знаки, кириллица не нужна
        keep = set(range(0x30, 0x3A)) | set(range(0x41, 0x5B)) \
            | {0x20, 0x25, 0xB0, 0x2D, 0x2E, 0x2F, 0x2B, 0x3A}
        return sorted(set(cps for cps in (PUNCT + list(range(0x21, 0x7F))) if cps in keep))
    # huge — температуры: две цифры и °, остальное на случай «--»
    return sorted({0x20, 0x25, 0xB0, 0x2D, 0x2E, 0x2F, 0x4E, 0x41}
                  | set(range(0x30, 0x3A)))


def render_font(ttf: str, w: int, h: int, size: int, kind: str):
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    baseline = h - descent if ascent + descent <= h else h - max(0, descent)
    # если строка выше клетки — поднимаем базовую линию чуть вверх
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

    # чернила шрифта: где реально начинается и сколько занимает текст
    ink_top, ink_h = ink_metrics(font, w, h, baseline)
    return glyphs, ascent + descent, baseline, ink_top, ink_h


def ink_metrics(font, w: int, h: int, baseline: int) -> tuple[int, int]:
    """Верх и высота чернил по контрольной строке INK_SAMPLE (одна клетка на знак)."""
    img = Image.new("1", (len(INK_SAMPLE) * w, h), 0)
    d = ImageDraw.Draw(img)
    for i, ch in enumerate(INK_SAMPLE):
        d.text((i * w, baseline), ch, font=font, fill=1, anchor="ls")
    bbox = img.getbbox()
    if not bbox:
        return 0, h
    return bbox[1], bbox[3] - bbox[1]


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
        glyphs, line_h, baseline, ink_top, ink_h = render_font(ttf, w, h, size, kind)
        nbytes = (w * h + 7) // 8
        parts.append((name, w, h, nbytes, glyphs, ink_top, ink_h))
        print(f"{name}: клетка {w}x{h}, кегль {size}, {len(glyphs)} символов, "
              f"{nbytes} Б на глиф, ~{len(glyphs) * nbytes // 1024} КБ, "
              f"чернила: верх {ink_top}, высота {ink_h}")

    with OUT.open("w", encoding="utf-8", newline="\n") as f:
        f.write("/* Сгенерировано tools/make_font.py — не править руками.\n"
                " * Растровые шрифты для панели 200x200: биты по строкам, старший бит первым.\n"
                " * INK_TOP/INK_H — где внутри клетки лежат чернила: y в display_text_f()\n"
                " * задаёт верх ЧЕРНИЛ (см. display.cpp). */\n"
                "#pragma once\n\n#include <stdint.h>\n\n")
        for name, w, h, nbytes, glyphs, ink_top, ink_h in parts:
            up = name.upper()
            f.write(f"#define {up}_COUNT {len(glyphs)}\n"
                    f"#define {up}_W {w}\n#define {up}_H {h}\n#define {up}_BYTES {nbytes}\n"
                    f"#define {up}_INK_TOP {ink_top}\n#define {up}_INK_H {ink_h}\n\n")
            # коды символов отдельной таблицей, биты — плоским массивом: это снимает
            # вопрос выравнивания структур и делает двоичный поиск в C двумя строками
            cps = ", ".join(f"0x{cp:04x}" for cp, _ in glyphs)
            f.write(f"static const uint16_t {name}_cp[{up}_COUNT] = {{ {cps} }};\n\n")
            f.write(f"static const uint8_t {name}_bits[{up}_COUNT][{up}_BYTES] = {{\n")
            for _, bits in glyphs:
                body = ", ".join(f"0x{b:02x}" for b in bits)
                f.write(f"    {{ {body} }},\n")
            f.write("};\n\n")
    print("записано:", OUT)

    # самопроверка: печатаем превью строки
    for name, w, h, nbytes, glyphs, ink_top, ink_h in parts:
        sample = {"font_small": "192.168.7.1 · В СЕТИ · t°C",
                  "font_mid": "37%",
                  "font_big": "НЕТ СВЯЗИ",
                  "font_huge": "78°",
             "font_ping": "PING - 15MS - 4/4",
             "font_txt9": " - 999MS - 4/4"}[name]
        print(f"\n--- превью {name}: «{sample}» ---")
        print(preview(glyphs, w, h, sample))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
