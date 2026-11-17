@echo off
rem Builds and runs the sf_browser end-to-end test (needs a desktop session and OpenGL 3.3).
rem Run examples\sf_browser\build.bat first.
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
where cl >nul 2>&1 || call "%VCVARS%" >nul || exit /b 1
set "HERE=%~dp0"
set "ROOT=%HERE%..\..\.."
set "OUT=%ROOT%\build\examples\sf_browser"
if not exist "%OUT%\sf_browser.dll" (echo ERROR: run examples\sf_browser\build.bat first & exit /b 1)
if not exist "%ROOT%\build\tests" mkdir "%ROOT%\build\tests"
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN "%HERE%e2e_test.cpp" ^
   "%ROOT%\src\mod_image.cpp" "%ROOT%\src\mod_runtime.cpp" "%ROOT%\src\mod_services.cpp" ^
   /Fo:"%ROOT%\build\tests\\" /Fe:"%ROOT%\build\tests\sf_browser_e2e.exe" || exit /b 1
"%ROOT%\build\tests\sf_browser_e2e.exe" "%OUT%"
