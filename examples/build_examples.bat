@echo off
rem Builds example mods (sw_core_lib, radar_telemetry, simple_dllmain_mod, mc_lua_qol, native_api_demo, video_fx, p0_demo).
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

if not exist "..\build\examples\sw_core_lib" mkdir "..\build\examples\sw_core_lib"
if not exist "..\build\examples\radar_telemetry" mkdir "..\build\examples\radar_telemetry"
if not exist "..\build\examples\simple_dllmain_mod" mkdir "..\build\examples\simple_dllmain_mod"
if not exist "..\build\examples\mc_lua_qol" mkdir "..\build\examples\mc_lua_qol"
if not exist "..\build\examples\native_api_demo" mkdir "..\build\examples\native_api_demo"
if not exist "..\build\examples\video_fx" mkdir "..\build\examples\video_fx"
if not exist "..\build\examples\p0_demo" mkdir "..\build\examples\p0_demo"

rem 1. Local dependency math_dep.dll + mod sw_core_lib.dll
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD sw_core_lib\math_dep.cpp ^
   /Fo:..\build\examples\sw_core_lib\ /Fe:..\build\examples\sw_core_lib\math_dep.dll ^
   /link /DLL /IMPLIB:..\build\examples\sw_core_lib\math_dep.lib /MANIFEST:NO /DEBUG:NONE || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD sw_core_lib\sw_core_lib.cpp ^
   ..\build\examples\sw_core_lib\math_dep.lib ^
   /Fo:..\build\examples\sw_core_lib\ /Fe:..\build\examples\sw_core_lib\sw_core_lib.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1

copy /Y sw_core_lib\mod.json ..\build\examples\sw_core_lib\mod.json >nul
del /Q ..\build\examples\sw_core_lib\*.obj ..\build\examples\sw_core_lib\*.exp ..\build\examples\sw_core_lib\*.lib >nul 2>&1

rem 2. Mod radar_telemetry.dll (depends on sw_core_lib)
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD radar_telemetry\radar_telemetry.cpp ^
   /Fo:..\build\examples\radar_telemetry\ /Fe:..\build\examples\radar_telemetry\radar_telemetry.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1

copy /Y radar_telemetry\mod.json ..\build\examples\radar_telemetry\mod.json >nul
del /Q ..\build\examples\radar_telemetry\*.obj ..\build\examples\radar_telemetry\*.exp ..\build\examples\radar_telemetry\*.lib >nul 2>&1

rem 3. Mod simple_dllmain_mod.dll (standard DllMain only)
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD simple_dllmain_mod\simple_dllmain_mod.cpp ^
   /Fo:..\build\examples\simple_dllmain_mod\ /Fe:..\build\examples\simple_dllmain_mod\simple_dllmain_mod.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1

copy /Y simple_dllmain_mod\mod.json ..\build\examples\simple_dllmain_mod\mod.json >nul
del /Q ..\build\examples\simple_dllmain_mod\*.obj ..\build\examples\simple_dllmain_mod\*.exp ..\build\examples\simple_dllmain_mod\*.lib >nul 2>&1

rem 4. Mod mc_lua_qol.dll (Microcontroller Lua QoL & Unlocker)
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD mc_lua_qol\mc_lua_qol.cpp ^
   /Fo:..\build\examples\mc_lua_qol\ /Fe:..\build\examples\mc_lua_qol\mc_lua_qol.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1

copy /Y mc_lua_qol\mod.json ..\build\examples\mc_lua_qol\mod.json >nul
del /Q ..\build\examples\mc_lua_qol\*.obj ..\build\examples\mc_lua_qol\*.exp ..\build\examples\mc_lua_qol\*.lib >nul 2>&1

rem 5. Stable SDK example: custom microcontroller Lua library (no game addresses)
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD native_api_demo\native_api_demo.cpp ^
   /Fo:..\build\examples\native_api_demo\ /Fe:..\build\examples\native_api_demo\native_api_demo.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1
copy /Y native_api_demo\mod.json ..\build\examples\native_api_demo\mod.json >nul
del /Q ..\build\examples\native_api_demo\*.obj ..\build\examples\native_api_demo\*.exp ..\build\examples\native_api_demo\*.lib >nul 2>&1

rem 6. Video FX: plugin uses stable loader API
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD video_fx\video_fx.cpp ^
   /Fo:..\build\examples\video_fx\ /Fe:..\build\examples\video_fx\video_fx.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1
copy /Y video_fx\mod.json ..\build\examples\video_fx\mod.json >nul
copy /Y video_fx\demo.lua ..\build\examples\video_fx\demo.lua >nul
del /Q ..\build\examples\video_fx\*.obj ..\build\examples\video_fx\*.exp ..\build\examples\video_fx\*.lib >nul 2>&1

rem 7. P0 native HUD, runtime events, input and storage
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD p0_demo\p0_demo.cpp ^
   /Fo:..\build\examples\p0_demo\ /Fe:..\build\examples\p0_demo\p0_demo.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1
copy /Y p0_demo\mod.json ..\build\examples\p0_demo\mod.json >nul
copy /Y p0_demo\demo.lua ..\build\examples\p0_demo\demo.lua >nul
copy /Y p0_demo\README.md ..\build\examples\p0_demo\README.md >nul

echo OK : Example mods built in build\examples\
exit /b 0
