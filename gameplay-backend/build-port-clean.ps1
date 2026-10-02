$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildScript = Join-Path $projectRoot 'build-port.bat'
$buildTemp = Join-Path $env:LOCALAPPDATA 'Temp'

$command = 'set "SystemRoot=' + $env:SystemRoot +
    '" && set "windir=' + $env:SystemRoot +
    '" && set "ProgramFiles=' + $env:ProgramFiles +
    '" && set "ProgramFiles(x86)=' + ${env:ProgramFiles(x86)} +
    '" && set "CommonProgramFiles=' + $env:CommonProgramFiles +
    '" && set "CommonProgramFiles(x86)=' + ${env:CommonProgramFiles(x86)} +
    '" && set "CommonProgramW6432=' + $env:CommonProgramW6432 +
    '" && set "ProgramData=' + $env:ProgramData +
    '" && set "ALLUSERSPROFILE=' + $env:ALLUSERSPROFILE +
    '" && set "LOCALAPPDATA=' + $env:LOCALAPPDATA +
    '" && set "APPDATA=' + $env:APPDATA +
    '" && set "USERPROFILE=' + $env:USERPROFILE +
    '" && set "HOMEDRIVE=' + $env:HOMEDRIVE +
    '" && set "HOMEPATH=' + $env:HOMEPATH +
    '" && set "PUBLIC=' + $env:PUBLIC +
    '" && set "SystemDrive=' + $env:SystemDrive +
    '" && set "PATH=' + $env:SystemRoot + '\System32"' +
    ' && set "TEMP=' + $buildTemp +
    '" && set "TMP=' + $buildTemp +
    '" && call "' + $buildScript + '"'

$process = Start-Process `
    -FilePath "$env:SystemRoot\System32\cmd.exe" `
    -ArgumentList @('/d', '/c', $command) `
    -WorkingDirectory $projectRoot `
    -NoNewWindow `
    -UseNewEnvironment `
    -PassThru

$process.WaitForExit()
exit $process.ExitCode
