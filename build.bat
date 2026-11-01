@echo off
rem Builds build\iphlpapi.dll (StormFabric proxy + mod loader UI) and build\mods\CustomDisplay\custom_display.dll.
rem Requires MSVC x64 compiler (cl.exe).
setlocal

if not defined VCVARS (
  if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  ) else (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
      if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)

if not defined VCVARS (
  echo ERROR: vcvars64.bat not found. Set VCVARS environment variable.
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"

if not exist build mkdir build
if not exist build\core mkdir build\core
if not exist build\mods\CustomDisplay mkdir build\mods\CustomDisplay

echo ========================================================
echo [1/2] Building StormFabric (iphlpapi.dll)
echo ========================================================
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /GS- /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /LD src\dllmain.cpp src\game.cpp src\log.cpp src\engine_discovery.cpp src\engine_bindings.cpp ^
       src\mods_scan.cpp src\mods_loader.cpp src\mods_ui.cpp src\engine_api.cpp src\engine_profile.cpp ^
       src\native_lua.cpp src\native_video.cpp src\notification_overlay.cpp src\ingame_notifications.cpp ^
       src\mod_runtime.cpp src\mod_native_ui.cpp src\mod_storage.cpp ^
   /Fo:build\core\ /Fe:build\iphlpapi.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE /OPT:REF /OPT:ICF opengl32.lib bcrypt.lib || exit /b 1

echo.
echo ========================================================
echo [2/2] Building CustomDisplay mod (custom_display.dll)
echo ========================================================
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /GS- /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /LD src\custom_display_mod_main.cpp src\custom_display_layout.cpp src\custom_display_dialog_layout.cpp ^
       src\custom_display_dialog_text.cpp src\custom_display_dialog_native.cpp ^
       src\custom_display_dialog_native_bindings.cpp src\custom_display_dialog_controller.cpp ^
       src\custom_display_vehicle_observer.cpp src\custom_display_observer.cpp ^
       src\custom_display_catalogue.cpp src\custom_display_dialog_callbacks.cpp ^
       src\custom_display_dialog_input.cpp src\custom_display_registry.cpp ^
       src\engine_discovery.cpp src\engine_bindings.cpp src\log.cpp ^
   /Fo:build\mods\CustomDisplay\ /Fe:build\mods\CustomDisplay\custom_display.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE /OPT:REF /OPT:ICF opengl32.lib bcrypt.lib || exit /b 1

if exist mods\CustomDisplay\mod.json (
  copy /Y mods\CustomDisplay\mod.json build\mods\CustomDisplay\mod.json >nul
)

rem Optional deployment if --deploy <path> or STORMWORKS_DIR is set
set "TARGET_GAME="
if "%~1"=="--deploy" if not "%~2"=="" set "TARGET_GAME=%~2"
if not defined TARGET_GAME if defined STORMWORKS_DIR set "TARGET_GAME=%STORMWORKS_DIR%"

if defined TARGET_GAME (
  if exist "%TARGET_GAME%\stormworks64.exe" (
    echo.
    echo Deploying to %TARGET_GAME%...
    copy /Y "%~dp0build\iphlpapi.dll" "%TARGET_GAME%\iphlpapi.dll" >nul
    if not exist "%TARGET_GAME%\mods\CustomDisplay" mkdir "%TARGET_GAME%\mods\CustomDisplay"
    copy /Y "%~dp0build\mods\CustomDisplay\custom_display.dll" "%TARGET_GAME%\mods\CustomDisplay\custom_display.dll" >nul
    copy /Y "%~dp0build\mods\CustomDisplay\mod.json" "%TARGET_GAME%\mods\CustomDisplay\mod.json" >nul
    echo Deployment completed successfully.
  ) else (
    echo WARNING: Target game directory does not contain stormworks64.exe: %TARGET_GAME%
  )
)

echo.
echo Build successful:
echo   - %~dp0build\iphlpapi.dll
echo   - %~dp0build\mods\CustomDisplay\custom_display.dll
