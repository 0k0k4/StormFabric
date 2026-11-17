@echo off
rem Builds third_party\cef\build\libcef_dll_wrapper.lib (CEF C++ wrapper, /MT, Release) with cl only.
rem Requires the CEF binary distribution extracted in third_party\cef (see examples\sf_browser\README.md).
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
where cl >nul 2>&1 || call "%VCVARS%" >nul || exit /b 1
set "CEF=%~dp0..\..\third_party\cef"
if not exist "%CEF%\include\cef_app.h" (echo ERROR: CEF not found in %CEF% & exit /b 1)
if not exist "%CEF%\build\obj" mkdir "%CEF%\build\obj"
pushd "%CEF%"
rem Quoted paths: the repository path may contain spaces.
(for /r libcef_dll %%f in (*.cc) do @echo "%%f") > build\sources.txt
set "FLAGS=/nologo /c /MP /O2 /Ob2 /MT /GR- /Gy /GF /utf-8 /std:c++20 /W3 /wd4100 /wd4244 /wd4996 /EHs-c-"
set "DEFS=/DWRAPPING_CEF_SHARED /DWIN32 /D_WIN32 /D_WINDOWS /DUNICODE /D_UNICODE /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=NTDDI_WIN10_FE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_HAS_EXCEPTIONS=0 /DNDEBUG /D_NDEBUG /D__STDC_CONSTANT_MACROS /D__STDC_FORMAT_MACROS"
cl %FLAGS% %DEFS% /I"%CEF%" /Fo:build\obj\ @build\sources.txt || (popd & exit /b 1)
lib /nologo /OUT:build\libcef_dll_wrapper.lib build\obj\*.obj || (popd & exit /b 1)
popd
echo OK: %CEF%\build\libcef_dll_wrapper.lib
