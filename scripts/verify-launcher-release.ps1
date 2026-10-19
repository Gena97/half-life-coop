[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [switch]$UpdateSignatures,
    [ValidateRange(1, 72)][int]$MaximumSignatureAgeHours = 24,
    [switch]$RequireAuthenticode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'update-signing.ps1')
. (Join-Path $PSScriptRoot 'read-launcher-resource.ps1')
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $repoRoot 'artifacts' }
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$reportPath = Join-Path $outputPath 'release-verification.json'
$started = [DateTime]::UtcNow
$report = [ordered]@{ schemaVersion = 1; status = 'failed'; generatedAtUtc = $started.ToString('o'); assets = @() }

function Get-CoopDefenderState
{
    param([object]$Status)

    return [ordered]@{
        serviceEnabled = [bool]$Status.AMServiceEnabled
        antivirusEnabled = [bool]$Status.AntivirusEnabled
        realtimeEnabled = [bool]$Status.RealTimeProtectionEnabled
        runningMode = [string]$Status.AMRunningMode
        engineVersion = [string]$Status.AMEngineVersion
        signatureVersion = [string]$Status.AntivirusSignatureVersion
    }
}

function Assert-CoopDefenderReady
{
    param([object]$Status)

    if (!$Status.AMServiceEnabled -or !$Status.AntivirusEnabled -or
        !$Status.RealTimeProtectionEnabled -or $Status.AMRunningMode -ne 'Normal' -or
        [string]::IsNullOrWhiteSpace([string]$Status.AMEngineVersion) -or $Status.AMEngineVersion -eq '0.0.0.0')
    {
        $details = Get-CoopDefenderState -Status $Status | ConvertTo-Json -Compress
        throw "Публикация запрещена: Microsoft Defender не готов к проверке. Состояние: $details"
    }
}

try
{
    $launcherPath = Join-Path $outputPath 'HLCoopLauncher.exe'
    $manifest = Assert-CoopLauncherUpdateSignature -ManifestPath (Join-Path $outputPath 'launcher-update.json') -SignaturePath (Join-Path $outputPath 'launcher-update.sig') -LauncherPath $launcherPath
    $versionInfo = (Get-Item -LiteralPath $launcherPath).VersionInfo
    if ($versionInfo.CompanyName -cne 'HalfLifeCoop' -or $versionInfo.ProductName -cne 'Half-Life Co-op' -or $versionInfo.FileVersion -cne ($manifest.version + '.0'))
    {
        throw 'Ресурсы версии EXE не соответствуют запускателю проекта'
    }
    $expectedRepository = (Get-Content -LiteralPath (Join-Path $repoRoot 'game\hlcoop\update.json') -Raw -Encoding UTF8 | ConvertFrom-Json).repository
    if ($manifest.repository -cne $expectedRepository) { throw 'Репозиторий подписанного манифеста не совпал с исходниками' }
    $archiveName = "hlcoop-$($manifest.version)-win32.zip"
    $names = @('HLCoopLauncher.exe', 'HLCoopLauncher.exe.sha256', 'launcher-update.json', 'launcher-update.sig', $archiveName, "$archiveName.sha256")
    foreach ($name in $names)
    {
        $path = Join-Path $outputPath $name
        $file = Get-Item -LiteralPath $path -Force
        if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Файл релиза не должен быть ссылкой' }
        $report.assets += [ordered]@{ file = $name; bytes = $file.Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    foreach ($name in @('HLCoopLauncher.exe', $archiveName))
    {
        $expected = (Get-Content -LiteralPath (Join-Path $outputPath "$name.sha256") -Raw -Encoding ASCII).Trim()
        $actual = ($report.assets | Where-Object { $_.file -ceq $name }).sha256
        if ($expected -cne "$actual  $name") { throw "Контрольная сумма не совпала: $name" }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $outputPath $archiveName))
    $zipFiles = @{}
    try
    {
        $entry = $archive.GetEntry('hlcoop/update-manifest.json')
        if ($null -eq $entry -or $entry.Length -gt 262144 -or $archive.Entries.Count -ne 25)
        {
            throw 'Некорректный совместимый ZIP мода'
        }
        $manifestReader = New-Object IO.StreamReader($entry.Open(), [Text.Encoding]::UTF8, $true)
        try { $zipManifest = $manifestReader.ReadToEnd() | ConvertFrom-Json }
        finally { $manifestReader.Dispose() }
        if ($zipManifest.schemaVersion -ne 1 -or $zipManifest.version -cne $manifest.version -or $zipManifest.files.Count -ne 24)
        {
            throw 'Манифест ZIP не соответствует релизу'
        }
        foreach ($file in $zipManifest.files)
        {
            if ($file.path -match '(^|/)\.\.(/|$)|[\\:]' -or $file.path.StartsWith('/') -or $zipFiles.ContainsKey([string]$file.path))
            {
                throw 'Некорректный путь в манифесте ZIP'
            }
            $entry = $archive.GetEntry('hlcoop/' + $file.path)
            if ($null -eq $entry -or $entry.Length -ne $file.bytes -or $entry.Length -gt 67108864)
            {
                throw 'Файл ZIP отсутствует или не совпал по размеру'
            }
            $hashAlgorithm = [Security.Cryptography.SHA256]::Create()
            $entryStream = $entry.Open()
            try { $fileHash = [BitConverter]::ToString($hashAlgorithm.ComputeHash($entryStream)).Replace('-', '').ToLowerInvariant() }
            finally { $entryStream.Dispose(); $hashAlgorithm.Dispose() }
            if ($fileHash -cne $file.sha256) { throw 'SHA256 файла ZIP не совпал с манифестом' }
            $zipFiles[[string]$file.path] = $file
        }
        $zipNames = @{}
        foreach ($entry in $archive.Entries)
        {
            if ($zipNames.ContainsKey($entry.FullName) -or
                ($entry.FullName -cne 'hlcoop/update-manifest.json' -and
                 (!$entry.FullName.StartsWith('hlcoop/', [StringComparison]::Ordinal) -or !$zipFiles.ContainsKey($entry.FullName.Substring(7)))))
            {
                throw 'ZIP содержит повторяющийся или лишний файл'
            }
            $zipNames[$entry.FullName] = $true
        }
    }
    finally { $archive.Dispose() }
    $stream = [IO.File]::OpenRead($launcherPath)
    $reader = New-Object IO.BinaryReader($stream)
    try
    {
        if ($reader.ReadUInt16() -ne 0x5a4d -or $stream.Length -lt 4096) { throw 'Нет заголовка EXE' }
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 26) { throw 'Повреждён заголовок PE' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c) { throw 'Для релиза требуется Win32 PE' }
        $stream.Position = $offset + 22
        $flags = $reader.ReadUInt16()
        if (($flags -band 0x2000) -ne 0 -or ($flags -band 2) -eq 0 -or $reader.ReadUInt16() -ne 0x10b) { throw 'Получен DLL вместо PE32 EXE' }
    }
    finally { $reader.Dispose() }
    $encoding = New-Object Text.UTF8Encoding($false, $true)
    $cabinet = [HalfLifeCoopBuild.ResourceReader]::Read($launcherPath, 101)
    if ([Text.Encoding]::ASCII.GetString($cabinet, 0, 4) -cne 'MSCF') { throw 'В EXE отсутствует CAB' }
    $payloadBytes = [HalfLifeCoopBuild.ResourceReader]::Read($launcherPath, 102)
    $buildPath = Join-Path $repoRoot 'build\Launcher\Release'
    if ([Convert]::ToBase64String($cabinet) -cne [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $buildPath 'payload.cab'))) -or
        [Convert]::ToBase64String($payloadBytes) -cne [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $buildPath 'payload.json'))))
    {
        throw 'Ресурсы EXE не совпали с файлами проверенной сборки'
    }
    $payload = $encoding.GetString($payloadBytes) | ConvertFrom-Json
    if ($payload.schemaVersion -ne 2 -or $payload.version -cne $manifest.version -or $payload.files.Count -ne 21)
    {
        throw 'Неверный манифест встроенного CAB'
    }
    $allowedPaths = @('autoexec.cfg', 'campaign.cfg', 'campaign.json', 'cl_dlls/client.dll', 'continueauto.cfg', 'continuecoop.cfg', 'coop.cfg', 'CSBotConfig.vdf', 'dlls/hl.dll', 'game.cfg', 'liblist.gam', 'LICENSE', 'listenserver.cfg', 'mapcycle.txt', 'motd.txt', 'newcampaign.cfg', 'README.txt', 'resource/CreateMultiplayerGameServerPage.res', 'resource/GameMenu.res', 'server.cfg', 'settings.scr')
    $allowedCabinetNames = @(0..20 | ForEach-Object { 'f{0:d4}' -f $_ })
    $seen = @{}
    $seenCabinetNames = @{}
    foreach ($file in $payload.files)
    {
        if ($file.path -cnotin $allowedPaths -or !$zipFiles.ContainsKey([string]$file.path) -or
            $file.sha256 -cne $zipFiles[[string]$file.path].sha256 -or $file.bytes -ne $zipFiles[[string]$file.path].bytes -or
            $file.path -match '(?i)(\.ps1|\.cmd|\.bat)$|(^|/)\.\.(/|$)|[\\:]' -or
            $file.path -in @('update.json', 'update-manifest.json') -or $file.path.StartsWith('/') -or
            $seen.ContainsKey([string]$file.path) -or $seenCabinetNames.ContainsKey([string]$file.cabinetName) -or $file.cabinetName -cnotin $allowedCabinetNames -or
            $file.sha256 -cnotmatch '^[a-f0-9]{64}$' -or $file.bytes -le 0 -or $file.bytes -gt 67108864)
        {
            throw 'Встроенный CAB содержит недопустимый путь или файл'
        }
        $seen[[string]$file.path] = $true
        $seenCabinetNames[[string]$file.cabinetName] = $true
    }
    $embeddedKey = $encoding.GetString([HalfLifeCoopBuild.ResourceReader]::Read($launcherPath, 103)) | ConvertFrom-Json
    $public = Get-CoopUpdatePublicKey
    if ($embeddedKey.keyId -cne $public.keyId -or $embeddedKey.modulus -cne $public.modulus -or $embeddedKey.exponent -cne $public.exponent -or $embeddedKey.algorithm -cne $public.algorithm)
    {
        throw 'В EXE встроен другой открытый ключ'
    }
    $license = [HalfLifeCoopBuild.ResourceReader]::Read($launcherPath, 104)
    if ([Convert]::ToBase64String($license) -cne [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $repoRoot 'launcher\vendor\json.LICENSE.MIT'))))
    {
        throw 'В EXE отсутствует лицензия JSON-библиотеки'
    }
    $authenticode = Get-AuthenticodeSignature -LiteralPath $launcherPath
    if (($RequireAuthenticode -and $authenticode.Status -ne 'Valid') -or $authenticode.Status -notin @('Valid', 'NotSigned'))
    {
        throw 'Подпись Authenticode отсутствует или недействительна'
    }
    $report.authenticode = [string]$authenticode.Status
    $status = Get-MpComputerStatus -ErrorAction Stop
    $report.defenderBeforeScan = Get-CoopDefenderState -Status $status
    Write-Host ('Microsoft Defender: ' + ($report.defenderBeforeScan | ConvertTo-Json -Compress))
    Assert-CoopDefenderReady -Status $status
    if ($UpdateSignatures)
    {
        Update-MpSignature -UpdateSource MMPC -ErrorAction Stop
        $status = Get-MpComputerStatus -ErrorAction Stop
        Assert-CoopDefenderReady -Status $status
    }
    if (([DateTime]::UtcNow - $status.AntivirusSignatureLastUpdated.ToUniversalTime()).TotalHours -gt $MaximumSignatureAgeHours)
    {
        throw 'Публикация запрещена: базы Microsoft Defender устарели'
    }
    $platformRoot = Join-Path $env:ProgramData 'Microsoft\Windows Defender\Platform'
    $candidates = @()
    if (Test-Path -LiteralPath $platformRoot)
    {
        $candidates = @(Get-ChildItem -LiteralPath $platformRoot -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'MpCmdRun.exe' } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    }
    $candidates += Join-Path $env:ProgramFiles 'Windows Defender\MpCmdRun.exe'
    $mpcmdrun = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($mpcmdrun)) { throw 'Не найден MpCmdRun.exe' }
    foreach ($name in @('HLCoopLauncher.exe', $archiveName))
    {
        Write-Host "Microsoft Defender: проверка $name"
        # Этот режим проверяет также исключённые пути и архивы, обнаружение не лечится и блокирует выпуск
        & $mpcmdrun -Scan -ScanType 3 -DisableRemediation -File (Join-Path $outputPath $name) | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Microsoft Defender не подтвердил чистый результат: $name" }
    }
    $detections = @(Get-MpThreatDetection -ErrorAction Stop | Where-Object {
        $detection = $_
        @($report.assets | Where-Object {
            $assetPath = Join-Path $outputPath $_.file
            @($detection.Resources | Where-Object { $_.IndexOf($assetPath, [StringComparison]::OrdinalIgnoreCase) -ge 0 }).Count -gt 0
        }).Count -gt 0 -and ($detection.InitialDetectionTime.ToUniversalTime() -ge $started -or $detection.LastThreatStatusChangeTime.ToUniversalTime() -ge $started)
    })
    if ($detections.Count -gt 0) { throw 'Microsoft Defender зарегистрировал обнаружение для файлов релиза' }
    foreach ($asset in $report.assets)
    {
        $path = Join-Path $outputPath $asset.file
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $asset.sha256 -or
            (Get-Item -LiteralPath $path).Length -ne $asset.bytes)
        {
            throw 'Файлы релиза изменились или были удалены во время проверки'
        }
    }
    $finalStatus = Get-MpComputerStatus -ErrorAction Stop
    $report.defenderAfterScan = Get-CoopDefenderState -Status $finalStatus
    Assert-CoopDefenderReady -Status $finalStatus
    $report.defender = [ordered]@{ signatureVersion = $status.AntivirusSignatureVersion; signatureUpdatedUtc = $status.AntivirusSignatureLastUpdated.ToUniversalTime().ToString('o'); customScanPassed = $true; scanMode = 'custom-ignore-exclusions-no-remediation' }
    $report.version = $manifest.version
    $report.keyId = $public.keyId
    $sourceCommit = & git -C $repoRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Не удалось определить исходный коммит' }
    $report.sourceCommit = $sourceCommit.Trim()
    $sourceChanges = @(& git -C $repoRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) { throw 'Не удалось проверить состояние исходников' }
    $report.sourceDirty = $sourceChanges.Count -gt 0
    $report.status = 'passed'
    Write-Host 'Проверка релиза пройдена'
}
catch
{
    $report.error = $_.Exception.Message
    throw
}
finally
{
    $report.generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 8) + "`n", (New-Object Text.UTF8Encoding($false)))
}
