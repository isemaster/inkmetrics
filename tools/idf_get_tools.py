#!/usr/bin/env python
"""
Загрузка тулчейнов ESP-IDF через финский VPS.

Зачем так: github release-assets и зеркало dl.espressif.com отдают нам 0 байт
(DPI-блокировка), а VPS 84.21.191.180 качает и стримит файл по SSH.

Что делает:
  1. читает C:\\esp\\esp-idf\\tools\\tools.json (описание тулчейнов);
  2. качает нужные для esp32s3 архивы в C:\\esp\\dist (по 2 потока), проверяет sha256;
  3. пишет C:\\esp\\dist\\tools_local.json — тот же файл, но со ссылками file://,
     чтобы штатный установщик IDF разложил всё по местам без сети.

Запуск:
  python tools/idf_get_tools.py            # скачать и подготовить json
  python tools/idf_get_tools.py --check    # только проверить, что уже скачано
"""
from __future__ import annotations

import concurrent.futures as cf
import hashlib
import json
import os
import shutil
import subprocess
import sys

IDF = r"C:\esp\esp-idf"
DIST = r"C:\esp\dist"
KEY = r"D:\NewHerm\proxy_key"
VPS = "root@84.21.191.180"
NEEDED = [                      # что требует установщик IDF для esp32s3
    "xtensa-esp-elf",
    "xtensa-esp-elf-gdb",
    "riscv32-esp-elf",
    "riscv32-esp-elf-gdb",
    "esp32ulp-elf",
    "cmake",
    "ninja",
    "idf-exe",
    "ccache",
    "openocd-esp32",
    "dfu-util",
    "esp-rom-elfs",
]


def entry_url(ver: dict) -> dict | None:
    """Вернуть {url, size, sha256} для win64 или any."""
    for key in ("win64", "any", "linux-amd64", "linux-i686"):
        e = ver.get(key)
        if isinstance(e, dict) and e.get("url"):
            return e
        if isinstance(e, list) and e and isinstance(e[0], dict) and e[0].get("url"):
            return e[0]
    return None


def local_name(url: str) -> str:
    return url.rsplit("/", 1)[-1]


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def have(path: str, size: int | None, want_sha: str | None) -> bool:
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return False
    if size and os.path.getsize(path) != size:
        return False
    if want_sha:
        return sha256(path) == want_sha
    return True


def fetch(url: str, out: str) -> tuple[str, bool, str]:
    """Качаем через VPS: ssh 'curl -sL url' -> локальный файл."""
    tmp = out + ".part"
    cmd = f'ssh -i "{KEY}" -o BatchMode=yes -o ConnectTimeout=20 -o StrictHostKeyChecking=no ' \
          f'{VPS} "curl -fsSL \'{url}\'"'
    with open(tmp, "wb") as fh:
        p = subprocess.Popen(cmd, shell=True, stdout=fh, stderr=subprocess.PIPE)
        _, err = p.communicate()
    if p.returncode != 0:
        if os.path.exists(tmp):
            os.remove(tmp)
        return out, False, err.decode("utf-8", "replace").strip()[:200]
    shutil.move(tmp, out)
    return out, True, ""


def main() -> int:
    check_only = "--check" in sys.argv
    os.makedirs(DIST, exist_ok=True)
    tj = json.load(open(os.path.join(IDF, "tools", "tools.json"), encoding="utf-8"))

    jobs = []
    for tool in tj["tools"]:
        if tool["name"] not in NEEDED:
            continue
        ver = tool["versions"][0]
        e = entry_url(ver)
        if not e:
            print(f"[skip] {tool['name']}: нет архива для win64")
            continue
        out = os.path.join(DIST, local_name(e["url"]))
        jobs.append((tool["name"], e["url"], out, e.get("size"), e.get("sha256")))

    total = sum(j[3] or 0 for j in jobs)
    print(f"[tools] к загрузке {len(jobs)} архивов, {total/1e6:.0f} МБ")
    for name, _url, out, size, s in jobs:
        st = "есть" if have(out, size, s if not check_only else None) else "нужно"
        print(f"  {name:22s} {(size or 0)/1e6:8.1f} МБ  {st}")

    if check_only:
        return 0

    todo = [(name, url, out, size, s) for name, url, out, size, s in jobs
            if not have(out, size, s)]

    def run(job):
        name, url, out, size, s = job
        print(f"[tools] качаю {name} ({url.rsplit('/',1)[-1]})…", flush=True)
        out, ok, err = fetch(url, out)
        if not ok:
            return f"[ОШИБКА] {name}: {err}"
        if size and os.path.getsize(out) != size:
            return f"[ОШИБКА] {name}: размер {os.path.getsize(out)} вместо {size}"
        if s and not have(out, size, s):
            return f"[ОШИБКА] {name}: sha256 не совпал"
        print(f"[tools] готово {name}", flush=True)
        return f"[ok] {name}"

    if todo:
        with cf.ThreadPoolExecutor(max_workers=2) as pool:
            for res in pool.map(run, todo):
                print(res, flush=True)

    # локальный tools.json со ссылками file://
    for tool in tj["tools"]:
        for ver in tool["versions"]:
            for key in ("win64", "any"):
                e = ver.get(key)
                if isinstance(e, dict) and e.get("url"):
                    cand = os.path.join(DIST, local_name(e["url"]))
                    if os.path.exists(cand):
                        e["url"] = "file:///" + cand.replace("\\", "/")
    local_json = os.path.join(DIST, "tools_local.json")
    json.dump(tj, open(local_json, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"[tools] записан {local_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
