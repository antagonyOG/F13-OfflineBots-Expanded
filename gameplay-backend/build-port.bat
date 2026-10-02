@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio or Build Tools with Desktop development with C++ is required.
  exit /b 1
)

set "MSBUILD="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
  echo MSBuild was not found.
  exit /b 1
)

"%MSBUILD%" "%ROOT%F13BaseGameOfflineBots.sln" /m /p:Configuration=Release /p:Platform=x64 /p:TrackFileAccess=false
if errorlevel 1 exit /b 1

echo Built: %ROOT%bin\F13BaseGameOfflineBots.dll
exit /b 0
