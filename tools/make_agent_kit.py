#!/usr/bin/env python
"""
Собирает папку `agent-kit/` — два .cmd файла, которые работают из любой папки:

    agent-kit\\instagent.cmd    установка агента (метрики + раздача интернета)
    agent-kit\\deinstall.cmd    удаление агента

Зачем так: набор для ПК должен быть **самодостаточным** — два файла, копируй куда угодно
(флешка, сеть, рабочий стол) и запускай. Поэтому код PowerShell лежит ВНУТРИ .cmd: файл при
запуске распаковывает свои же разделы (они помечены строкой-маркером после бат-части) в
папку установки и запускает установщик.

Источники (единственное место правки — они):

    tools/pc_setup/metrics_agent.ps1      агент метрик       -> внутри как agent.ps1
    tools/ics_enable.ps1                  раздача интернета  -> внутри как ics.ps1
    tools/pc_setup/agent_cmd_setup.ps1    регистрация задач  -> внутри как setup.ps1
    tools/pc_setup/agent_cmd_remove.ps1   удаление           -> внутри как remove.ps1

Правило проекта: .cmd и .ps1 — только ASCII (cmd.exe и PowerShell 5.1 ломают кириллицу),
поэтому сборщик отказывается работать, если в источнике есть не-ASCII символ.

    python tools/make_agent_kit.py            # собрать/обновить agent-kit/
    python tools/make_agent_kit.py --check    # только сверить (код 1 при расхождении)
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "agent-kit"

PS = ROOT / "tools" / "pc_setup"
TOOLS = ROOT / "tools"

# Что во что заворачивается: payload-имя -> исходник
INST_PAYLOADS = [
    ("agent.ps1", PS / "metrics_agent.ps1"),
    ("ics.ps1",   TOOLS / "ics_enable.ps1"),
    ("setup.ps1", PS / "agent_cmd_setup.ps1"),
]
DEINST_PAYLOADS = [
    ("ics.ps1",    TOOLS / "ics_enable.ps1"),
    ("remove.ps1", PS / "agent_cmd_remove.ps1"),
]

MARKER = "rem =====PAYLOAD:{name}====="

UNPACK = (
    'powershell -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText($env:SELF); '
    "$p=[regex]::Split($t,'rem =====PAYLOAD:([A-Za-z0-9._-]+)====='); "
    "for($i=1;$i -lt $p.Count;$i+=2){ $n=$p[$i]; $b=$p[$i+1].Replace([string][char]13,''); "
    "[IO.File]::WriteAllText((Join-Path $env:EINK_DIR $n),$b); "
    "Write-Host ('    '+$n+': '+$b.Length+' bytes') }\""
)

INST = r"""@echo off
rem ============================================================================
rem  instagent.cmd - install the inkmetrics agent on this PC.
rem
rem  The agent code travels INSIDE this file, so the file is self-contained:
rem  copy it anywhere and run it from any folder (double-click works too).
rem  Nothing else is needed - no Python, no ESP-IDF, no other scripts.
rem
rem  What it installs into C:\ProgramData\inkmetrics (change the folder with the
rem  environment variable EINK_INSTALL_DIR if you need to):
rem      agent.ps1  the metrics agent: CPU / memory / disk / ping -> the device, every minute
rem      ics.ps1    keeps internet sharing (ICS) on for the device adapter
rem      setup.ps1  registers both tasks and starts everything
rem  Tasks: "inkmetrics agent" and "inkmetrics ICS" - as SYSTEM, at startup, at
rem  logon and every minute.
rem
rem  Usage:   instagent.cmd              install (asks for administrator rights)
rem           instagent.cmd --dry-run    unpack into TEMP only, change nothing
rem  Removal: deinstall.cmd
rem
rem  ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .cmd as garbage.
rem  Below the batch part there are payload sections, each starting with a rem marker
rem  line; from the first marker on the file is DATA, not batch code, so the batch part
rem  must always end with exit /b and never fall through into the payload.
rem ============================================================================
setlocal EnableExtensions
set "SELF=%~f0"
set "DRY="
set "DIR="
if /i "%~1"=="--dry-run" set "DRY=1"

if defined EINK_INSTALL_DIR set "DIR=%EINK_INSTALL_DIR%"
if not defined DIR if defined DRY set "DIR=%TEMP%\inkmetrics-dry-run"
if not defined DIR set "DIR=%ProgramData%\inkmetrics"

echo.
echo   inkmetrics: agent installer
echo   install folder : %DIR%
if defined DRY echo   mode           : DRY RUN - files go to TEMP, nothing is registered
echo.

if not defined DRY (
    net session >nul 2>&1
    if errorlevel 1 (
        echo   Administrator rights are required - asking for them now.
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:SELF -Verb RunAs"
        exit /b 0
    )
)

if not exist "%DIR%" mkdir "%DIR%" 2>nul
if not exist "%DIR%" (
    echo   FAILED: cannot create "%DIR%".
    pause
    exit /b 1
)

echo   unpacking the agent code from this file...
set "EINK_DIR=%DIR%"
__UNPACK__
if errorlevel 1 (
    echo   FAILED: cannot unpack the payload from this file.
    pause
    exit /b 1
)
if not exist "%DIR%\agent.ps1" (
    echo   FAILED: agent.ps1 was not unpacked.
    pause
    exit /b 1
)
for %%A in ("%DIR%\agent.ps1") do if %%~zA LSS 2000 (
    echo   FAILED: agent.ps1 is empty - the unpack step did not work.
    pause
    exit /b 1
)

if defined DRY (
    echo.
    echo   DRY RUN: would stop an old agent, register the two tasks, turn the sharing on
    echo   and start the agent. Nothing was changed.
    echo   Unpacked files are in "%DIR%" - delete that folder by hand.
    exit /b 0
)

echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%DIR%\setup.ps1"
if errorlevel 1 (
    echo.
    echo   The installer reported a problem - see the lines above.
    pause
    exit /b 1
)

echo.
echo   Agent installed. On the device press PWR until page 5/5 HOST SYS: the CPU and
echo   memory numbers there must match this PC.
echo   Check this PC:   schtasks /query /tn "inkmetrics agent" /v /fo LIST
echo                    type "%DIR%\agent.log"
echo   Remove it again: deinstall.cmd
echo.
pause
exit /b 0
"""

DEINST = r"""@echo off
rem ============================================================================
rem  deinstall.cmd - remove the inkmetrics agent from this PC.
rem
rem  Self-contained as well: the removal script travels inside this file, so it
rem  works from any folder. It stops the agent, removes the tasks "inkmetrics
rem  agent" and "inkmetrics ICS", turns internet sharing off and deletes the
rem  install folder (C:\ProgramData\inkmetrics, or EINK_INSTALL_DIR if that is
rem  set). Nothing else on this PC is touched.
rem
rem  Usage:   deinstall.cmd              remove everything (asks for admin rights)
rem           deinstall.cmd --dry-run    only report what is installed
rem
rem  ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .cmd as garbage.
rem  Below the batch part there are payload sections, each starting with a rem marker
rem  line; from the first marker on the file is DATA, not batch code.
rem ============================================================================
setlocal EnableExtensions
set "SELF=%~f0"
set "DRY="
set "DIR="
if /i "%~1"=="--dry-run" set "DRY=1"
if defined EINK_INSTALL_DIR set "DIR=%EINK_INSTALL_DIR%"
if not defined DIR set "DIR=%ProgramData%\inkmetrics"

echo.
echo   inkmetrics: agent removal
echo   install folder : %DIR%
echo.

set "EINK_DIR=%TEMP%\inkmetrics-remove"
if not defined DRY (
    net session >nul 2>&1
    if errorlevel 1 (
        echo   Administrator rights are required - asking for them now.
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:SELF -Verb RunAs"
        exit /b 0
    )
)

if not exist "%EINK_DIR%" mkdir "%EINK_DIR%" 2>nul
echo   unpacking the removal script...
__UNPACK__
if errorlevel 1 (
    echo   FAILED: cannot unpack the payload from this file.
    pause
    exit /b 1
)

if defined DRY (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%EINK_DIR%\remove.ps1" -DryRun -InstallDir "%DIR%"
    echo.
    echo   DRY RUN: nothing was changed.
    exit /b 0
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%EINK_DIR%\remove.ps1" -InstallDir "%DIR%"

echo.
if exist "%DIR%" (
    echo   deleting "%DIR%" ...
    rd /s /q "%DIR%" 2>nul
)
if exist "%DIR%" (
    echo   WARN: could not delete "%DIR%" - look at it by hand
) else (
    echo   install folder removed.
)
rd /s /q "%EINK_DIR%" 2>nul

echo.
echo   Agent removed: tasks gone, sharing off, files deleted.
echo.
pause
exit /b 0
"""


def payload(name: str, src: Path) -> str:
    if not src.exists():
        raise SystemExit(f"нет исходника: {src}")
    text = src.read_text(encoding="utf-8")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    for i, ch in enumerate(text):
        if ord(ch) > 127:
            line = text[:i].count("\n") + 1
            raise SystemExit(f"{src} не ASCII: строка {line}, символ {ch!r} "
                             f"(правило проекта: .ps1/.cmd только латиница)")
    # Маркер идёт строкой, а перевод строки перед ним уже есть в шаблоне или в конце
    # предыдущего раздела: так распакованный файл совпадает с источником байт в байт.
    return MARKER.format(name=name) + text


def build_cmd(template: str, parts: list[tuple[str, Path]]) -> bytes:
    body = template.replace("__UNPACK__", UNPACK)
    if not body.endswith("\n"):
        body += "\n"
    for name, src in parts:
        if not body.endswith("\n"):
            body += "\n"
        body += payload(name, src)
    return body.replace("\n", "\r\n").encode("ascii")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="только сверить, ничего не писать")
    args = ap.parse_args()

    targets = [
        ("instagent.cmd", build_cmd(INST, INST_PAYLOADS)),
        ("deinstall.cmd", build_cmd(DEINST, DEINST_PAYLOADS)),
    ]
    changed: list[str] = []
    OUT.mkdir(parents=True, exist_ok=True)
    for name, data in targets:
        path = OUT / name
        old = path.read_bytes() if path.exists() else b""
        state = "совпадает" if old == data else "изменён"
        if old != data:
            changed.append(name)
            if not args.check:
                path.write_bytes(data)
        print(f"  {name:<15} {len(data):>6} Б  sha256 {hashlib.sha256(data).hexdigest()[:12]}  {state}")

    keep = {n for n, _ in targets}
    extra = [p for p in OUT.iterdir() if p.is_file() and p.name not in keep]
    for p in extra:
        changed.append(p.name + " (убран)")
        if not args.check:
            p.unlink()

    if args.check:
        print("расходится: " + (", ".join(changed) if changed else "—"))
        return 1 if changed else 0
    print(f"папка: {OUT}")
    print("готово, изменено: " + (", ".join(changed) if changed else "ничего (файлы уже совпадали)"))
    print("файлов внутри: " + str(len(list(OUT.iterdir()))) + " (должно быть 2)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
