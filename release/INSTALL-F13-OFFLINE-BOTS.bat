@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0OfflineBots\OfflineBots-Setup.ps1" -Mode Install -GameRoot "%~dp0." -PackageRoot "%~dp0."
if errorlevel 1 (echo INSTALL FAILED. Read the error above; run VERIFY-F13-OFFLINE-BOTS.bat.)
pause
