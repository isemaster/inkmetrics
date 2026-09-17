@echo off
rem inkmetrics (ESP-IDF build): firmware + host disk image.
rem Usage: flash.bat COM5
rem Put the board into bootloader first: hold BOOT, plug USB, keep 2 s, release.
rem The disk image contains SETUP.CMD - the device brings the PC
rem setup scripts with it (see pc-setup folder for copies).
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
%PY% -m esptool --chip esp32s3 --port %~1 --baud 921600 write-flash -z ^
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
rem into the application, so USB does not have to be replugged.
%PY% -m esptool --chip esp32s3 --port %~1 --before no-reset --after no-reset ^
  write-mem 0x6000812C 0x00

echo.
echo DONE. The device should start: screen shows DEVICE page, the PC gets a new disk
echo and a network adapter. On that disk: instagent.cmd installs the agent,
pause
