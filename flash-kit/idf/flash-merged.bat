@echo off
rem Flash inkmetrics (ESP-IDF build, USB network) as ONE merged image at 0x0.
rem Usage: flash-merged.bat COM5
rem Put the board into bootloader first: hold BOOT, plug USB, keep 2 s, release.
rem Note: this build currently reboots every ~20 s on hardware; flash for debugging only.
rem ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .bat as garbage.
setlocal
if "%~1"=="" (
  echo.
  echo Usage: flash-merged.bat COMx     ^(example: flash-merged.bat COM5^)
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

echo Flashing merged IDF image to %~1 ...
%PY% -m esptool --chip esp32s3 --port %~1 --baud 921600 write-flash -z ^
  0x0 "%~dp0inkmetrics-idf-0x0.bin"
if errorlevel 1 (
  echo.
  echo FAILED. Check the port and that the board is in bootloader mode ^(hold BOOT, plug USB^).
  pause
  exit /b 1
)
echo.
echo DONE. Windows should show a network adapter "Remote NDIS based Internet Sharing Device".
pause
