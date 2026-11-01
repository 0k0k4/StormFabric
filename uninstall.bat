@echo off
rem Retire le proxy du dossier du jeu. Usage : uninstall.bat [chemin_du_dossier_du_jeu]
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%~dp0..\Stormworks"
del /Q "%GAME%\iphlpapi.dll" "%GAME%\StormworksModLoader.log" 2>nul
echo Desinstalle de "%GAME%"
