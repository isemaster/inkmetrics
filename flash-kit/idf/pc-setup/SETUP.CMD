@echo off
title inkmetrics - настройка компьютера
echo.
echo   inkmetrics: настраиваю этот компьютер для прибора
echo.
echo   1. Сейчас появится запрос прав администратора - нажмите "Да".
echo   2. Скрипт поставит агента и включит раздачу интернета для прибора.
echo   3. Адрес страницы смотрите на экране прибора (строка ADDR).
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath powershell -Verb RunAs -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0AGENT.PS1')"
echo.
echo   Готово. Нажмите любую клавишу, чтобы закрыть окно.
pause >nul
