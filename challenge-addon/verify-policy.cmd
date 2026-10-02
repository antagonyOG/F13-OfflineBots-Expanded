@echo off
setlocal
call "F:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0"
if not exist obj mkdir obj
cl /nologo /std:c++17 /EHsc /W4 /WX /MT /Foobj\ /Feobj\ChallengeSetupPolicyTests.exe ChallengeSetupPolicy.cpp ChallengeNativeUi.cpp ChallengeRuntime.cpp ChallengeSetupPolicyTests.cpp
if errorlevel 1 (popd & exit /b 1)
obj\ChallengeSetupPolicyTests.exe
set "challengeTestResult=%errorlevel%"
popd
exit /b %challengeTestResult%
