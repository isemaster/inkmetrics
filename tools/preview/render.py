#!/usr/bin/env python
"""
Сборка листа предпросмотра из кадров стенда (tools/preview/*.pgm).

Кадры рисует НАСТОЯЩИЙ screen.c (см. tools/preview/build.sh) — здесь только проверки и
склейка: измеряем, сколько строк занято, где границы блоков, самый большой промежуток,
не выходит ли что-то за 200 px, и собираем PNG ×3 для просмотра.

    bash tools/preview/build.sh                 # отрисовать кадры (нужен WSL/gcc)
    python tools/preview/render.py               # лист + отчёт
"""
from __future__ import annotations

import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit("нужен Pillow: C:/Python314/python.exe -m pip install pillow")

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT_PNG = ROOT / "docs" / "screens-v7.png"   # макет текущей ревизии экранов
W = H = 200
SCALE = 3

FRAMES = [
    ("summary.pgm", "PING - 15MS - 4/4 — состояние хоста, крупно CPU % и вторая карта"),
    ("temps.pgm", "в крупных числах температуры карт (78° 79°)"),
    ("wide.pgm", "полная загрузка 100 %: три знака — средний кегль"),
    ("setup.pgm", "SETUP — строка SHOW с выбранной парой"),
    ("offline.pgm", "OFFLINE - 0/4 — интернета у хоста нет, числа живые"),
    ("nodata.pgm", "NO DATA — агент молчит"),
]


def load_pgm(path: Path) -> Image.Image:
    """P2 (ASCII PGM): 0 — чернила, 255 — бумага."""
    tokens = path.read_text().split()
    if tokens[0] != "P2":
        raise SystemExit(f"{path}: ожидался P2")
    w, h, _max = int(tokens[1]), int(tokens[2]), int(tokens[3])
    vals = [int(v) for v in tokens[4:]]
    img = Image.new("L", (w, h), 255)
    img.putdata(vals[:w * h])
    return img


def band_groups(img: Image.Image, y0: int, y1: int):
    """Группы чернил по горизонтали в полосе строк и самый узкий промежуток между ними.

    Нужно для числовых полос: два числа, слипшиеся в одно, выглядят на экране как одно
    число, и по строкам это не видно — только по колонкам."""
    px = img.load()
    used = [x for x in range(W) if any(px[x, y] < 128 for y in range(y0, y1 + 1))]
    if not used:
        return [], None
    groups, start, prev = [], used[0], used[0]
    for x in used[1:]:
        if x != prev + 1:
            groups.append((start, prev))
            start = x
        prev = x
    groups.append((start, prev))
    gaps = [groups[i + 1][0] - groups[i][1] - 1 for i in range(len(groups) - 1)]
    return groups, (min(gaps) if gaps else None)


def report(name: str, img: Image.Image) -> None:
    px = img.load()
    rows = [y for y in range(H) if any(px[x, y] < 128 for x in range(W))]
    cols = [x for x in range(W) if any(px[x, y] < 128 for y in range(H))]

    # промежутки между блоками (по пустым строкам)
    blanks, start = [], None
    for y in range(H):
        busy = y in set(rows)
        if not busy and start is None:
            start = y
        if busy and start is not None:
            blanks.append((start, y - start))
            start = None
    if start is not None:
        blanks.append((start, H - start))
    inner = [g for g in blanks if g[0] > min(rows) and g[0] + g[1] < max(rows)]
    big = max(inner, key=lambda g: g[1]) if inner else (0, 0)

    # блоки: группы подряд идущих занятых строк
    blocks, start = [], None
    row_set = set(rows)
    for y in range(H):
        if y in row_set and start is None:
            start = y
        elif y not in row_set and start is not None:
            blocks.append((start, y - 1))
            start = None
    if start is not None:
        blocks.append((start, H - 1))

    print(f"\n{name}")
    print(f"  чернила: строки {min(rows)}..{max(rows)} (занято {len(rows)} из {H}), "
          f"колонки {min(cols)}..{max(cols)}")
    print(f"  блоков {len(blocks)}: " + ", ".join(f"{a}..{b}" for a, b in blocks))
    print(f"  самый большой промежуток внутри: {big[1]} px (на строке {big[0]})")

    # числовые полосы: где именно стоит чернила по горизонтали и не слиплось ли
    for a, b in blocks:
        if 10 <= b - a + 1 <= 45:
            groups, gap = band_groups(img, a, b)
            if not groups:
                continue
            where = ", ".join(f"{g0}..{g1}" for g0, g1 in groups)
            tail = "слиплось!" if gap is not None and gap < 2 else f"{gap} px"
            print(f"  полоса {a}..{b}: группы {where} · узкий промежуток {tail}")
    if max(cols) > W - 1 or max(rows) > H - 1:
        raise SystemExit(f"  ОШИБКА: содержимое выходит за {W}x{H}")


def main() -> int:
    imgs = []
    for fname, title in FRAMES:
        path = HERE / fname
        if not path.exists():
            raise SystemExit(f"нет кадра {path} — сначала bash tools/preview/build.sh")
        img = load_pgm(path)
        if img.size != (W, H):
            raise SystemExit(f"{fname}: размер {img.size}, ожидался {W}x{H}")
        report(title, img)
        imgs.append((title, img))

    gap = 24
    sheet = Image.new("L", (len(imgs) * W * SCALE + (len(imgs) + 1) * gap,
                            H * SCALE + 3 * gap + 26), 255)
    sd = ImageDraw.Draw(sheet)
    label = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 18)
    x = gap
    for title, img in imgs:
        sheet.paste(img.resize((W * SCALE, H * SCALE), Image.NEAREST), (x, 2 * gap + 26))
        sd.rectangle([x - 1, 2 * gap + 25, x + W * SCALE, 2 * gap + 26 + H * SCALE], outline=0)
        sd.text((x, gap + 4), title, font=label, fill=0)
        x += W * SCALE + gap
    OUT_PNG.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(OUT_PNG)
    print(f"\nлист: {sheet.size} -> {OUT_PNG}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
