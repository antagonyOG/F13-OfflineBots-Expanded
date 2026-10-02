@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0OfflineBots\OfflineBots-Setup.ps1" -Mode Verify -GameRoot "%~dp0." -PackageRoot "%~dp0."
if errorlevel 1 (echo VERIFICATION FAILED. Send F13-OfflineBots-Diagnostic.txt and the error above.)
pause
