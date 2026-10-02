@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0OfflineBots\OfflineBots-Setup.ps1" -Mode Uninstall -GameRoot "%~dp0." -PackageRoot "%~dp0."
if errorlevel 1 (echo UNINSTALL STOPPED. Changed files and saves are preserved; read the error above.)
pause
