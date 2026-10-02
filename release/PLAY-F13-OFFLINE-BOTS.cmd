@echo off
if not exist "%~dp0SummerCamp\Binaries\Win64\F13PostAuthAttach.exe" (
  echo Offline Bots launcher missing. Run INSTALL-F13-OFFLINE-BOTS.bat first.
  pause
  exit /b 1
)
start "" "%~dp0SummerCamp\Binaries\Win64\F13PostAuthAttach.exe" --auto-launch
