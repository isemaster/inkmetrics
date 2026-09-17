@echo off
rem inkmetrics - check this PC, the agent and the device. ASCII only on purpose.
rem Just double-click this file. The window stays open and a report file is written.
setlocal
set "HERE=%~dp0"

echo.
echo   inkmetrics: checking this PC, the agent, the tasks and the device...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%CHECK.PS1" %*

echo.
echo   Done. The report file path is printed above (INKMETRICS-CHECK-REPORT.TXT in %%TEMP%%).
echo   Send that file if something needs a closer look.
echo.
pause
