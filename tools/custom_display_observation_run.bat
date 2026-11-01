@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0observe.ps1" -Mode Run
if errorlevel 1 echo ERROR: session incomplete. Read the message above; do not replace DLLs manually.
pause
