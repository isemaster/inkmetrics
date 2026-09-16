@echo off
rem Flash inkmetrics (ESP-IDF build, USB network) to ESP32-S3-ePaper-1.54. Four images.
rem Usage: flash.bat COM5
rem Put the board into bootloader first: hold BOOT, plug USB, keep 2 s, release.
rem Note: debug build. Self-check: if the host stays silent for 5 minutes the board
rem enters bootloader by itself (idf/main/selfcheck.c), so BOOT is not needed to re-flash.
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

echo Flashing IDF build to %~1 ...
%PY% -m esptool --chip esp32s3 --port %~1 --baud 921600 write-flash -z ^
  0x0     "%~dp0bootloader.bin" ^
  0x8000  "%~dp0partition-table.bin" ^
  0xe000  "%~dp0ota_data_initial.bin" ^
  0x20000 "%~dp0inkmetrics_idf.bin"
if errorlevel 1 (
  echo.
  echo FAILED. Check the port and that the board is in bootloader mode ^(hold BOOT, plug USB^).
  pause
  exit /b 1
)
rem Clear the sticky RTC force-download bit, then reset into the app. The bit is set by
rem /api/boot or by the automatic fallback; while it is set the ROM re-enters download
rem mode on every reset and the app would not start until USB is re-plugged.
%PY% -m esptool --chip esp32s3 --port %~1 --before no-reset --after watchdog-reset ^
  write-mem 0x6000812C 0x00
echo.
echo DONE. Windows should show a network adapter "Remote NDIS based Internet Sharing Device".
pause
