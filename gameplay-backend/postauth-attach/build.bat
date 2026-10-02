@echo off
setlocal EnableExtensions

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
set "VSDEVCMD="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -find Common7\Tools\VsDevCmd.bat`) do set "VSDEVCMD=%%i"
if not defined VSDEVCMD exit /b 1
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1

if not exist "%~dp0bin" mkdir "%~dp0bin"
cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /DUNICODE /D_UNICODE "%~dp0PostAuthAttach.cpp" /Fo"%~dp0bin\PostAuthAttach.obj" /Fe"%~dp0bin\F13PostAuthAttach.exe"
exit /b %errorlevel%
