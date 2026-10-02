param(
    [ValidateSet('Install','Verify','Uninstall')][string]$Mode = 'Verify',
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [string]$PackageRoot = (Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$utf8 = New-Object Text.UTF8Encoding($false)
$supported = '941E249E4CAABF93A22FB57A6EB61D894D224C16A7464CD7451B41205436E62F'
$allowed = @('SummerCamp\Binaries\Win64\F13BaseGameOfflineBots.dll',
    'SummerCamp\Binaries\Win64\F13PostAuthAttach.exe','PLAY-F13-OFFLINE-BOTS.cmd',
    'SummerCamp\Binaries\Win64\dlllist.txt','SummerCamp\Binaries\Win64\Play Offline Bots.cmd')
function Hash([string]$Path) { if (Test-Path -LiteralPath $Path -PathType Leaf) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash } else { '' } }
function InRoot([string]$Base,[string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative)) { throw 'Absolute manifest path refused.' }
    $baseFull = [IO.Path]::GetFullPath($Base).TrimEnd('\')
    $full = [IO.Path]::GetFullPath((Join-Path $baseFull $Relative))
    if (-not $full.StartsWith($baseFull + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Path outside game/package root refused.' }
    $probe = $full
    while ($probe) {
        if (Test-Path -LiteralPath $probe) {
            if ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse-point path refused: $probe" }
        }
        $probe = Split-Path -Parent $probe
    }
    $full
}
function WriteJson([string]$Path,$Value) { [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 12),$utf8) }
function GameInfo([string]$Root) {
    $shipping = InRoot $Root 'SummerCamp\Binaries\Win64\SummerCamp-Win64-Shipping.exe'
    $legacy = InRoot $Root 'SummerCamp\Binaries\Win64\SummerCamp.exe'
    $exe = if (Test-Path -LiteralPath $shipping -PathType Leaf) { $shipping } else { $legacy }
    $sha = Hash $exe
    $arch = 'Missing/invalid'
    $profile = $false
    if ($sha) {
        $stream = [IO.File]::OpenRead($exe)
        $reader = New-Object IO.BinaryReader($stream)
        try {
            if ($reader.ReadUInt16() -eq 0x5A4D) {
                $stream.Position = 0x3C; $pe = $reader.ReadInt32()
                if ($pe -gt 0 -and $pe + 88 -lt $stream.Length) {
                    $stream.Position = $pe
                    if ($reader.ReadUInt32() -eq 0x4550) {
                        $machine = $reader.ReadUInt16(); $null = $reader.ReadUInt16(); $timestamp = $reader.ReadUInt32()
                        $stream.Position = $pe + 24; $magic = $reader.ReadUInt16()
                        $stream.Position = $pe + 80; $imageSize = $reader.ReadUInt32()
                        if ($machine -eq 0x8664 -and $magic -eq 0x20B) { $arch = 'x64' }
                        $profile = $arch -eq 'x64' -and $timestamp -eq 0x6777E461 -and $imageSize -eq 0x035C9000
                    }
                }
            }
        } finally { $reader.Dispose(); $stream.Dispose() }
    }
    $content = InRoot $Root 'SummerCamp\Content\Paks\SummerCamp-WindowsNoEditor.pak'
    $normal = (Test-Path -LiteralPath $content -PathType Leaf) -and
        (Test-Path -LiteralPath (InRoot $Root 'Engine') -PathType Container)
    $wrapper = Test-Path -LiteralPath (InRoot $Root 'SummerCamp.exe') -PathType Leaf
    $layout = if ($exe -eq $shipping) { 'Normal shipping filename' } else { 'Existing community Win64 SummerCamp.exe layout (no rename performed)' }
    [PSCustomObject]@{Root=$Root;Exe=$exe;Hash=$sha;Architecture=$arch;Supported=($sha -eq $supported -and $profile -and $normal);NormalContent=$normal;RootWrapper=$wrapper;Layout=$layout}
}
function AssertClosed {
    $running = @(Get-Process -Name 'SummerCamp','SummerCamp-Win64-Shipping','F13PostAuthAttach' -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Close all game copies and Offline Bots launchers before installing/uninstalling.' }
}
function LoadManifest {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $null }
    $m = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($m.Schema -ne 1 -or $m.GameRoot -ne $root) { throw 'Wrong-root or unsupported install manifest.' }
    $seen = @{}
    foreach ($f in $m.Files) {
        if ($allowed -notcontains $f.Path -or $seen.ContainsKey($f.Path)) { throw 'Invalid/duplicate manifest file.' }
        $seen[$f.Path] = $true; $null = InRoot $root $f.Path
        if ($f.OriginalExists) {
            if ($f.Backup -notlike 'F13OfflineBots-Install\backups\*') { throw 'Invalid backup path.' }
            $null = InRoot $root $f.Backup
        }
    }
    $m
}

$root = (Resolve-Path -LiteralPath $GameRoot).Path.TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $root 'SummerCamp\Binaries\Win64') -PathType Container)) {
    # Support extracting beside the one inner game folder, without scanning disks.
    $candidates = @(Get-ChildItem -LiteralPath $root -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'SummerCamp\Binaries\Win64') -PathType Container })
    if ($candidates.Count -ne 1) { throw 'No unique game root found. Extract into the folder containing SummerCamp and Engine.' }
    $root = $candidates[0].FullName
}
$state = InRoot $root 'F13OfflineBots-Install'
$manifestPath = InRoot $root 'F13OfflineBots-Install\manifest.json'
$transactionPath = InRoot $root 'F13OfflineBots-Install\pending-transaction.json'
$info = GameInfo $root
$manifest = LoadManifest
$config = Get-Content -LiteralPath (InRoot $PackageRoot 'OfflineBots\Release.json') -Raw | ConvertFrom-Json

if ($Mode -eq 'Verify') {
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('F13 Offline Bots diagnostic - ' + [DateTime]::UtcNow.ToString('o'))
    $lines.Add('Windows: ' + [Environment]::OSVersion.VersionString)
    $lines.Add('Game root: ' + $root); $lines.Add('Game EXE: ' + $info.Exe)
    $lines.Add('Game EXE SHA256: ' + $info.Hash); $lines.Add('Architecture: ' + $info.Architecture)
    $lines.Add('Layout: ' + $info.Layout); $lines.Add('Root SummerCamp.exe present: ' + $info.RootWrapper)
    $lines.Add('Normal game content present: ' + $info.NormalContent)
    $lines.Add('Release: ' + $config.Version)
    $lines.Add('Installed manifest version: ' + $(if ($manifest) { $manifest.Version } else { 'NONE' }))
    $filesOk = $true
    foreach ($f in $config.Files) {
        if ($allowed -notcontains $f.Destination) { throw 'Invalid release destination.' }
        $path = InRoot $root $f.Destination; $sha = Hash $path
        $ok = $sha -eq $f.Hash; if (-not $ok) { $filesOk = $false }
        $lines.Add($f.Destination + ' | SHA256=' + $sha + ' | ' + $(if ($ok) {'OK'} elseif (-not $sha) {'REQUIRED MOD FILE MISSING (possibly quarantined)'} else {'HASH MISMATCH'}))
    }
    $lines.Add('Bootstrap method: dedicated delayed launcher, no proxy DLL')
    $lines.Add('Steam: point a non-Steam shortcut to PLAY-F13-OFFLINE-BOTS.cmd. Stock Steam Play alone does not start the mod.')
    $lines.Add('OnlineFix is external, unchanged, and not distributed with this mod.')
    foreach ($name in @('OnlineFix64.dll','winmm.dll','version.dll','BaseGameOfflineBotsBootstrap.dll','ResurrectedOfflineBots.dll','ResurrectedPrivateLobbyAI.dll','EpsteinHook.dll')) {
        $path = InRoot $root ('SummerCamp\Binaries\Win64\' + $name)
        if (Test-Path -LiteralPath $path) { $lines.Add('Loader/other module: ' + $name + ' | SHA256=' + (Hash $path)) }
    }
    $listPath = InRoot $root 'SummerCamp\Binaries\Win64\dlllist.txt'
    $badLoader = $false
    if (Test-Path -LiteralPath $listPath) {
        $entries = @(Get-Content -LiteralPath $listPath | ForEach-Object {$_.Trim()} | Where-Object {$_})
        $lines.Add('Existing dlllist.txt: ' + ($entries -join ', '))
        $badLoader = @($entries | Where-Object {$_ -in @('F13BaseGameOfflineBots.dll','BaseGameOfflineBotsBootstrap.dll','ResurrectedOfflineBots.dll','ResurrectedPrivateLobbyAI.dll')}).Count -gt 0
    }
    $bootPath = Join-Path $env:TEMP 'F13-OfflineBots-Bootstrap.log'
    $runtimePath = Join-Path $env:TEMP 'F13BaseGameOfflineBots.log'
    $bootstrap = if (Test-Path -LiteralPath $bootPath) { Get-Content -LiteralPath $bootPath -Raw } else { '' }
    $backendHash = Hash (InRoot $root 'SummerCamp\Binaries\Win64\F13BaseGameOfflineBots.dll')
    $sameSession = $bootstrap.Contains('ExecutablePath=' + $info.Exe) -and
        $bootstrap.Contains('ExecutableSHA256=' + $info.Hash) -and $backendHash -and
        $bootstrap.Contains('BackendSHA256=' + $backendHash)
    $bootRan = $sameSession -and $bootstrap.Contains('BOOTSTRAP ENTRY: EXECUTED')
    $loaded = $false; $menu = $false; $runtime = @()
    if ($bootRan -and (Test-Path -LiteralPath $runtimePath)) {
        $runtime = @(Get-Content -LiteralPath $runtimePath -Tail 2000)
        $pidMatch = [regex]::Match($bootstrap,'(?m)^\d+ PID=(\d+)\r?$')
        $startMatch = [regex]::Match($bootstrap,'SessionStartUTC=([^\r\n]+)')
        if ($pidMatch.Success -and $startMatch.Success) {
            $marker = 'RELEASE SESSION: PID=' + $pidMatch.Groups[1].Value + ' | version=' + $config.Version
            $index = -1
            for ($i=0;$i -lt $runtime.Count;$i++) { if ($runtime[$i].Contains($marker)) { $index=$i } }
            $utcMatch = if ($index -ge 0) { [regex]::Match($runtime[$index], '\| utc=([^ ]+)') } else { $null }
            if ($index -ge 0 -and $utcMatch.Success -and
                [DateTimeOffset]::Parse($utcMatch.Groups[1].Value) -ge [DateTimeOffset]::Parse($startMatch.Groups[1].Value) -and
                $bootstrap.Contains('BACKEND MODULE: LOADED')) {
                $runtime = @($runtime[$index..($runtime.Count-1)])
                $loaded = $runtime[0].Contains('BACKEND: LOADED')
                $menu = @($runtime | Where-Object {$_ -match 'visibility hook installed|MENU PATCH: ACTIVE'}).Count -gt 0
            }
        }
    }
    $lines.Add('Latest matching bootstrap log:'); $lines.Add($(if ($sameSession) {$bootstrap} else {'NONE for this executable/backend hash; old or other-install logs not accepted.'}))
    $lines.Add('Recent runtime log (evidence only):'); foreach ($line in ($runtime | Select-Object -Last 120)) { $lines.Add($line) }
    $lines.Add('GAME BUILD: ' + $(if ($info.Supported) {'SUPPORTED HASH (clean Steam gameplay verification pending)'} else {'UNSUPPORTED'}))
    $lines.Add('INSTALL FILES: ' + $(if ($filesOk -and -not $badLoader -and -not (Test-Path -LiteralPath $transactionPath)) {'OK'} else {'FAILED / INCOMPLETE'}))
    $lines.Add('BOOTSTRAP: ' + $(if ($bootRan) {'EXECUTED (see failures above)'} else {'PENDING GAME START / NOT CONFIRMED'}))
    $lines.Add('BACKEND: ' + $(if ($loaded) {'LOADED'} else {'NOT CONFIRMED'}))
    $lines.Add('MENU: ' + $(if ($menu) {'HOOK ACTIVE; visual/multi-match verification required'} else {'NOT CONFIRMED'}))
    $lines.Add('SEND THIS DIAGNOSTIC FILE TO MOD AUTHOR if anything failed or is missing.')
    $report = InRoot $root 'F13-OfflineBots-Diagnostic.txt'
    [IO.File]::WriteAllLines($report,$lines,$utf8)
    Write-Host "Diagnostic written: $report"
    if (-not $filesOk -or -not $info.Supported -or $badLoader) { exit 2 }; exit 0
}

AssertClosed
if (Test-Path -LiteralPath $transactionPath) { throw 'An interrupted install is recorded. Run verifier and send the manifest/backups to the author; do not delete them.' }
if ($Mode -eq 'Uninstall') {
    if (-not $manifest -or -not $manifest.Active) { throw 'No active owned installation manifest; refusing to guess which files to remove.' }
    foreach ($f in $manifest.Files) {
        $path = InRoot $root $f.Path; $sha = Hash $path
        if ($sha -and $sha -ne $f.InstalledHash -and
            (-not $f.OriginalExists -or $sha -ne $f.OriginalHash)) { throw "File changed after installation; preserve it and stop: $path" }
        if ($f.OriginalExists -and (Hash (InRoot $root $f.Backup)) -ne $f.OriginalHash) { throw "Original backup missing or changed: $($f.Backup)" }
    }
    foreach ($f in $manifest.Files) {
        $path = InRoot $root $f.Path
        if ($f.OriginalExists) { Copy-Item -LiteralPath (InRoot $root $f.Backup) -Destination $path -Force; Write-Host "RESTORED: $($f.Path)" }
        elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path; Write-Host "REMOVED: $($f.Path) (owned file; backup history retained)" }
    }
    $manifest.Active = $false; WriteJson $manifestPath $manifest
    Write-Host 'UNINSTALLED: game executables/content/saves/OnlineFix were not changed. Original same-name files restored; backup history retained.'
    exit 0
}
if (-not $info.Supported) { throw "UNSUPPORTED GAME BUILD: $($info.Hash). No files installed. Do not rename or patch the executable." }
$plans = @()
foreach ($f in $config.Files) {
    if ($allowed -notcontains $f.Destination -or $f.Destination -eq $allowed[3]) { throw 'Invalid payload destination.' }
    $source = InRoot $PackageRoot $f.Source
    if ((Hash $source) -ne $f.Hash) { throw "Missing/corrupt release payload: $($f.Source)" }
    $plans += [PSCustomObject]@{Path=$f.Destination;Source=$source;Hash=$f.Hash;Contents=$null}
}
$loader = InRoot $root $allowed[3]
if (Test-Path -LiteralPath $loader) {
    $entries = @([IO.File]::ReadAllLines($loader) | Where-Object { $_.Trim() -and $_.Trim() -notin @('F13BaseGameOfflineBots.dll','BaseGameOfflineBotsBootstrap.dll','EpsteinHook.dll') })
    if (@($entries | Where-Object {$_.Trim() -in @('ResurrectedOfflineBots.dll','ResurrectedPrivateLobbyAI.dll')}).Count) { throw 'Conflicting Resurrected loader entry; no installation performed. Remove/disable it using that mod installer first.' }
    $clean = $entries -join "`n"
    if ([IO.File]::ReadAllText($loader) -ne $clean) {
        $plans += [PSCustomObject]@{Path=$allowed[3];Source=$null;Hash=$null;Contents=$clean}
    }
}
if ($manifest -and $manifest.Active) {
    foreach ($f in $manifest.Files) {
        if ((Hash (InRoot $root $f.Path)) -ne $f.InstalledHash) { throw "Existing install changed or quarantined: $($f.Path). Verify before upgrading." }
        if ($f.OriginalExists -and (Hash (InRoot $root $f.Backup)) -ne $f.OriginalHash) { throw 'Existing original backup is missing/corrupt; upgrade stopped.' }
    }
}
$null = New-Item -ItemType Directory -Path $state -Force
$history = 'F13OfflineBots-Install\backups\' + [guid]::NewGuid().ToString('N')
$null = New-Item -ItemType Directory -Path (InRoot $root $history)
$rollback = @(); $records = @()
if ($manifest -and $manifest.Active) { $records = @($manifest.Files) }
try {
    # Journal every target and its rollback before modifying any target.
    foreach ($plan in $plans) {
        $path = InRoot $root $plan.Path; $prior = Hash $path
        $backupRel = $history + '\' + [IO.Path]::GetFileName($path)
        if ($prior) { Copy-Item -LiteralPath $path -Destination (InRoot $root $backupRel) }
        $rollback += [PSCustomObject]@{Path=$plan.Path;Exists=[bool]$prior;Backup=$backupRel;Hash=$prior}
    }
    WriteJson $transactionPath $rollback
    foreach ($plan in $plans) {
        $path = InRoot $root $plan.Path
        $old = @($records | Where-Object {$_.Path -eq $plan.Path})
        $saved = $rollback | Where-Object {$_.Path -eq $plan.Path}
        if ($plan.Source) { Copy-Item -LiteralPath $plan.Source -Destination $path -Force }
        else { [IO.File]::WriteAllText($path,$plan.Contents,$utf8) }
        $copied = Hash $path
        if ($plan.Hash -and $copied -ne $plan.Hash) { throw "Copied hash mismatch: $($plan.Path)" }
        $record = if ($old.Count) { $old[0] } else { [PSCustomObject]@{Path=$plan.Path;InstalledHash='';OriginalExists=$saved.Exists;Backup=$saved.Backup;OriginalHash=$saved.Hash} }
        $record.InstalledHash = $copied
        $records = @($records | Where-Object {$_.Path -ne $plan.Path}) + @($record)
        Write-Host "INSTALLED: $($plan.Path) | SHA256=$copied"
        if ($saved.Exists) { Write-Host "BACKED UP: $($saved.Backup)" }
    }
    $newManifest = [PSCustomObject]@{Schema=1;Active=$true;Version=$config.Version;GameRoot=$root;GameExe=$info.Exe;GameHash=$info.Hash;Utc=[DateTime]::UtcNow.ToString('o');Bootstrap='Dedicated delayed launcher';Files=$records}
    $tempManifest = InRoot $root 'F13OfflineBots-Install\manifest.new.json'
    WriteJson $tempManifest $newManifest
    if (Test-Path -LiteralPath $manifestPath) { [IO.File]::Replace($tempManifest,$manifestPath,(InRoot $root ($history + '\prior-manifest.json'))) }
    else { [IO.File]::Move($tempManifest,$manifestPath) }
} catch {
    foreach ($saved in $rollback) {
        $path = InRoot $root $saved.Path
        if ($saved.Exists) { Copy-Item -LiteralPath (InRoot $root $saved.Backup) -Destination $path -Force }
        elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    if (Test-Path -LiteralPath $transactionPath) { Remove-Item -LiteralPath $transactionPath }
    throw
}
Remove-Item -LiteralPath $transactionPath
Write-Host 'FILES INSTALLED: OK'
Write-Host 'GAME BUILD: SUPPORTED HASH'
Write-Host 'BOOTSTRAP FILE: VERIFIED (dedicated launcher; runtime not yet tested)'
Write-Host 'BACKEND DLL: PRESENT AND HASH VERIFIED'
Write-Host 'RUNTIME LOAD: PENDING GAME START'
Write-Host 'Launch PLAY-F13-OFFLINE-BOTS.cmd, then open Offline Play. Steam shortcut instructions: README.txt.'
