@echo off
rem Builds build\examples\sf_browser\ (sf_browser.dll + cef\ with sf_browser_host.exe and the CEF runtime)
rem and build\examples\browser_demo\. Needs the CEF binary distribution in third_party\cef and
rem third_party\cef\build\libcef_dll_wrapper.lib (build_cef_wrapper.bat). See README.md.
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
where cl >nul 2>&1 || call "%VCVARS%" >nul || exit /b 1
set "HERE=%~dp0"
set "CEF=%HERE%..\..\third_party\cef"
set "OUT=%HERE%..\..\build\examples\sf_browser"
set "DEMO=%HERE%..\..\build\examples\browser_demo"
if not exist "%CEF%\build\libcef_dll_wrapper.lib" (
  call "%HERE%build_cef_wrapper.bat" || exit /b 1
)
if not exist "%OUT%\cef" mkdir "%OUT%\cef"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%DEMO%" mkdir "%DEMO%"

echo --- sf_browser_host.exe (CEF, own process)
cl /nologo /O2 /MT /GR- /EHs-c- /utf-8 /std:c++20 /W3 /wd4100 /wd4244 ^
   /DWIN32 /D_WIN32 /D_WINDOWS /DUNICODE /D_UNICODE /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=NTDDI_WIN10_FE ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_HAS_EXCEPTIONS=0 /DNDEBUG /D__STDC_CONSTANT_MACROS /D__STDC_FORMAT_MACROS ^
   /I"%CEF%" "%HERE%host\host.cpp" /Fo:"%OUT%\obj\\" /Fe:"%OUT%\cef\sf_browser_host.exe" ^
   /link /SUBSYSTEM:WINDOWS /STACK:0x800000 /MANIFEST:EMBED /MANIFESTINPUT:"%HERE%host\compatibility.manifest" ^
   "%CEF%\Release\libcef.lib" "%CEF%\build\libcef_dll_wrapper.lib" ^
   user32.lib gdi32.lib comctl32.lib shlwapi.lib rpcrt4.lib ws2_32.lib crypt32.lib wintrust.lib delayimp.lib || exit /b 1

echo --- sf_browser.dll (in the game)
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /LD /DNOMINMAX /DWIN32_LEAN_AND_MEAN "%HERE%mod\sf_browser.cpp" ^
   /Fo:"%OUT%\obj\\" /Fe:"%OUT%\sf_browser.dll" /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1
copy /Y "%HERE%mod.json" "%OUT%\mod.json" >nul
copy /Y "%HERE%sf_browser_api.h" "%OUT%\sf_browser_api.h" >nul

echo --- CEF runtime next to the host (never loaded by the game)
xcopy /Y /Q /E "%CEF%\Release\*.dll" "%OUT%\cef\" >nul
xcopy /Y /Q "%CEF%\Release\*.bin" "%OUT%\cef\" >nul
xcopy /Y /Q "%CEF%\Release\*.json" "%OUT%\cef\" >nul
xcopy /Y /Q /E "%CEF%\Resources\*" "%OUT%\cef\" >nul
del /Q "%OUT%\sf_browser.exp" "%OUT%\sf_browser.lib" "%OUT%\cef\sf_browser_host.exp" "%OUT%\cef\sf_browser_host.lib" >nul 2>&1

echo --- browser_demo (consumer of the service)
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /LD /DNOMINMAX /DWIN32_LEAN_AND_MEAN "%HERE%demo\browser_demo.cpp" ^
   /Fo:"%OUT%\obj\\" /Fe:"%DEMO%\sf_browser_demo.dll" /link /DLL /MANIFEST:NO /DEBUG:NONE || exit /b 1
copy /Y "%HERE%demo\mod.json" "%DEMO%\mod.json" >nul
copy /Y "%HERE%demo\demo.html" "%DEMO%\demo.html" >nul
del /Q "%DEMO%\*.exp" "%DEMO%\*.lib" >nul 2>&1
rmdir /S /Q "%OUT%\obj" >nul 2>&1
echo OK: %OUT% and %DEMO%
