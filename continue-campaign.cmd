@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\launch.ps1" -Mode Coop -LoadSave quick -Windowed
if errorlevel 1 pause
