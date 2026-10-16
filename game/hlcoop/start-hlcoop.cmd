@echo off
chcp 65001 >nul
if not exist "%~dp0..\hl.exe" (
    echo Не найден hl.exe. Папка hlcoop должна находиться рядом с hl.exe
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0update-hlcoop.ps1" -Launch
if errorlevel 1 pause
