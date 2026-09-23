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
    ("net.ps1",   PS / "agent_cmd_net.ps1"),
    ("setup.ps1", PS / "agent_cmd_setup.ps1"),
]
DEINST_PAYLOADS = [
    ("net.ps1",    PS / "agent_cmd_net.ps1"),
    ("remove.ps1", PS / "agent_cmd_remove.ps1"),
]

MARKER = "rem =====PAYLOAD:{name}====="

DOC_RU = """ИНСТРУКЦИЯ: агент метрик inkmetrics (прибор работает как монитор)

Что это
-------
Два файла командной строки, которые ставят и снимают агента на любом компьютере
с Windows 10 или 11:

    instagent.cmd   установка агента
    deinstall.cmd   удаление агента

Код агента лежит ВНУТРИ этих файлов: рядом больше ничего не нужно, ни Python, ни
ESP-IDF, ни драйверов. Запускать можно из любой папки - с флешки, из сетевой папки,
с рабочего стола.

Прибор работает как МОНИТОР: интернета у него нет и раздача интернета (ICS) в этом
наборе не используется вообще. Это сделано намеренно - так вся схема проще.

Адреса: всегда одни и те же
---------------------------
    прибор (на его экране, строка ADDR) : 192.168.7.1
    этот компьютер, адаптер прибора      : 192.168.7.2/24

Ничего искать и согласовывать не нужно: адреса фиксированные, агент всегда шлёт
метрики на 192.168.7.1, а установщик ставит компьютеру 192.168.7.2.

Важно: адрес ставится БЕЗ ШЛЮЗА и с метрикой 9000. Если Windows получит аренду от
прибора вместе со шлюзом 192.168.7.1, то весь интернет компьютера уйдёт в прибор -
внешний интернет на этом компьютере пропадёт. Установщик делает правильно сам, но
если адрес ставите руками - шлюз указывать нельзя.

Что именно ставится
-------------------
    папка установки : C:\\ProgramData\\inkmetrics
    файлы           : agent.ps1   агент метрик (CPU, память, диск, пинг -> прибор)
                      net.ps1     адрес на линии прибора (192.168.7.2 без шлюза)
                      setup.ps1   служебный: ставит задачу и запускает агента
                      agent.log   журнал агента (создаёт сам агент)
                      agent.pid   номер процесса агента
                      cpu.state   предыдущий замер CPU (чтобы считать проценты)
    задача          : "inkmetrics agent" - при старте, при входе и раз в минуту,
                      от имени SYSTEM
    в реестр, в автозагрузку и в службы ничего не прописывается, сторонних
    программ не устанавливается. Раздача интернета не включается.

Установка (5 шагов)
-------------------
1. Подключить прибор к компьютеру USB-кабелем (напрямую в порт, без хаба).
2. Запустить instagent.cmd двойным кликом. Появится запрос прав администратора -
   согласиться ("Да"). Права нужны один раз: для адреса адаптера и задачи.
3. Дождаться строк "set : 192.168.7.2/24, no gateway", "task inkmetrics agent :
   registered" и "Agent installed". Окно само держится открытым.
4. Проверить на приборе: на экране должна быть рамка ONLINE (значит, у этого компьютера
   есть интернет), под ней - температура видеокарты крупными цифрами, а ниже - проценты
   CPU / RAM / DISK и аптайм. Короткое нажатие PWR переключает на экран SETUP и обратно.
   Числа должны совпадать с этим компьютером. Если в рамке NO DATA - агент не шлёт
   метрики, смотрите пункт 5.
5. Проверить на компьютере (любая из проверок):

       schtasks /query /tn "inkmetrics agent" /v /fo LIST
       type C:\\ProgramData\\inkmetrics\\agent.log

   В журнале раз в минуту должны появляться строки "sent ok (cpu=..%)".

Что агент делает
----------------
Раз в 60 секунд агент собирает метрики этого компьютера и отправляет их прибору
запросом POST на http://192.168.7.1/ingest: загрузку CPU, память, диск, температуры и
загрузку видеокарт (по всем картам), аптайм. Отдельно агент проверяет интернет этого
компьютера (пинг цели, по умолчанию 8.8.8.8, при молчании ICMP - проверка TCP 443):
именно по этому ответу прибор пишет в рамке ONLINE или OFFLINE.

Если метрики не приходят дольше 90 секунд, прибор пишет NO DATA и показывает прочерки
вместо чисел - старые данные за текущие он не выдаёт.

Проверить канал руками (в окне PowerShell):

    curl.exe -s -X POST -H 'Content-Type: application/json' -d '{"cpu_percent":50.0}' http://192.168.7.1/ingest

Ответ "ok" и число 50 на экране прибора означают, что канал рабочий и дело только
в агенте. Эти числа - проба: агент перезапишет их своим замером в течение минуты.

Удаление
--------
Запустить deinstall.cmd двойным кликом (тоже спросит права администратора). Он
останавливает агент, снимает задачу "inkmetrics agent", возвращает адаптеру прибора
адрес по DHCP и удаляет папку C:\\ProgramData\\inkmetrics. Больше ничего не
трогается: ярлыки, реестр, службы и прочие сетевые настройки остаются как были.

Ключи и переменные
------------------
    instagent.cmd --dry-run    ничего не менять: распаковать файлы во временную
                               папку и показать, что было бы сделано
    instagent.cmd --keep-net   не трогать адрес адаптера (адрес уже настроен)
    deinstall.cmd --dry-run    только показать состояние: задача, процесс, папка
    deinstall.cmd --keep-net   не возвращать адрес в DHCP
    EINK_INSTALL_DIR=путь      поставить агента в другую папку вместо
                               C:\\ProgramData\\inkmetrics (например для проверки)

Безопасный порядок осмотра: instagent.cmd --dry-run, затем deinstall.cmd --dry-run.

Если метрик нет - что смотреть по порядку
-----------------------------------------
1. Задача: schtasks /query /tn "inkmetrics agent". Нет задачи - установка не
   дошла: запустите instagent.cmd заново, с правами администратора.
2. Журнал: type C:\\ProgramData\\inkmetrics\\agent.log
       "agent started ... sent ok"       - всё работает;
       "send error: ..."                 - агент жив, но прибор недоступен:
                                           проверьте кабель и адрес адаптера;
       файла нет                         - агент не запускается (см. пункт 1).
3. Файл агента: type C:\\ProgramData\\inkmetrics\\agent.ps1 - первая строка должна
   быть "agent.ps1 - host monitoring agent". Если там другой текст, файл подменён
   старым набором: удалите (deinstall.cmd) и поставьте заново.
4. Адрес: в окне PowerShell

       Get-NetIPAddress -AddressFamily IPv4 | Where-Object InterfaceAlias -like '*NDIS*'

   у адаптера прибора должно быть 192.168.7.2 и не должно быть шлюза по умолчанию
   (Get-NetRoute -DestinationPrefix 0.0.0.0/0 - на нём ноль маршрутов). Если шлюз
   есть - интернет компьютера уходит в прибор: уберите шлюз, оставьте только адрес.
5. Прибор: страница http://192.168.7.1/api/state, поле ingest (count и age).
   count растёт каждую минуту, age - секунды.
6. Прошивка прибора: метрики принимает только 0.5.0 и новее (сводный экран и запрос
   /ingest); два крупных числа на сводном экране выбираются на странице настроек с 0.6.0.
   Версия показана на странице прибора и на экране SETUP прибора.

Требования и ограничения
------------------------
* Windows 10 или 11, права администратора на время установки.
* Прибор подключён по USB; прошивка прибора 0.5.0 или новее (крупные числа на сводном
  экране настраиваются с 0.6.0).
* Загрузку видеокарты агент берёт из nvidia-smi, а если его нет - из системных счётчиков
  "GPU Engine" (любой вендор, Windows 10 1709 и новее). Температуры карт есть только там,
  где установлен nvidia-smi; температуры процессора агент не показывает: штатного
  источника в Windows нет.
* Интернета у прибора нет по устройству: это монитор. Компьютер при этом сохраняет
  свой интернет, потому что адрес на линии прибора ставится без шлюза.
* Скрипты написаны латиницей намеренно: cmd.exe и PowerShell 5.1 при системной
  кодировке портят русский текст в .cmd/.ps1, поэтому сообщения английские, а
  подробности - в этом файле.
* macOS без сторонних драйверов сеть прибора не поднимет; на Linux скрипты
  Windows-only. Это инструмент для Windows.

Чего этот набор НЕ делает
-------------------------
* Не настраивает раздачу интернета прибору - её в этой схеме нет.
* Не ремонтирует случай, когда Windows не показала диск прибора: для этого есть
  FIXDISK.PS1 в папке pc-setup проекта.
* Не делает подробную проверку агента, прибора и сети с вердиктами: это CHECK.CMD
  из той же папки pc-setup.
* Не меняет прошивку прибора.

Файлы рядом
-----------
В этой папке четыре файла: два .cmd и две инструкции (русская и английская).
Ровно те же четыре файла лежат на диске прибора - его видно в Windows как обычный
съёмный диск, и агента можно поставить прямо с него, ничего не копируя.
"""

DOC_EN = """INSTRUCTIONS: the inkmetrics metrics agent (the device works as a monitor)

What this is
------------
Two command files that install and remove the agent on any Windows 10/11 PC:

    instagent.cmd   install the agent
    deinstall.cmd   remove the agent

The agent code travels INSIDE these files: nothing else is needed next to them, no
Python, no ESP-IDF, no drivers. They can be run from any folder - a USB stick, a
network share, the desktop.

The device works as a MONITOR: it has no internet, and internet sharing (ICS) is not
used in this set at all. That is deliberate - it keeps the whole scheme simple.

Addresses: always the same
--------------------------
    device (the ADDR line on its screen)  : 192.168.7.1
    this PC, device adapter               : 192.168.7.2/24

Nothing has to be looked up or negotiated: the addresses are fixed, the agent always
posts to 192.168.7.1, and the installer gives this PC 192.168.7.2.

Important: the address is set WITHOUT a gateway and with metric 9000. If Windows takes
a lease from the device together with the gateway 192.168.7.1, the host internet goes
into the device and this PC loses its internet. The installer does it right; if you set
the address by hand, do not fill in a gateway.

What gets installed
-------------------
    install folder : C:\\ProgramData\\inkmetrics
    files          : agent.ps1   the metrics agent (CPU, memory, disk, ping -> device)
                     net.ps1     the address on the device link (192.168.7.2, no gateway)
                     setup.ps1   helper: registers the task and starts the agent
                     agent.log   agent journal (written by the agent itself)
                     agent.pid   the agent process id
                     cpu.state   previous CPU sample (needed to compute percents)
    task           : "inkmetrics agent" - at startup, at logon and every minute, as SYSTEM
    Nothing is added to the registry, to startup or to services; no third-party software
    is installed. Internet sharing is not enabled.

Installation (5 steps)
----------------------
1. Connect the device to the PC with a USB cable (directly to a port, no hub).
2. Start instagent.cmd by double click. Windows asks for administrator rights - accept
   ("Yes"). The rights are needed once: for the adapter address and the task.
3. Wait for "set : 192.168.7.2/24, no gateway", "task inkmetrics agent : registered"
   and "Agent installed". The window stays open.
4. Check the device: the frame must read ONLINE (this PC has internet), below it the GPU
   temperature in large digits, then CPU / RAM / DISK percentages and uptime. A short PWR
   press switches to the SETUP screen and back. The numbers must match this PC; NO DATA in
   the frame means the agent is not sending (see step 5).
5. Check on the PC (either):

       schtasks /query /tn "inkmetrics agent" /v /fo LIST
       type C:\\ProgramData\\inkmetrics\\agent.log

   The journal must get "sent ok (cpu=..%)" lines once a minute.

What the agent does
-------------------
Every 60 seconds the agent collects this PC's metrics and sends them to the device with
POST http://192.168.7.1/ingest: CPU load, memory, disk, temperature and load of every
NVIDIA card, uptime. Separately the agent checks this PC's internet access (ping to the
target, 8.8.8.8 by default, with a TCP 443 fallback when ICMP stays silent) - that answer
is what the device prints as ONLINE or OFFLINE in the frame.

When no metrics arrive for 90 seconds the device shows NO DATA and replaces the numbers
with dashes: it will not pass old readings off as current ones.

Manual channel test (from a PowerShell window):

    curl.exe -s -X POST -H 'Content-Type: application/json' -d '{"cpu_percent":50.0}' http://192.168.7.1/ingest

An "ok" answer plus 50 on the device screen means the channel works and only the agent is
in question. Those numbers are a probe: the agent overwrites them with a real sample
within a minute.

Removal
-------
Run deinstall.cmd by double click (it asks for administrator rights too). It stops the
agent, removes the task "inkmetrics agent", gives the device adapter address back to
DHCP and deletes C:\\ProgramData\\inkmetrics. Nothing else is touched: shortcuts, the
registry, services and other network settings stay as they were.

Switches and variables
----------------------
    instagent.cmd --dry-run    change nothing: unpack into a temporary folder and show
                               what would be done
    instagent.cmd --keep-net   do not touch the adapter address (already configured)
    deinstall.cmd --dry-run    only report the state: task, process, folder
    deinstall.cmd --keep-net   do not give the address back to DHCP
    EINK_INSTALL_DIR=path      install into another folder instead of
                               C:\\ProgramData\\inkmetrics (used for testing)

A safe way to look around: instagent.cmd --dry-run, then deinstall.cmd --dry-run.

No metrics - what to check, in order
------------------------------------
1. Task: schtasks /query /tn "inkmetrics agent". If it is missing, the installation did
   not finish: run instagent.cmd again as administrator.
2. Journal: type C:\\ProgramData\\inkmetrics\\agent.log
       "agent started ... sent ok"      - all good;
       "send error: ..."                - the agent runs but cannot reach the device:
                                          check the cable and the adapter address;
       no file at all                   - the agent never started (see item 1).
3. Agent file: type C:\\ProgramData\\inkmetrics\\agent.ps1 - the first line must read
   "agent.ps1 - host monitoring agent". Any other text means an older kit overwrote it:
   remove it (deinstall.cmd) and install again.
4. Address: in a PowerShell window

       Get-NetIPAddress -AddressFamily IPv4 | Where-Object InterfaceAlias -like '*NDIS*'

   the device adapter must be 192.168.7.2 and must have no default route
   (Get-NetRoute -DestinationPrefix 0.0.0.0/0 - zero routes on it). A gateway there means
   the host internet goes into the device: remove the gateway, keep only the address.
5. Device: http://192.168.7.1/api/state, the ingest block (count and age). count must
   grow every minute, age must be seconds.
6. Device firmware: only 0.5.0 and newer accepts metrics (the summary screen and the
   /ingest request); the two large numbers on the summary screen are configurable from
   0.6.0 on. The version is shown on the device page and on its SETUP screen.

Requirements and limitations
----------------------------
* Windows 10 or 11, administrator rights during the installation.
* The device connected over USB; device firmware 0.5.0 or newer (the large numbers on the
  summary screen are configurable from 0.6.0).
* GPU load comes from nvidia-smi, or - when it is not installed - from the Windows
  "GPU Engine" counters (any vendor, Windows 10 1709+). GPU temperatures exist only where
  nvidia-smi is installed; CPU temperature is not reported at all: Windows has no stock
  source for it.
* The device has no internet by design: it is a monitor. This PC keeps its own internet
  because the address on the device link is set without a gateway.
* The scripts are ASCII on purpose: cmd.exe and PowerShell 5.1 mangle Russian text in
  .cmd/.ps1 files under the system codepage, so messages are English and the details
  live in this file.
* macOS will not bring the device network up without extra drivers; on Linux the scripts
  are Windows-only. This is a Windows tool.

What this set does NOT do
-------------------------
* It does not set up internet sharing for the device - there is none in this scheme.
* It does not repair the case when Windows shows no device disk: FIXDISK.PS1 in the
  project folder pc-setup does that.
* It does not run the detailed check of the agent, the device and the network with
  verdicts: that is CHECK.CMD from the same pc-setup folder.
* It does not touch the device firmware.

Files nearby
------------
This folder holds four files: the two .cmd files and the two instructions (Russian and
English). Exactly the same four files sit on the device disk, which Windows shows as an
ordinary removable drive - the agent can be installed right from it, nothing to copy.
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
rem      net.ps1    the fixed address on the device link (192.168.7.2, no gateway)
rem      setup.ps1  registers the task and starts everything
rem  Task: "inkmetrics agent" - as SYSTEM, at startup, at logon and every minute.
rem
rem  The device is a MONITOR only: no internet sharing. The addresses are fixed -
rem  the device is always 192.168.7.1, this PC gets 192.168.7.2 on the device adapter
rem  (without a gateway, so the host internet never goes into the device).
rem
rem  Usage:   instagent.cmd              install (asks for administrator rights)
rem           instagent.cmd --keep-net   do not touch the adapter address
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
set "KEEPNET="
set "DIR="
for %%A in (%*) do (
    if /i "%%A"=="--dry-run"  set "DRY=1"
    if /i "%%A"=="--keep-net" set "KEEPNET=1"
)

if defined EINK_INSTALL_DIR set "DIR=%EINK_INSTALL_DIR%"
if not defined DIR if defined DRY set "DIR=%TEMP%\inkmetrics-dry-run"
if not defined DIR set "DIR=%ProgramData%\inkmetrics"

echo.
echo   inkmetrics: agent installer (device as a monitor)
echo   install folder : %DIR%
if defined DRY echo   mode           : DRY RUN - files go to TEMP, nothing is registered
if defined KEEPNET echo   address        : left as is - the adapter is NOT touched
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
    echo   DRY RUN: would set the fixed address on the device adapter, stop an old agent,
    echo   register the metrics task and start it. Nothing was changed.
    echo   Unpacked files are in "%DIR%" - delete that folder by hand.
    exit /b 0
)

set "SETUPARGS="
if defined KEEPNET set "SETUPARGS=-KeepNet"

echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%DIR%\setup.ps1" %SETUPARGS%
if errorlevel 1 (
    echo.
    echo   The installer reported a problem - see the lines above.
    pause
    exit /b 1
)

echo.
echo   Agent installed. The device is used as a monitor: this PC is 192.168.7.2 on the
echo   device link, the device is always 192.168.7.1.
echo   On the device the frame must read ONLINE and the numbers (GPU temperature, CPU / RAM
echo   / DISK) must match this PC.
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
rem  works from any folder. It stops the agent, removes the task "inkmetrics
rem  agent", gives the device adapter address back to DHCP and deletes the install
rem  folder (C:\ProgramData\inkmetrics, or EINK_INSTALL_DIR if that is set).
rem  Nothing else on this PC is touched.
rem
rem  Usage:   deinstall.cmd              remove everything (asks for admin rights)
rem           deinstall.cmd --keep-net   keep the adapter address as it is
rem           deinstall.cmd --dry-run    only report what is installed
rem
rem  ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .cmd as garbage.
rem  Below the batch part there are payload sections, each starting with a rem marker
rem  line; from the first marker on the file is DATA, not batch code.
rem ============================================================================
setlocal EnableExtensions
set "SELF=%~f0"
set "DRY="
set "KEEPNET="
set "DIR="
for %%A in (%*) do (
    if /i "%%A"=="--dry-run"  set "DRY=1"
    if /i "%%A"=="--keep-net" set "KEEPNET=1"
)
if defined EINK_INSTALL_DIR set "DIR=%EINK_INSTALL_DIR%"
if not defined DIR set "DIR=%ProgramData%\inkmetrics"

set "RARGS="
if defined KEEPNET set "RARGS=-KeepNet"

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
    powershell -NoProfile -ExecutionPolicy Bypass -File "%EINK_DIR%\remove.ps1" -DryRun -InstallDir "%DIR%" %RARGS%
    echo.
    echo   DRY RUN: nothing was changed.
    exit /b 0
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%EINK_DIR%\remove.ps1" -InstallDir "%DIR%" %RARGS%

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
