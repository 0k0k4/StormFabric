@echo off
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
cd /d "%~dp0.."

rem Target game executable for binary-dependent discovery tests (optional)
set "GAME_EXE=%~1"
if not defined GAME_EXE if defined STORMWORKS_EXE set "GAME_EXE=%STORMWORKS_EXE%"
if not defined GAME_EXE if exist "..\Stormworks\stormworks64.exe" set "GAME_EXE=..\Stormworks\stormworks64.exe"

rem Optional second authenticated executable for offline tests
set "NEWER_EXE=%~2"
if not defined NEWER_EXE if defined STORMWORKS_NEWER_EXE set "NEWER_EXE=%STORMWORKS_NEWER_EXE%"

if not exist build\tests mkdir build\tests

echo --- Running pure C/C++ unit test suites ---
cl /nologo /TC /W4 /c tests\sdk_c_smoke.c /Fo:build\tests\sdk_c_smoke.obj || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\engine_api_tests.cpp src\engine_api.cpp src\game.cpp src\engine_bindings.cpp src\engine_discovery.cpp src\mod_runtime.cpp src\mod_native_ui.cpp src\mod_storage.cpp /Fo:build\tests\ /Fe:build\tests\engine_api_tests.exe /link bcrypt.lib || exit /b 1
build\tests\engine_api_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\lua_profile_tests.cpp src\engine_profile.cpp src\native_video.cpp src\game.cpp src\engine_bindings.cpp src\engine_discovery.cpp /Fo:build\tests\ /Fe:build\tests\lua_profile_tests.exe /link opengl32.lib bcrypt.lib || exit /b 1

if defined GAME_EXE if exist "%GAME_EXE%" (
  build\tests\lua_profile_tests.exe "%GAME_EXE%" || exit /b 1
) else (
  echo [SKIP] Game binary not found; skipping lua_profile_tests on target binary.
)

if defined NEWER_EXE (
  if exist "%NEWER_EXE%" (
    build\tests\lua_profile_tests.exe "%NEWER_EXE%" || exit /b 1
  ) else (
    echo [SKIP] Requested second executable not found: %NEWER_EXE%
  )
) else (
  echo [SKIP] Second executable unavailable; its offline profile was not retested.
)

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\notification_overlay_tests.cpp src\notification_overlay.cpp /Fo:build\tests\ /Fe:build\tests\notification_overlay_tests.exe || exit /b 1
build\tests\notification_overlay_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 tests\video_model_tests.cpp /Fo:build\tests\ /Fe:build\tests\video_model_tests.exe || exit /b 1
build\tests\video_model_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 tests\custom_display_model_tests.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_model_tests.exe || exit /b 1
build\tests\custom_display_model_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\custom_display_dialog_tests.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_dialog_tests.exe || exit /b 1
build\tests\custom_display_dialog_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\custom_display_dialog_text_tests.cpp src\custom_display_dialog_text.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_dialog_text_tests.exe || exit /b 1
build\tests\custom_display_dialog_text_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\custom_display_dialog_native_tests.cpp src\custom_display_dialog_native.cpp src\custom_display_dialog_controller.cpp src\custom_display_dialog_text.cpp src\custom_display_catalogue.cpp src\custom_display_dialog_callbacks.cpp src\custom_display_dialog_input.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_dialog_native_tests.exe || exit /b 1
build\tests\custom_display_dialog_native_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 tests\custom_display_targets_tests.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_targets_tests.exe || exit /b 1
build\tests\custom_display_targets_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\custom_display_admission_tests.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_admission_tests.exe || exit /b 1
build\tests\custom_display_admission_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\custom_display_observer_tests.cpp src\custom_display_layout.cpp src\engine_bindings.cpp src\engine_discovery.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_observer_tests.exe /link bcrypt.lib || exit /b 1
build\tests\custom_display_observer_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\custom_display_vehicle_observer_tests.cpp src\custom_display_dialog_layout.cpp src\engine_bindings.cpp src\engine_discovery.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_vehicle_observer_tests.exe /link bcrypt.lib || exit /b 1
build\tests\custom_display_vehicle_observer_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\video_backend_tests.cpp src\engine_bindings.cpp src\engine_discovery.cpp /Fo:build\tests\ /Fe:build\tests\video_backend_tests.exe /link opengl32.lib bcrypt.lib || exit /b 1
build\tests\video_backend_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\engine_discovery_tests.cpp src\engine_discovery.cpp src\custom_display_layout.cpp src\custom_display_dialog_layout.cpp src\custom_display_dialog_native.cpp src\custom_display_dialog_native_bindings.cpp /Fo:build\tests\ /Fe:build\tests\engine_discovery_tests.exe || exit /b 1

if defined GAME_EXE if exist "%GAME_EXE%" (
  build\tests\engine_discovery_tests.exe "%GAME_EXE%" || exit /b 1
) else (
  echo [SKIP] Game binary not found; skipping engine_discovery_tests on target binary.
)

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\discovery_progress_tests.cpp src\log.cpp /Fo:build\tests\ /Fe:build\tests\discovery_progress_tests.exe || exit /b 1
build\tests\discovery_progress_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\hook_transaction_tests.cpp /Fo:build\tests\ /Fe:build\tests\hook_transaction_tests.exe || exit /b 1
build\tests\hook_transaction_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\mod_runtime_tests.cpp src\mod_runtime.cpp /Fo:build\tests\ /Fe:build\tests\mod_runtime_tests.exe || exit /b 1
build\tests\mod_runtime_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\mod_native_ui_tests.cpp src\mod_native_ui.cpp src\mod_runtime.cpp /Fo:build\tests\ /Fe:build\tests\mod_native_ui_tests.exe || exit /b 1
build\tests\mod_native_ui_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\mod_storage_tests.cpp src\mod_storage.cpp /Fo:build\tests\ /Fe:build\tests\mod_storage_tests.exe || exit /b 1
build\tests\mod_storage_tests.exe || exit /b 1

cl /nologo /TC /std:c11 /W4 /c tests\p0_lua_c_smoke.c /Fo:build\tests\p0_lua_c_smoke.obj || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\p0_lua_backend_tests.cpp /Fo:build\tests\ /Fe:build\tests\p0_lua_backend_tests.exe || exit /b 1
build\tests\p0_lua_backend_tests.exe || exit /b 1

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\p0_demo_tests.cpp src\mod_runtime.cpp src\mod_native_ui.cpp src\mod_storage.cpp /Fo:build\tests\ /Fe:build\tests\p0_demo_tests.exe || exit /b 1
build\tests\p0_demo_tests.exe || exit /b 1

echo --- Running Python unit tests ---
where python >nul 2>&1
if %ERRORLEVEL% equ 0 (
  python -m unittest discover -s tests -p "*_tests.py" -v || exit /b 1
) else (
  echo [SKIP] Python not found in PATH; skipping python unit tests.
)

echo.
echo All available test suites passed successfully!
exit /b 0
