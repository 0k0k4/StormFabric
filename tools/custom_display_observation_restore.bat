@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0observe.ps1" -Mode Restore
pause
