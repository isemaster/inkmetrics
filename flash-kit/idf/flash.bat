@echo off
rem inkmetrics (ESP-IDF build): firmware + host disk image.
rem Usage: flash.bat COM5
rem Put the board into bootloader first: hold BOOT, plug USB, keep 2 s, release.
rem The disk image is written in the same run: it is what the device shows the PC
rem as a 3.69 MB removable drive with instagent.cmd and the instructions.
rem Command names use UNDERSCORES on purpose: esptool 4.x accepts only that form,
rem 5.x accepts both, so this spelling works with any version (write-flash fails on 4.x).
rem ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .bat as garbage.
setlocal
if "%~1"=="" (
  echo.
  echo Usage: flash.bat COMx     ^(example: flash.bat COM5^)
  echo Ports found:
  python -m serial.tools.list_ports -v
  echo.
  pause
  exit /b 1
)

rem esptool may live in another interpreter than plain "python" - probe both
set PY=python
%PY% -c "import esptool" >nul 2>&1 || set PY=py -3
%PY% -c "import esptool" >nul 2>&1
if errorlevel 1 (
  echo esptool not found. Install it first:  pip install esptool
  pause
  exit /b 1
)

echo Flashing inkmetrics to %~1 ...
rem --after no_reset keeps the board in the bootloader after writing: the port stays the
rem same one and the download flag is cleared below BEFORE the board leaves the bootloader
rem (otherwise the command below races with the USB re-enumeration and fails, leaving a
rem freshly flashed board sitting in the bootloader).
%PY% -m esptool --chip esp32s3 --port %~1 --baud 921600 --after no_reset write_flash -z ^
  0x0      "%~dp0bootloader.bin" ^
  0x8000   "%~dp0partition-table.bin" ^
  0xe000   "%~dp0ota_data_initial.bin" ^
  0x20000  "%~dp0inkmetrics_idf.bin" ^
  0x430000 "%~dp0setup-disk-big.img"
if errorlevel 1 (
  echo.
  echo FAILED. Check the port and that the board is in bootloader mode ^(hold BOOT, plug USB^).
  pause
  exit /b 1
)

rem Clear the sticky RTC bit (a previous /api/boot or self-check fallback sets it) and reset
rem into the application, so USB does not have to be replugged and the board actually starts.
%PY% -m esptool --chip esp32s3 --port %~1 --before no_reset --after watchdog_reset ^
  write_mem 0x6000812C 0x00 0x1
if errorlevel 1 (
  echo.
  echo WARNING: could not clear the download flag. If the board does not start, unplug USB
  echo and plug it in again - it will boot normally.
)

echo.
echo DONE. The device should start: the screen shows the summary page (ONLINE / OFFLINE /
echo NO DATA), the PC gets a new disk and a network adapter. On that disk: instagent.cmd
echo installs the agent, README-RU.txt explains the rest.
pause
