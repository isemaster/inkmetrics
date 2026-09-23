#!/usr/bin/env python
"""
Собрать релиз «прошивка из браузера»: страницу, манифест и один самодостаточный HTML.

Что делает:
  1. собирает объединённый образ прошивки flash-kit/idf/inkmetrics-fw-0x0.bin
     (загрузчик + таблица разделов + ota_data + приложение; пишется с адреса 0x0);
  2. пишет flash-web/manifest.js — имена файлов, адреса, размеры и sha256;
  3. собирает один файл inkmetrics-webflash.html с вшитыми образами (работает из папки,
     без сервера и без интернета) и кладёт его в архив inkmetrics-webflash-YYYY-MM-DD.zip.

Страница flash-web/index.html при этом одна и та же: она сама понимает, вшиты образы
(window.INKMETRICS_EMBEDDED) или их надо тянуть рядом лежащими файлами.

  python tools/make_web_flasher.py
"""
from __future__ import annotations

import base64
import datetime as dt
import hashlib
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KIT = ROOT / "flash-kit" / "idf"
WEB = ROOT / "flash-web"

# адреса берём из генератора комплекта, чтобы не разъезжаться с flash.bat
MSC_OFFSET = None
m = re.search(r"^MSC_OFFSET\s*=\s*(0x[0-9a-fA-F]+)", (ROOT / "tools" / "make_flash_kit.py").read_text(encoding="utf-8"), re.M)
if m:
    MSC_OFFSET = int(m.group(1), 16)
if MSC_OFFSET is None:
    MSC_OFFSET = 0x430000

MERGED = KIT / "inkmetrics-fw-0x0.bin"
DISK = KIT / "setup-disk-big.img"
SOURCES = ["bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "inkmetrics_idf.bin"]


def fw_version() -> str:
    txt = (ROOT / "idf" / "main" / "main.c").read_text(encoding="utf-8", errors="replace")
    m = re.search(r'FW_VERSION\s+"([^"]+)"', txt)
    return m.group(1) if m else "?"


def build_merged(python: str) -> None:
    """Объединённый образ: пишется одним файлом с 0x0, ошибиться адресом нельзя."""
    missing = [n for n in SOURCES if not (KIT / n).is_file()]
    if missing:
        raise SystemExit("нет файлов прошивки в комплекте: " + ", ".join(missing)
                         + "\nсначала: idf.py -C idf build && python tools/make_flash_kit.py")
    fresh = MERGED.is_file() and all(MERGED.stat().st_mtime >= (KIT / n).stat().st_mtime for n in SOURCES)
    if fresh and "--force" not in sys.argv:
        print("  объединённый образ   : уже свежий")
        return
    cmd = [python, "-m", "esptool", "--chip", "esp32s3", "merge-bin", "-o", str(MERGED),
           "0x0", str(KIT / SOURCES[0]), "0x8000", str(KIT / SOURCES[1]),
           "0xe000", str(KIT / SOURCES[2]), "0x20000", str(KIT / SOURCES[3])]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0 or not MERGED.is_file():
        raise SystemExit("merge-bin не сработал:\n" + (res.stdout + res.stderr)[-800:])
    print(f"  объединённый образ   : {MERGED.name} ({MERGED.stat().st_size} Б)")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write_manifest() -> dict:
    files = [
        {"name": MERGED.name, "label": "прошивка прибора (загрузчик, разделы, приложение)",
         "address": 0x0, "size": MERGED.stat().st_size, "sha256": sha256(MERGED)},
        {"name": DISK.name, "label": "диск прибора: скрипты агента и инструкции",
         "address": MSC_OFFSET, "size": DISK.stat().st_size, "sha256": sha256(DISK)},
    ]
    manifest = {
        "device": "inkmetrics",
        "fw": fw_version(),
        "built": dt.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "files": files,
    }
    body = ("/* Сгенерировано tools/make_web_flasher.py — правки затрутся. */\n"
            "export const MANIFEST = " + json.dumps(manifest, ensure_ascii=False, indent=2) + ";\n")
    (WEB / "manifest.js").write_text(body, encoding="utf-8", newline="\n")
    print(f"  манифест             : flash-web/manifest.js ({len(files)} файла, fw {manifest['fw']})")
    return manifest


def build_single_file(manifest: dict) -> Path:
    """Один HTML со вшитыми библиотекой и образами: работает из папки, без сервера."""
    html = (WEB / "index.html").read_text(encoding="utf-8")
    lib = base64.b64encode((WEB / "esptool-js.bundle.js").read_bytes()).decode("ascii")
    embedded = {
        "lib": lib,
        "manifest": manifest,
        "files": {f["name"]: base64.b64encode((KIT / f["name"]).read_bytes()).decode("ascii")
                  for f in manifest["files"]},
    }
    inject = ("<script>\n/* вшито tools/make_web_flasher.py: библиотека esptool-js и образы прошивки */\n"
              "window.INKMETRICS_EMBEDDED = " + json.dumps(embedded) + ";\n</script>\n")
    out = html.replace("</head>", inject + "</head>", 1)
    if "INKMETRICS_EMBEDDED" not in out:
        raise SystemExit("не нашёл </head> в flash-web/index.html — некуда вшивать образы")
    target = ROOT / "inkmetrics-webflash.html"
    target.write_text(out, encoding="utf-8", newline="\n")
    print(f"  один файл            : {target.name} ({target.stat().st_size / 1048576:.1f} МБ)")
    return target


def make_zip(single: Path, manifest: dict, day: str) -> Path:
    zip_path = ROOT / f"inkmetrics-webflash-{day}.zip"
    readme = (
        "inkmetrics — прошивка прибора из браузера\r\n"
        "========================================\r\n\r\n"
        "Что нужно: компьютер на Windows с Chrome или Edge и USB-кабель. Больше ничего —\r\n"
        "ни Python, ни esptool, ни ESP-IDF, ни Arduino IDE.\r\n\r\n"
        "Порядок\r\n"
        "-------\r\n"
        "1. Открыть файл inkmetrics-webflash.html (двойной клик, потом «Разрешить»).\r\n"
        "2. Перевести прибор в режим загрузчика: зажать кнопку BOOT, не отпуская подключить\r\n"
        "   USB, подержать ~2 секунды, отпустить.\r\n"
        "3. В браузере: «Проверить файлы» → «Подключить прибор» → «Прошить».\r\n\r\n"
        f"Внутри: прошивка {manifest['fw']} (адрес 0x0) и диск прибора (адрес 0x{manifest['files'][1]['address']:x}).\r\n"
        "Суммы файлов — на самой странице и в файле видны кнопкой «Проверить файлы».\r\n\r\n"
        "Если браузер не даёт выбрать порт — проверьте, что прибор в режиме загрузчика (порт\r\n"
        "появляется только там) и что кабель подключён напрямую, без хаба.\r\n"
    )
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(single, "inkmetrics-webflash.html")
        z.writestr("КАК-ПРОШИТЬ.txt", readme.encode("utf-8"))
    print(f"  архив                : {zip_path.name} ({zip_path.stat().st_size / 1048576:.1f} МБ)")
    return zip_path


def main() -> int:
    python = sys.executable
    print("сборка веб-прошивальщика:")
    if not (WEB / "index.html").is_file():
        raise SystemExit("нет flash-web/index.html")
    build_merged(python)
    manifest = write_manifest()
    single = build_single_file(manifest)
    day = dt.date.today().isoformat()
    make_zip(single, manifest, day)
    print("\nготово. Страница в репозитории: flash-web/index.html (файлы образов берутся из "
          "flash-kit/idf).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
