#!/usr/bin/env python
"""
Один цикл отладки IDF-прошивки: залить → дать отработать → снять «чёрный ящик».

Зачем: у IDF-прошивки нет консоли (USB-CDC и USB-Serial/JTAG отключены — общий PHY
с TinyUSB, UART наружу не выведен), поэтому каждый цикл — это «залить, дать упасть,
прочитать раздел diag». Скрипт ведёт по шагам и сам называет, когда что зажать.

    python tools/idf_cycle.py                             # наша прошивка (idf/build)
    python tools/idf_cycle.py --build C:/.../eink-ncm/build  # A/B: стоковый пример IDF
    python tools/idf_cycle.py --wait 25                   # больше времени на отработку

Плата должна быть подключена по USB. На первом шаге: зажать BOOT, не отпуская
передёрнуть USB, подержать ~2 с, отпустить — появится порт ROM.
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_BUILD = ROOT / "idf" / "build"


def run(title: str, args: list[str]) -> int:
    print(f"\n=== {title} ===", flush=True)
    return subprocess.run(args).returncode


def main() -> int:
    build = str(DEFAULT_BUILD)
    if "--build" in sys.argv:
        build = sys.argv[sys.argv.index("--build") + 1]
    wait_s = 20
    if "--wait" in sys.argv:
        wait_s = int(sys.argv[sys.argv.index("--wait") + 1])

    print("ШАГ 1. Плата в режиме загрузчика: зажми BOOT, не отпуская передёрни USB,")
    print("        подержи ~2 с и отпусти. Скрипт сам поймает порт и зальёт прошивку.")
    print(f"        Образ: {build}")
    rc = run("заливка", [sys.executable, str(HERE / "try_flash.py"), build])
    if rc != 0:
        print("Заливка не удалась — повтори с BOOT.")
        return rc

    print(f"\nШАГ 2. Прошивка ушла в приложение. Даю {wait_s} с: она поднимет PHY,")
    print("        попробует TinyUSB и запишет «чёрный ящик».")
    for left in range(wait_s, 0, -5):
        print(f"        осталось ~{left} с…", flush=True)
        time.sleep(min(5, left))

    print("\nШАГ 3. Теперь ещё раз: зажми BOOT и передёрни USB — перевожу плату")
    print("        в загрузчик и читаю раздел diag.")
    return run("чтение «чёрного ящика»", [sys.executable, str(HERE / "idf_diag.py")])


if __name__ == "__main__":
    raise SystemExit(main())
