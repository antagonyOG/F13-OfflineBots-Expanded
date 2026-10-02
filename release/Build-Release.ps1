param([string]$OutputRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'release-output\1.0.0-rc1'))
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$utf8 = New-Object Text.UTF8Encoding($false)
$version = '1.0.0-rc1'
$out = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $out) { throw 'Use a fresh output directory; existing release artifacts are preserved.' }
$public = Join-Path $out 'Public'; $source = Join-Path $out 'Source'
$null = New-Item -ItemType Directory -Path $public,$source
$payload = Join-Path $public 'OfflineBots'; $licenses = Join-Path $public 'LICENSES'
$null = New-Item -ItemType Directory -Path $payload,$licenses
Copy-Item -LiteralPath (Join-Path $project 'gameplay-backend\bin\F13BaseGameOfflineBots.dll') -Destination $payload
Copy-Item -LiteralPath (Join-Path $project 'gameplay-backend\postauth-attach\bin\F13PostAuthAttach.exe') -Destination $payload
Copy-Item -LiteralPath (Join-Path $project 'gameplay-backend\postauth-attach\Play Offline Bots.cmd') -Destination $payload
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PLAY-F13-OFFLINE-BOTS.cmd') -Destination (Join-Path $payload 'PLAY-F13-OFFLINE-BOTS.cmd')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'OfflineBots-Setup.ps1') -Destination $payload
foreach ($name in @('INSTALL-F13-OFFLINE-BOTS.bat','VERIFY-F13-OFFLINE-BOTS.bat',
    'UNINSTALL-F13-OFFLINE-BOTS.bat','README.txt')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $public
}
$pairs = @(
    @('F13BaseGameOfflineBots.dll','SummerCamp\Binaries\Win64\F13BaseGameOfflineBots.dll'),
    @('F13PostAuthAttach.exe','SummerCamp\Binaries\Win64\F13PostAuthAttach.exe'),
    @('Play Offline Bots.cmd','SummerCamp\Binaries\Win64\Play Offline Bots.cmd'),
    @('PLAY-F13-OFFLINE-BOTS.cmd','PLAY-F13-OFFLINE-BOTS.cmd'))
$files = @($pairs | ForEach-Object { [PSCustomObject]@{Source=('OfflineBots\' + $_[0]);Destination=$_[1];Hash=(Get-FileHash -LiteralPath (Join-Path $payload $_[0])).Hash} })
$config = [PSCustomObject]@{Version=$version;SupportedExeSHA256='941E249E4CAABF93A22FB57A6EB61D894D224C16A7464CD7451B41205436E62F';Files=$files}
[IO.File]::WriteAllText((Join-Path $payload 'Release.json'),($config | ConvertTo-Json -Depth 6),$utf8)
$licenseSource = [IO.File]::ReadAllText((Join-Path $project 'gameplay-backend\port\vendor\minhook\src\hook.c'))
$license = $licenseSource.Substring(2,$licenseSource.IndexOf('*/')-2).Trim()
[IO.File]::WriteAllText((Join-Path $licenses 'MinHook.txt'),$license,$utf8)

# Explicit active-source allowlist: no backups, assets, PAKs, game files or binaries.
foreach ($folder in @('gameplay-backend\port\src','gameplay-backend\port\vendor\minhook')) {
    $from = Join-Path $project $folder
    foreach ($f in Get-ChildItem -LiteralPath $from -Recurse -File) {
        if ($f.Extension -notin @('.cpp','.c','.hpp','.h','.inc','.inl')) { continue }
        $rel = $f.FullName.Substring($project.Length+1); $dest = Join-Path $source $rel
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force
        Copy-Item -LiteralPath $f.FullName -Destination $dest
    }
}
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $project 'challenge-addon') -File) {
    if ($f.Extension -notin @('.cpp','.hpp','.cmd')) { continue }
    $dest = Join-Path $source ('challenge-addon\' + $f.Name)
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force
    Copy-Item -LiteralPath $f.FullName -Destination $dest
}
foreach ($rel in @('gameplay-backend\F13BaseGameOfflineBots.sln','gameplay-backend\port\BaseGameOfflineBots.vcxproj',
    'gameplay-backend\build-port.bat','gameplay-backend\build-port-clean.ps1',
    'gameplay-backend\postauth-attach\PostAuthAttach.cpp','gameplay-backend\postauth-attach\build.bat',
    'gameplay-backend\postauth-attach\Play Offline Bots.cmd')) {
    $dest = Join-Path $source $rel; $null = New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force
    Copy-Item -LiteralPath (Join-Path $project $rel) -Destination $dest
}
$sourceRelease = Join-Path $source 'release'; $null = New-Item -ItemType Directory -Path $sourceRelease
Get-ChildItem -LiteralPath $PSScriptRoot -File | Where-Object {$_.Extension -in @('.ps1','.bat','.cmd','.txt')} | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $sourceRelease}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'BUILD-INSTRUCTIONS.txt') -Destination $source
Copy-Item -LiteralPath $licenses -Destination $source -Recurse
[IO.File]::WriteAllText((Join-Path $source 'SOURCE-REVISION.txt'),"Exact packaging source for $version`r`nBackend SHA256=$($files[0].Hash)`r`nLauncher SHA256=$($files[1].Hash)`r`nFrozen core SHA256=$((Get-FileHash -LiteralPath (Join-Path $project 'gameplay-backend\port\src\Game\Features\Features.cpp')).Hash)`r`n",$utf8)
foreach ($stage in @($source)) {
    $hashes = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName).Hash + '  ' + $_.FullName.Substring($stage.Length+1) })
    [IO.File]::WriteAllLines((Join-Path $stage 'CHECKSUMS-SHA256.txt'),$hashes,$utf8)
}
$publicZip = Join-Path $out "F13-OfflineBots-Complete-$version.zip"
$sourceZip = Join-Path $out "F13-OfflineBots-Complete-GitHub-Source-$version.zip"
Compress-Archive -Path (Join-Path $public '*') -DestinationPath $publicZip
Compress-Archive -Path (Join-Path $source '*') -DestinationPath $sourceZip
$hashes = @()
$hashes += (Get-FileHash -LiteralPath $publicZip).Hash + '  ' + (Split-Path -Leaf $publicZip)
$hashes += (Get-FileHash -LiteralPath $sourceZip).Hash + '  ' + (Split-Path -Leaf $sourceZip)
foreach ($f in $files | Where-Object {$_.Source -match '\.(dll|exe)$'}) { $hashes += $f.Hash + '  ' + $f.Source }
$hashes += $config.SupportedExeSHA256 + '  supported game executable (not shipped)'
[IO.File]::WriteAllLines((Join-Path $out 'CHECKSUMS-SHA256.txt'),$hashes,$utf8)
Write-Host "Candidate archives created: $out"
Get-FileHash -LiteralPath $publicZip,$sourceZip
