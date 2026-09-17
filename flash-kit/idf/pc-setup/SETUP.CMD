@echo off
title inkmetrics - настройка компьютера
echo.
echo   inkmetrics: настраиваю этот компьютер для прибора
echo.
echo   1. Сейчас появится запрос прав администратора - нажмите "Да".
echo   2. Скрипт поставит агента метрик и включит раздачу интернета для прибора.
echo   3. Метрики смотрите на экране прибора: страница HOST SYS (кнопка PWR),
echo      либо на странице прибора, строка "Метрики хоста".
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath powershell -Verb RunAs -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0MINSTALL.PS1')"
echo.
echo   Готово. Нажмите любую клавишу, чтобы закрыть окно.
pause >nul
