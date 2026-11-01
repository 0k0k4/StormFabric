@echo off
rem Offline model tests only: no game launch, injection or loader DLL build.
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
  echo ERROR: MSVC x64 environment not found. Set VCVARS to vcvars64.bat.
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0.."

if not exist build\tests mkdir build\tests
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\custom_display_model_tests.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_model_tests.exe || exit /b 1
build\tests\custom_display_model_tests.exe || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\custom_display_dialog_tests.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_dialog_tests.exe || exit /b 1
build\tests\custom_display_dialog_tests.exe || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\custom_display_dialog_text_tests.cpp src\custom_display_dialog_text.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_dialog_text_tests.exe || exit /b 1
build\tests\custom_display_dialog_text_tests.exe || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\custom_display_dialog_native_tests.cpp src\custom_display_dialog_native.cpp src\custom_display_dialog_controller.cpp src\custom_display_dialog_text.cpp src\custom_display_catalogue.cpp src\custom_display_dialog_callbacks.cpp src\custom_display_dialog_input.cpp /Fo:build\tests\ /Fe:build\tests\custom_display_dialog_native_tests.exe || exit /b 1
build\tests\custom_display_dialog_native_tests.exe || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX tests\video_model_tests.cpp /Fo:build\tests\ /Fe:build\tests\video_model_tests.exe || exit /b 1
build\tests\video_model_tests.exe || exit /b 1
