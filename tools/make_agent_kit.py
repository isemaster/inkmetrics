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

DOC_RU = """ИНСТРУКЦИЯ: установка агента inkmetrics на компьютер

Что это
-------
Два файла командной строки, которые ставят и снимают агента на любом компьютере
с Windows 10 или 11:

    instagent.cmd   установка агента (метрики + раздача интернета прибору)
    deinstall.cmd   удаление агента

Код агента лежит ВНУТРИ этих файлов: рядом больше ничего не нужно, других файлов
и установленных программ (Python, ESP-IDF, драйверов) они не требуют. Можно
запускать из любой папки - с флешки, из сетевой папки, с рабочего стола.

Что именно ставится
-------------------
    папка установки : C:\\ProgramData\\inkmetrics
    файлы           : agent.ps1   агент метрик (CPU, память, диск, пинг -> прибор)
                      ics.ps1     раздача интернета прибору (ICS)
                      setup.ps1   служебный, ставит задачи и запускает всё
                      agent.log   журнал агента (создаёт сам агент)
                      agent.pid   файл с номером процесса агента
                      cpu.state   предыдущий замер CPU (чтобы считать проценты)
    задачи          : "inkmetrics agent" - раз в минуту, от имени SYSTEM
                      "inkmetrics ICS"   - при старте, при входе и раз в минуту
    ничего в реестре, в автозагрузке или в службах не прописывается; сторонних
    программ не устанавливается.

Установка (5 шагов)
-------------------
1. Подключить прибор к компьютеру USB-кабелем (напрямую в порт, без хаба).
2. Запустить instagent.cmd двойным кликом. Появится запрос прав администратора -
   согласиться ("Да"). Права нужны один раз, для регистрации задач.
3. Дождаться строк "task ... registered" и "Agent installed". Окно само держится
   открытым, прочитать его можно спокойно.
4. Проверить на приборе: нажимать кнопку PWR, пока не появится страница HOST SYS
   (5/5). Там CPU, память, диск и возраст данных - числа должны совпадать с этим
   компьютером, возраст - десятки секунд.
5. Проверить на компьютере (любая из проверок):

       schtasks /query /tn "inkmetrics agent" /v /fo LIST
       type C:\\ProgramData\\inkmetrics\\agent.log

   В журнале раз в минуту должны появляться строки "sent ok (cpu=..%)".

Что агент делает и куда шлёт
----------------------------
Раз в 60 секунд агент собирает метрики этого компьютера и отправляет их прибору
запросом POST http://192.168.7.1/ingest. Прибор на странице HOST SYS показывает
их и считает возраст: если данные старше 3 минут, помечает как STALE (устарело).

Проверить канал руками можно так (из окна PowerShell):

    curl.exe -s -X POST -H "Content-Type: application/json" -d "{\\"cpu_percent\\":50.0}" http://192.168.7.1/ingest

Ответ "ok" и цифра 50 на экране прибора означают, что канал до прибора рабочий,
и дело только в агенте.

Раздача интернета (ICS) - что нужно знать
-----------------------------------------
Вместе с агентом включается раздача интернета (Internet Connection Sharing) для
адаптера прибора: тогда прибор и всё, что за ним, выходят в интернет через этот
компьютер. Windows часто сбрасывает раздачу (смена сети, перезагрузка), поэтому
её каждую минуту поднимает вторая задача, "inkmetrics ICS".

ВАЖНО, известное ограничение: когда раздача включена, адаптер прибора получает
адрес 192.168.137.1, прибор переезжает в подсеть 192.168.137.x, а агент метрик
по-прежнему отправляет данные на 192.168.7.1 - метрики в этом режиме на прибор
НЕ приходят (у прибора при этом интернет есть). Пока это ограничение не снято,
выбирайте одно из двух:

    * нужны метрики      - выключить раздачу: ICS.PS1 -Off (скрипт из набора
                           pc-setup) или в свойствах адаптера прибора снять
                           "Разрешить другим пользователям...";
    * нужен интернет     - оставить раздачу включённой, но метрик не ждать.

Удаление
--------
Запустить deinstall.cmd двойным кликом (тоже спросит права администратора).
Он останавливает агент, снимает обе задачи, выключает раздачу и удаляет папку
C:\\ProgramData\\inkmetrics. Больше ничего на компьютере не трогается: ярлыки,
реестр, службы и другие сетевые настройки остаются как были.

Ключи и переменные
------------------
    instagent.cmd --dry-run    ничего не менять: распаковать файлы во временную
                               папку и показать, что было бы сделано
    deinstall.cmd --dry-run    только показать состояние: задачи, процесс, папка
    EINK_INSTALL_DIR=путь      поставить агента в другую папку вместо
                               C:\\ProgramData\\inkmetrics (например для проверки)

Порядок для проверки без последствий: instagent.cmd --dry-run, затем
deinstall.cmd --dry-run.

Если метрик нет - что смотреть по порядку
-----------------------------------------
1. Задачи: schtasks /query /tn "inkmetrics agent" - если задачи нет, установка
   не дошла до конца, запустите instagent.cmd заново (с правами администратора).
2. Журнал: type C:\\ProgramData\\inkmetrics\\agent.log
       "agent started ... sent ok"        - всё работает;
       "send error: время ожидания ..."   - агент жив, но прибор недоступен:
                                            проверьте, подключён ли прибор и его
                                            адрес (строка ADDR на экране);
       файла нет                         - агент не запускается (см. пункт 1).
3. Файл агента: type C:\\ProgramData\\inkmetrics\\agent.ps1 - первая строка
   должна быть "agent.ps1 - host monitoring agent". Если там текст про
   установку раздачи, значит агент затёрт старым набором: удалите
   (deinstall.cmd) и поставьте заново этим файлом.
4. Прибор: страница http://192.168.7.1/api/state, поле ingest (count и age).
   count должен расти каждую минуту, age - секунды.
5. Прошивка прибора: метрики принимает только 0.4.0 и новее (страница HOST SYS
   и запрос /ingest). Версия видна на странице прибора и на его экране.

Требования и ограничения
------------------------
* Windows 10 или 11, права администратора на время установки.
* Прибор подключён по USB; прошивка прибора 0.4.0 или новее.
* Скрипты написаны латиницей намеренно: cmd.exe и PowerShell 5.1 при системной
  кодировке портят русский текст в .cmd/.ps1, поэтому сообщения английские,
  а подробности - в этом файле.
* macOS без сторонних драйверов сеть прибора не поднимет; на Linux скрипты
  Windows-only. Это инструмент для Windows.
* Агент знает адрес прибора 192.168.7.1. Если прибор получил другой адрес
  (например 192.168.137.x при включённой раздаче), метрики не дойдут - см.
  раздел про ICS выше.

Файлы рядом
-----------
В этой папке лежат только эти два .cmd и две инструкции (русская и английская).
Более полный набор для настройки компьютера - в папке pc-setup проекта
(там же CHECK.CMD - подробная проверка агента, прибора и сети, и FIXDISK.PS1 -
ремонт, если диск прибора не появился в Windows).
"""

DOC_EN = """INSTRUCTIONS: installing the inkmetrics agent on a PC

What this is
------------
Two command files that install and remove the agent on any Windows 10/11 PC:

    instagent.cmd   install the agent (metrics + internet sharing for the device)
    deinstall.cmd   remove the agent

The agent code travels INSIDE these files: nothing else is needed next to them,
no Python, no ESP-IDF, no drivers. They can be run from any folder - a USB stick,
a network share, the desktop.

What gets installed
-------------------
    install folder : C:\\ProgramData\\inkmetrics
    files          : agent.ps1   the metrics agent (CPU, memory, disk, ping -> device)
                     ics.ps1     internet sharing (ICS) for the device
                     setup.ps1   helper: registers the tasks and starts everything
                     agent.log   agent journal (written by the agent itself)
                     agent.pid   the agent process id
                     cpu.state   previous CPU sample (needed to compute percents)
    tasks          : "inkmetrics agent" - every minute, as SYSTEM
                     "inkmetrics ICS"   - at startup, at logon, every minute
    Nothing is added to the registry, to startup or to services; no third-party
    software is installed.

Installation (5 steps)
----------------------
1. Connect the device to the PC with a USB cable (directly to a port, no hub).
2. Start instagent.cmd by double click. Windows asks for administrator rights -
   accept ("Yes"). The rights are needed once, to register the tasks.
3. Wait for the lines "task ... registered" and "Agent installed". The window
   stays open, so you can read it calmly.
4. Check the device: press PWR until page HOST SYS (5/5). It shows CPU, memory,
   disk and the age of the data - the numbers must match this PC and the age must
   be tens of seconds.
5. Check on the PC (either):

       schtasks /query /tn "inkmetrics agent" /v /fo LIST
       type C:\\ProgramData\\inkmetrics\\agent.log

   The journal must get "sent ok (cpu=..%)" lines once a minute.

What the agent does
-------------------
Every 60 seconds the agent collects this PC's metrics and sends them to the device
with POST http://192.168.7.1/ingest. The device shows them on the HOST SYS page and
tracks their age: data older than 3 minutes is marked STALE.

Manual channel test (from a PowerShell window):

    curl.exe -s -X POST -H "Content-Type: application/json" -d "{\\"cpu_percent\\":50.0}" http://192.168.7.1/ingest

An "ok" answer plus 50 on the device screen means the channel works and only the
agent is in question.

Internet sharing (ICS) - what you must know
-------------------------------------------
Together with the agent, internet sharing is enabled for the device adapter: the
device (and anything behind it) goes online through this PC. Windows drops sharing
often (network change, reboot), so a second task "inkmetrics ICS" brings it back
every minute.

IMPORTANT, known limitation: while sharing is on, the device adapter becomes
192.168.137.1 and the device moves into the 192.168.137.x subnet, while the metrics
agent still posts to 192.168.7.1 - in that mode metrics do NOT arrive (the device
does have internet). Until this is fixed, pick one:

    * you want metrics   - turn sharing off: ICS.PS1 -Off (from the pc-setup
                           folder), or clear "Allow other users..." in the
                           properties of the device adapter;
    * you want internet  - keep sharing on and do not expect metrics.

Removal
-------
Run deinstall.cmd by double click (it asks for administrator rights too). It stops
the agent, removes both tasks, turns sharing off and deletes
C:\\ProgramData\\inkmetrics. Nothing else on the PC is touched: shortcuts, the
registry, services and other network settings stay as they were.

Switches and variables
----------------------
    instagent.cmd --dry-run    change nothing: unpack into a temporary folder and
                               show what would be done
    deinstall.cmd --dry-run    only report the state: tasks, process, folder
    EINK_INSTALL_DIR=path      install into another folder instead of
                               C:\\ProgramData\\inkmetrics (used for testing)

A safe way to look around: instagent.cmd --dry-run, then deinstall.cmd --dry-run.

No metrics - what to check, in order
------------------------------------
1. Tasks: schtasks /query /tn "inkmetrics agent" - if the task is missing, the
   installation did not finish; run instagent.cmd again (as administrator).
2. Journal: type C:\\ProgramData\\inkmetrics\\agent.log
       "agent started ... sent ok"      - all good;
       "send error: ..."                - the agent runs but cannot reach the
                                          device: check the cable and the device
                                          address (ADDR line on its screen);
       no file at all                   - the agent never started (see item 1).
3. Agent file: type C:\\ProgramData\\inkmetrics\\agent.ps1 - the first line must
   read "agent.ps1 - host monitoring agent". If it is text about installing
   sharing, an older kit overwrote the agent: remove it (deinstall.cmd) and
   install again with this file.
4. Device: http://192.168.7.1/api/state, the ingest block (count and age). count
   must grow every minute, age must be seconds.
5. Device firmware: only 0.4.0 and newer accepts metrics (the HOST SYS page and
   the /ingest request). The version is shown on the device page and screen.

Requirements and limitations
----------------------------
* Windows 10 or 11, administrator rights during the installation.
* The device connected over USB; device firmware 0.4.0 or newer.
* The scripts are ASCII on purpose: cmd.exe and PowerShell 5.1 mangle Russian
  text in .cmd/.ps1 files under the system codepage, so messages are English and
  the details live in this file.
* macOS will not bring the device network up without extra drivers; on Linux the
  scripts are Windows-only. This is a Windows tool.
* The agent knows the device address 192.168.7.1. If the device got another
  address (for example 192.168.137.x with sharing on), metrics will not arrive -
  see the ICS section above.

Files nearby
------------
This folder holds only these two .cmd files and the two instructions (Russian and
English). A fuller PC setup set lives in the project folder pc-setup (it also has
CHECK.CMD - a detailed check of the agent, the device and the network - and
FIXDISK.PS1, which repairs Windows not showing the device disk).
"""

DOCS = [
    ("README-RU.txt", DOC_RU, True),    # BOM: Notepad reads Russian UTF-8 reliably
    ("README-EN.txt", DOC_EN, False),
]

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
    docs = []
    for name, text, bom in DOCS:
        data = text.replace("\n", "\r\n").encode("utf-8")
        if bom:
            data = b"\xef\xbb\xbf" + data          # Блокнот надёжно читает UTF-8 с BOM
        docs.append((name, data))

    changed: list[str] = []
    OUT.mkdir(parents=True, exist_ok=True)
    for name, data in targets + docs:
        path = OUT / name
        old = path.read_bytes() if path.exists() else b""
        state = "совпадает" if old == data else "изменён"
        if old != data:
            changed.append(name)
            if not args.check:
                path.write_bytes(data)
        print(f"  {name:<15} {len(data):>6} Б  sha256 {hashlib.sha256(data).hexdigest()[:12]}  {state}")

    keep = {n for n, _ in targets} | {n for n, _ in docs}
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
    print("файлов внутри: " + str(len(list(OUT.iterdir()))) + " (2 .cmd + 2 инструкции)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
