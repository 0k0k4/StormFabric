@echo off
rem Copie build\iphlpapi.dll et les mods dans le dossier Stormworks.
rem Usage : install.bat [chemin_du_dossier_du_jeu]
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%~dp0..\Stormworks"
if not exist "%GAME%\stormworks64.exe" (
  echo stormworks64.exe introuvable dans "%GAME%"
  exit /b 1
)
if not exist "%~dp0build\iphlpapi.dll" (
  echo build\iphlpapi.dll absent : lancez build.bat d'abord
  exit /b 1
)
copy /Y "%~dp0build\iphlpapi.dll" "%GAME%\iphlpapi.dll" >nul || exit /b 1
echo Installe : "%GAME%\iphlpapi.dll"

if exist "%~dp0build\mods" (
  if not exist "%GAME%\mods" mkdir "%GAME%\mods"
  xcopy /E /I /Y "%~dp0build\mods" "%GAME%\mods" >nul
  echo Mods installes dans : "%GAME%\mods"
)
echo Journal : "%GAME%\StormworksModLoader.log"
