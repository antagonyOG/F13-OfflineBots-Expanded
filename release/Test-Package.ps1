param([Parameter(Mandatory=$true)][string]$PublicZip,
    [string]$SupportedExe = 'F:\Games\Friday the 13th\F13 base game\Friday the 13th The Game\SummerCamp\Binaries\Win64\SummerCamp.exe')
$ErrorActionPreference = 'Stop'
$base = 'F:\Games\Friday the 13th\.codex-temp'
$testRoot = Join-Path $base ('release-fixture-' + [guid]::NewGuid().ToString('N'))
$game = Join-Path $testRoot 'Game With Spaces'
$win64 = Join-Path $game 'SummerCamp\Binaries\Win64'
$null = New-Item -ItemType Directory -Path $win64,(Join-Path $game 'Engine'),(Join-Path $game 'SummerCamp\Content\Paks') -Force
Expand-Archive -LiteralPath $PublicZip -DestinationPath $game
$shipping = Join-Path $win64 'SummerCamp-Win64-Shipping.exe'
Copy-Item -LiteralPath $SupportedExe -Destination $shipping
[IO.File]::WriteAllText((Join-Path $game 'SummerCamp\Content\Paks\SummerCamp-WindowsNoEditor.pak'),'Synthetic install fixture only - no gameplay assets')
$script = Join-Path $game 'OfflineBots\OfflineBots-Setup.ps1'
function Require($Value,[string]$Name) { if (-not $Value) { throw "FAILED: $Name" }; Write-Host "PASS: $Name" }
function Run([string]$Mode,[int]$Expected) {
    $output = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $script -Mode $Mode -GameRoot $game -PackageRoot $game 2>&1
    $code = $LASTEXITCODE
    if ($code -ne $Expected) { $output | Write-Host; throw "$Mode returned $code; expected $Expected" }
}
$backend = Join-Path $win64 'F13BaseGameOfflineBots.dll'
$oldCmd = Join-Path $win64 'Play Offline Bots.cmd'
$loader = Join-Path $win64 'dlllist.txt'
[IO.File]::WriteAllText($backend,'Existing version placeholder')
[IO.File]::WriteAllText($oldCmd,'Existing command placeholder')
[IO.File]::WriteAllText($loader,"OnlineFix64.dll`r`nF13BaseGameOfflineBots.dll`r`nEpsteinHook.dll")
$originalHash = (Get-FileHash -LiteralPath $backend).Hash
$oldCmdHash = (Get-FileHash -LiteralPath $oldCmd).Hash
$listHash = (Get-FileHash -LiteralPath $loader).Hash
$exeHash = (Get-FileHash -LiteralPath $shipping).Hash
$unrelated = Join-Path $win64 'UnrelatedMod.keep'
[IO.File]::WriteAllText($unrelated,'Must remain')
Run Install 0
Require ((Get-FileHash -LiteralPath $shipping).Hash -eq $exeHash) 'normal shipping name/executable bytes unchanged'
Require ([IO.File]::ReadAllText($loader) -eq 'OnlineFix64.dll') 'old autoload removed; unrelated OnlineFix loader preserved'
Run Verify 0
$report = [IO.File]::ReadAllText((Join-Path $game 'F13-OfflineBots-Diagnostic.txt'))
Require ($report.Contains('INSTALL FILES: OK') -and $report.Contains('BACKEND: NOT CONFIRMED')) 'no fake runtime success after copy'
Run Install 0
Run Uninstall 0
Require ((Get-FileHash -LiteralPath $backend).Hash -eq $originalHash) 'repeat install/upgrade preserves original backend backup'
Require ((Get-FileHash -LiteralPath $oldCmd).Hash -eq $oldCmdHash) 'old command restored on uninstall'
Require ((Get-FileHash -LiteralPath $loader).Hash -eq $listHash) 'original loader bytes restored'
Require ((Test-Path -LiteralPath $unrelated) -and -not (Test-Path -LiteralPath (Join-Path $win64 'F13PostAuthAttach.exe'))) 'only owned files removed; unrelated files retained'
Run Install 0
[IO.File]::AppendAllText($oldCmd,'User modification')
Run Uninstall 1
Require (Test-Path -LiteralPath $backend) 'changed-file uninstall refuses before changing anything'
Copy-Item -LiteralPath (Join-Path $game 'OfflineBots\Play Offline Bots.cmd') -Destination $oldCmd -Force
Remove-Item -LiteralPath $backend
Run Verify 2
Require ([IO.File]::ReadAllText((Join-Path $game 'F13-OfflineBots-Diagnostic.txt')).Contains('REQUIRED MOD FILE MISSING')) 'quarantined/missing backend diagnosed'
Copy-Item -LiteralPath (Join-Path $game 'OfflineBots\F13BaseGameOfflineBots.dll') -Destination $backend
Run Uninstall 0
$manifestFile = Join-Path $game 'F13OfflineBots-Install\manifest.json'
$manifestText = [IO.File]::ReadAllText($manifestFile)
$m = $manifestText | ConvertFrom-Json; $m.Files[0].Path = '..\outside.dll'
[IO.File]::WriteAllText($manifestFile,($m | ConvertTo-Json -Depth 12))
Run Install 1
Require (-not (Test-Path -LiteralPath (Join-Path $testRoot 'outside.dll'))) 'manifest traversal refused'
[IO.File]::WriteAllText($manifestFile,$manifestText)
Copy-Item -LiteralPath $shipping -Destination (Join-Path $win64 'SummerCamp.exe')
Remove-Item -LiteralPath $shipping
Run Install 0
& (Join-Path $win64 'F13PostAuthAttach.exe') --check-files
Require ($LASTEXITCODE -eq 0) 'legacy filename launcher preflight recognizes exact hash'
Run Uninstall 0
Copy-Item -LiteralPath (Join-Path $win64 'SummerCamp.exe') -Destination $shipping
Run Install 0
& (Join-Path $win64 'F13PostAuthAttach.exe') --check-files
Require ($LASTEXITCODE -eq 0) 'normal shipping filename launcher preflight recognizes exact hash'
Run Uninstall 0
[IO.File]::WriteAllText($shipping,'Unsupported executable fixture')
Run Install 1
Require ((Get-FileHash -LiteralPath $backend).Hash -eq $originalHash) 'unsupported executable refused without replacing backend'
$payload = Join-Path $game 'OfflineBots\F13BaseGameOfflineBots.dll'
[IO.File]::WriteAllText($payload,'Damaged release payload')
Copy-Item -LiteralPath $SupportedExe -Destination $shipping -Force
Run Install 1
Require ((Get-FileHash -LiteralPath $backend).Hash -eq $originalHash) 'corrupt payload refused before installation'
Write-Host "Fixture retained for audit: $testRoot"
Write-Host 'All package fixture tests passed. These are not gameplay/vanilla/AppData tests.'
