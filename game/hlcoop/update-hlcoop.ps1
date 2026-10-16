[CmdletBinding()]
param(
    [switch]$Launch,
    [switch]$CheckOnly,
    [switch]$SkipUpdate,
    [switch]$LibraryOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$modPath = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$gamePath = Split-Path -Parent $modPath
$journalPath = Join-Path $modPath '.hlcoop-update-transaction.json'
$script:blockLaunch = $false
$workPath = $null
$keepWork = $false
$mutex = $null
$mutexOwned = $false
$exitCode = 0
$allowedFiles = @(
    'autoexec.cfg', 'campaign.cfg', 'campaign.json', 'cl_dlls/client.dll',
    'continueauto.cfg', 'continuecoop.cfg', 'coop.cfg', 'CSBotConfig.vdf',
    'dlls/hl.dll', 'game.cfg', 'liblist.gam', 'LICENSE', 'listenserver.cfg',
    'mapcycle.txt', 'motd.txt', 'newcampaign.cfg', 'README.txt',
    'resource/CreateMultiplayerGameServerPage.res', 'resource/GameMenu.res',
    'server.cfg', 'settings.scr', 'start-hlcoop.cmd', 'update-hlcoop.ps1',
    'update.json', 'update-manifest.json'
)
$requiredFiles = @(
    'liblist.gam', 'dlls/hl.dll', 'cl_dlls/client.dll', 'coop.cfg',
    'start-hlcoop.cmd', 'update-hlcoop.ps1', 'update.json'
)

function Get-PropertyValue
{
    param([object]$Object, [string]$Name, [object]$Default = $null)

    if ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name])
    {
        return $Object.PSObject.Properties[$Name].Value
    }
    return $Default
}

function Assert-PlainPath
{
    param([string]$Path, [string]$Boundary)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $rootPath = [System.IO.Path]::GetFullPath($Boundary).TrimEnd('\')
    if (!$fullPath.Equals($rootPath, [System.StringComparison]::OrdinalIgnoreCase) -and
        !$fullPath.StartsWith($rootPath + '\', [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "Путь выходит за пределы разрешённой папки: $fullPath"
    }

    # Проверяем также предков, чтобы не работать через junction или символические ссылки
    $currentPath = $fullPath
    while (![string]::IsNullOrWhiteSpace($currentPath))
    {
        if (Test-Path -LiteralPath $currentPath)
        {
            $item = Get-Item -LiteralPath $currentPath -Force
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                throw "Обновление через ссылку или junction запрещено: $currentPath"
            }
        }
        $parentPath = Split-Path -Parent $currentPath
        if ([string]::IsNullOrWhiteSpace($parentPath) -or $parentPath -eq $currentPath)
        {
            break
        }
        $currentPath = $parentPath
    }
    return $fullPath
}

function Get-RelativeFilePath
{
    param([object]$Path)

    if ($Path -isnot [string] -or [string]::IsNullOrWhiteSpace($Path) -or
        $Path.Length -gt 180 -or $Path.Contains('\') -or $Path.Contains(':') -or
        $Path.StartsWith('/') -or $Path.EndsWith('/'))
    {
        throw 'Недопустимый относительный путь в пакете'
    }
    foreach ($part in $Path.Split('/'))
    {
        if ($part -in @('', '.', '..') -or $part -match '[\x00-\x1f<>"|?*]' -or
            $part.EndsWith(' ') -or $part.EndsWith('.') -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)')
        {
            throw 'Недопустимый компонент пути в пакете'
        }
    }
    if ($allowedFiles -notcontains $Path)
    {
        throw "Пакет содержит неподдерживаемый файл: $Path"
    }
    return $Path
}

function Read-CoopVersion
{
    param([string]$Path)

    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Get-Item -LiteralPath $Path).Length -gt 16384)
    {
        throw 'Не найден корректный liblist.gam'
    }
    $metadata = [System.IO.File]::ReadAllText($Path)
    if ($metadata -notmatch '(?m)^game\s+"Half-Life Co-op"\s*$' -or
        $metadata -notmatch '(?m)^gamedll\s+"dlls[\\/]hl\.dll"\s*$' -or
        $metadata -notmatch '(?m)^fallback_dir\s+"valve"\s*$')
    {
        throw 'Папка не распознана как Half-Life Co-op'
    }
    $matches = [regex]::Matches($metadata, '(?m)^version\s+"([0-9]+\.[0-9]+\.[0-9]+)"\s*$')
    if ($matches.Count -ne 1)
    {
        throw 'В liblist.gam не найдена однозначная версия мода'
    }
    return [version]$matches[0].Groups[1].Value
}

function Assert-X86Dll
{
    param([string]$Path)

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    $reader = New-Object System.IO.BinaryReader($stream)
    try
    {
        if ($stream.Length -lt 4096 -or $reader.ReadUInt16() -ne 0x5a4d)
        {
            throw 'Не найдена DLL в формате PE'
        }
        $stream.Position = 0x3c
        $headerOffset = $reader.ReadInt32()
        if ($headerOffset -lt 64 -or $headerOffset -gt ($stream.Length - 26))
        {
            throw 'Повреждён заголовок DLL'
        }
        $stream.Position = $headerOffset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x014c)
        {
            throw 'Для мода требуется 32-разрядная DLL x86'
        }
        $stream.Position = $headerOffset + 22
        if (($reader.ReadUInt16() -band 0x2000) -eq 0 -or $reader.ReadUInt16() -ne 0x010b)
        {
            throw 'Файл не является DLL PE32'
        }
    }
    finally
    {
        $reader.Dispose()
    }
}

function Assert-InstalledMod
{
    if (!(Split-Path -Leaf $modPath).Equals('hlcoop', [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw 'Обновлятор должен находиться в папке hlcoop'
    }
    [void](Assert-PlainPath -Path $modPath -Boundary $modPath)
    $enginePath = Join-Path $gamePath 'hl.exe'
    [void](Assert-PlainPath -Path $enginePath -Boundary $gamePath)
    if (!(Test-Path -LiteralPath $enginePath -PathType Leaf))
    {
        throw 'Не найден hl.exe рядом с папкой hlcoop: исходный пакет обновлять нельзя'
    }
    foreach ($relativePath in @('liblist.gam', 'dlls/hl.dll', 'cl_dlls/client.dll'))
    {
        [void](Assert-PlainPath -Path (Join-Path $modPath $relativePath) -Boundary $modPath)
    }
    $installedVersion = Read-CoopVersion -Path (Join-Path $modPath 'liblist.gam')
    Assert-X86Dll -Path (Join-Path $modPath 'dlls/hl.dll')
    Assert-X86Dll -Path (Join-Path $modPath 'cl_dlls/client.dll')

    # Старые ручные установки узнаём по описанию мода и двум обязательным DLL
    $manifestPath = Join-Path $modPath 'update-manifest.json'
    [void](Assert-PlainPath -Path $manifestPath -Boundary $modPath)
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf)
    {
        if ((Get-Item -LiteralPath $manifestPath).Length -gt 262144)
        {
            throw 'Слишком большой манифест установленного мода'
        }
        $manifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
        if ((Get-PropertyValue $manifest 'schemaVersion') -ne 1 -or
            (Get-PropertyValue $manifest 'version') -ne $installedVersion.ToString())
        {
            throw 'Манифест не соответствует установленной версии мода'
        }
        foreach ($relativePath in @('liblist.gam', 'dlls/hl.dll', 'cl_dlls/client.dll'))
        {
            $entries = @(Get-PropertyValue $manifest 'files' | Where-Object {
                (Get-PropertyValue $_ 'path') -eq $relativePath
            })
            if ($entries.Count -ne 1 -or
                (Get-PropertyValue $entries[0] 'sha256') -notmatch '^[a-fA-F0-9]{64}$' -or
                (Get-FileHash -LiteralPath (Join-Path $modPath $relativePath) -Algorithm SHA256).Hash -ne
                    (Get-PropertyValue $entries[0] 'sha256'))
            {
                throw "Установленный файл не соответствует манифесту: $relativePath"
            }
        }
    }
    return $installedVersion
}

function Assert-GameStopped
{
    # Не завершаем игру и не обновляем DLL, если сведения о процессе недоступны
    try
    {
        $processes = @(Get-CimInstance Win32_Process -Filter "Name = 'hl.exe' OR Name = 'hlds.exe'")
    }
    catch
    {
        throw 'Нельзя проверить запущенные процессы Half-Life: обновление отложено'
    }
    foreach ($process in $processes)
    {
        if ([string]::IsNullOrWhiteSpace($process.ExecutablePath) -or
            [string]::IsNullOrWhiteSpace($process.CommandLine))
        {
            throw 'Не удалось определить папку запущенной Half-Life: обновление отложено'
        }
        $processGamePath = Split-Path -Parent ([System.IO.Path]::GetFullPath($process.ExecutablePath))
        if ($processGamePath.Equals($gamePath, [System.StringComparison]::OrdinalIgnoreCase) -or
            $process.CommandLine.IndexOf($modPath, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $process.CommandLine -match '(?i)(?:^|\s)-game\s+"?hlcoop(?:"|\s|$)')
        {
            throw 'Half-Life или её сервер уже работает: обновление будет установлено после закрытия игры'
        }
    }
}

function Assert-HttpsUri
{
    param([uri]$Uri)

    if (!$Uri.IsAbsoluteUri -or $Uri.Scheme -ne 'https' -or !$Uri.IsDefaultPort -or
        ![string]::IsNullOrEmpty($Uri.UserInfo) -or
        $Uri.Host -notin @('api.github.com', 'github.com', 'objects.githubusercontent.com',
            'release-assets.githubusercontent.com', 'github-releases.githubusercontent.com'))
    {
        throw 'Загрузка разрешена только с HTTPS-серверов GitHub'
    }
}

function Invoke-GitHubDownload
{
    param([uri]$Uri, [string]$Path, [long]$MaximumBytes, [int]$TimeoutSeconds)

    $currentUri = $Uri
    $response = $null
    $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
    try
    {
        for ($redirect = 0; $redirect -le 5; $redirect++)
        {
            Assert-HttpsUri -Uri $currentUri
            $remaining = ($TimeoutSeconds * 1000) - [int]$elapsed.ElapsedMilliseconds
            if ($remaining -le 0)
            {
                throw 'Истекло время ожидания GitHub'
            }
            $request = [System.Net.HttpWebRequest]::Create($currentUri)
            $request.Method = 'GET'
            $request.UserAgent = 'Half-Life-Coop-Updater/1'
            $request.Accept = 'application/vnd.github+json'
            $request.Headers['X-GitHub-Api-Version'] = '2022-11-28'
            $request.AllowAutoRedirect = $false
            $request.Timeout = $remaining
            $request.ReadWriteTimeout = $remaining
            $response = $request.GetResponse()
            $status = [int]$response.StatusCode
            if ($status -ge 300 -and $status -lt 400)
            {
                $location = $response.Headers['Location']
                if ([string]::IsNullOrWhiteSpace($location) -or $redirect -eq 5)
                {
                    throw 'Некорректное перенаправление GitHub'
                }
                $currentUri = New-Object System.Uri($currentUri, $location)
                $response.Dispose()
                $response = $null
                continue
            }
            if ($status -ne 200 -or $response.ContentLength -gt $MaximumBytes)
            {
                throw 'GitHub вернул неподдерживаемый или слишком большой ответ'
            }

            $inputStream = $response.GetResponseStream()
            $outputStream = $null
            $memory = $null
            try
            {
                if ([string]::IsNullOrWhiteSpace($Path))
                {
                    $memory = New-Object System.IO.MemoryStream
                    $outputStream = $memory
                }
                else
                {
                    $outputStream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
                        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
                }
                $buffer = New-Object byte[] 65536
                [long]$totalBytes = 0
                while (($readBytes = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0)
                {
                    $totalBytes += $readBytes
                    if ($totalBytes -gt $MaximumBytes -or $elapsed.Elapsed.TotalSeconds -gt $TimeoutSeconds)
                    {
                        throw 'Превышен размер загрузки или время ожидания GitHub'
                    }
                    $outputStream.Write($buffer, 0, $readBytes)
                }
                if ($null -ne $memory)
                {
                    return [System.Text.Encoding]::UTF8.GetString($memory.ToArray())
                }
                return
            }
            finally
            {
                if ($null -ne $outputStream) { $outputStream.Dispose() }
                $inputStream.Dispose()
            }
        }
    }
    finally
    {
        if ($null -ne $response) { $response.Dispose() }
        $elapsed.Stop()
    }
}

function Get-ReleaseAsset
{
    param([object]$Release, [string]$Name, [string]$Repository, [string]$Tag)

    $assets = @(Get-PropertyValue $Release 'assets' | Where-Object {
        (Get-PropertyValue $_ 'name') -ceq $Name
    })
    if ($assets.Count -ne 1 -or (Get-PropertyValue $assets[0] 'state') -ne 'uploaded')
    {
        throw "В GitHub Release не найден однозначный готовый файл $Name"
    }
    $downloadUri = [uri](Get-PropertyValue $assets[0] 'browser_download_url')
    Assert-HttpsUri -Uri $downloadUri
    $pathPrefix = "/$Repository/releases/download/"
    $downloadPath = [uri]::UnescapeDataString($downloadUri.AbsolutePath)
    if ($downloadUri.Host -ne 'github.com' -or
        !$downloadPath.StartsWith($pathPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        $downloadPath.Substring($pathPrefix.Length) -cne "$Tag/$Name" -or
        ![string]::IsNullOrEmpty($downloadUri.Query) -or ![string]::IsNullOrEmpty($downloadUri.Fragment))
    {
        throw 'Адрес файла не соответствует выбранному GitHub Release'
    }
    return $assets[0]
}

function Expand-VerifiedPackage
{
    param([string]$ArchivePath, [string]$StagePath, [string]$Version)

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stream = [System.IO.File]::OpenRead($ArchivePath)
    $archive = $null
    try
    {
        $archive = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Read)
        if ($archive.Entries.Count -lt 8 -or $archive.Entries.Count -gt 512)
        {
            throw 'Недопустимое число файлов в архиве'
        }
        $entries = New-Object 'System.Collections.Generic.Dictionary[string,object]' ([System.StringComparer]::OrdinalIgnoreCase)
        $allNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
        [long]$totalBytes = 0
        foreach ($entry in $archive.Entries)
        {
            if (!$allNames.Add($entry.FullName) -or $entry.FullName.Contains('\') -or
                !$entry.FullName.StartsWith('hlcoop/', [System.StringComparison]::Ordinal) -or
                (($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000 -or
                ($entry.ExternalAttributes -band 0x400) -ne 0)
            {
                throw 'В архиве обнаружен повторный, внешний или ссылочный путь'
            }
            if ($entry.FullName.EndsWith('/'))
            {
                if ($entry.FullName -notin @('hlcoop/', 'hlcoop/dlls/', 'hlcoop/cl_dlls/', 'hlcoop/resource/') -or
                    $entry.Length -ne 0)
                {
                    throw 'Недопустимая папка в архиве'
                }
                continue
            }
            $relativePath = Get-RelativeFilePath -Path $entry.FullName.Substring(7)
            if ($entry.Length -lt 0 -or $entry.Length -gt 134217728 -or
                ($entry.Length -gt 0 -and $entry.CompressedLength -eq 0) -or
                ($entry.CompressedLength -gt 0 -and $entry.Length / $entry.CompressedLength -gt 1000))
            {
                throw "Недопустимый размер или сжатие файла: $relativePath"
            }
            $totalBytes += $entry.Length
            if ($totalBytes -gt 536870912)
            {
                throw 'Слишком большой распакованный мод'
            }
            $entries.Add($relativePath, $entry)
        }
        if (!$entries.ContainsKey('update-manifest.json') -or
            $entries['update-manifest.json'].Length -gt 262144)
        {
            throw 'В архиве отсутствует корректный update-manifest.json'
        }
        $manifestStream = $entries['update-manifest.json'].Open()
        $manifestBuffer = New-Object System.IO.MemoryStream
        try
        {
            $buffer = New-Object byte[] 65536
            [long]$manifestBytes = 0
            while (($readBytes = $manifestStream.Read($buffer, 0, $buffer.Length)) -gt 0)
            {
                $manifestBytes += $readBytes
                if ($manifestBytes -gt 262144 -or $manifestBytes -gt $entries['update-manifest.json'].Length)
                {
                    throw 'Распакованный манифест превышает заявленный размер'
                }
                $manifestBuffer.Write($buffer, 0, $readBytes)
            }
            if ($manifestBytes -ne $entries['update-manifest.json'].Length)
            {
                throw 'Размер распакованного манифеста не соответствует архиву'
            }
            $manifestText = [System.Text.Encoding]::UTF8.GetString($manifestBuffer.ToArray()).TrimStart([char]0xfeff)
            $manifest = $manifestText | ConvertFrom-Json
        }
        finally
        {
            $manifestBuffer.Dispose()
            $manifestStream.Dispose()
        }
        if ((Get-PropertyValue $manifest 'schemaVersion') -ne 1 -or
            (Get-PropertyValue $manifest 'version') -cne $Version)
        {
            throw 'Версия или формат манифеста не соответствует релизу'
        }
        $manifestFiles = @(Get-PropertyValue $manifest 'files')
        if ($manifestFiles.Count -ne ($entries.Count - 1))
        {
            throw 'Список файлов манифеста не соответствует архиву'
        }
        $manifestNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($file in $manifestFiles)
        {
            $relativePath = Get-RelativeFilePath -Path (Get-PropertyValue $file 'path')
            $sha256 = Get-PropertyValue $file 'sha256'
            $bytes = Get-PropertyValue $file 'bytes'
            if ($relativePath -eq 'update-manifest.json' -or !$manifestNames.Add($relativePath) -or
                !$entries.ContainsKey($relativePath) -or $sha256 -isnot [string] -or
                $sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
                ($bytes -isnot [int] -and $bytes -isnot [long]) -or
                $bytes -lt 0 -or $bytes -ne $entries[$relativePath].Length)
            {
                throw 'Повреждён список файлов или контрольных сумм манифеста'
            }
        }
        foreach ($relativePath in $requiredFiles)
        {
            if (!$manifestNames.Contains($relativePath))
            {
                throw "В пакете отсутствует обязательный файл: $relativePath"
            }
        }

        [void][System.IO.Directory]::CreateDirectory($StagePath)
        foreach ($file in $manifestFiles)
        {
            $relativePath = [string]$file.path
            $destinationPath = Assert-PlainPath -Path (Join-Path $StagePath $relativePath) -Boundary $StagePath
            [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $destinationPath))
            $inputStream = $entries[$relativePath].Open()
            $outputStream = [System.IO.File]::Open($destinationPath, [System.IO.FileMode]::CreateNew)
            try
            {
                $buffer = New-Object byte[] 65536
                [long]$actualBytes = 0
                while (($readBytes = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0)
                {
                    $actualBytes += $readBytes
                    if ($actualBytes -gt $file.bytes)
                    {
                        throw "Распакованный файл превышает заявленный размер: $relativePath"
                    }
                    $outputStream.Write($buffer, 0, $readBytes)
                }
            }
            finally
            {
                $outputStream.Dispose()
                $inputStream.Dispose()
            }
            if ((Get-Item -LiteralPath $destinationPath).Length -ne $file.bytes -or
                (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash -ne $file.sha256)
            {
                throw "Не совпала контрольная сумма файла: $relativePath"
            }
        }
        [System.IO.File]::WriteAllText((Join-Path $StagePath 'update-manifest.json'),
            $manifestText, (New-Object System.Text.UTF8Encoding($true)))
        if ((Read-CoopVersion -Path (Join-Path $StagePath 'liblist.gam')).ToString() -cne $Version)
        {
            throw 'Версия liblist.gam не соответствует GitHub Release'
        }
        Assert-X86Dll -Path (Join-Path $StagePath 'dlls/hl.dll')
        Assert-X86Dll -Path (Join-Path $StagePath 'cl_dlls/client.dll')
        return $manifest
    }
    finally
    {
        if ($null -ne $archive) { $archive.Dispose() }
        $stream.Dispose()
    }
}

function Test-PreservedFile
{
    param([string]$RelativePath)

    return $RelativePath -match '(?i)\.cfg$' -or
        $RelativePath -in @('settings.scr', 'CSBotConfig.vdf', 'update.json') -or
        $RelativePath -match '(?i)^(save|logs)/'
}

function Set-FileAtomically
{
    param([string]$Source, [string]$Destination, [string]$TransactionId)

    [void](Assert-PlainPath -Path $Destination -Boundary $modPath)
    $temporaryPath = Join-Path (Split-Path -Parent $Destination) ".hlcoop-update-$TransactionId.tmp"
    [void](Assert-PlainPath -Path $temporaryPath -Boundary $modPath)
    try
    {
        [System.IO.File]::Copy($Source, $temporaryPath, $false)
        if (Test-Path -LiteralPath $Destination -PathType Leaf)
        {
            [System.IO.File]::Replace($temporaryPath, $Destination, [NullString]::Value)
        }
        else
        {
            [System.IO.File]::Move($temporaryPath, $Destination)
        }
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
        {
            [System.IO.File]::Delete($temporaryPath)
        }
    }
}

function Install-VerifiedPackage
{
    param([object]$Manifest, [string]$StagePath, [string]$BackupPath,
        [string]$TransactionId, [string]$OldVersion)

    $plan = New-Object System.Collections.Generic.List[object]
    $createdDirectories = New-Object System.Collections.Generic.List[string]
    foreach ($relativePath in @(@($Manifest.files | ForEach-Object { $_.path }) + 'update-manifest.json'))
    {
        $targetPath = Assert-PlainPath -Path (Join-Path $modPath $relativePath) -Boundary $modPath
        if (Test-Path -LiteralPath $targetPath -PathType Container)
        {
            throw "Вместо файла существует папка: $relativePath"
        }
        $existed = Test-Path -LiteralPath $targetPath -PathType Leaf
        if ($existed -and (Test-PreservedFile -RelativePath $relativePath))
        {
            continue
        }
        $backupFilePath = Assert-PlainPath -Path (Join-Path $BackupPath $relativePath) -Boundary $BackupPath
        $backupHash = $null
        if ($existed)
        {
            [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $backupFilePath))
            [System.IO.File]::Copy($targetPath, $backupFilePath, $false)
            $backupHash = (Get-FileHash -LiteralPath $backupFilePath -Algorithm SHA256).Hash
            if ($backupHash -ne (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash)
            {
                throw "Файл изменился во время резервного копирования: $relativePath"
            }
        }
        $plan.Add([PSCustomObject]@{
            relative = $relativePath; target = $targetPath; existed = $existed
            backup = $backupFilePath; backupHash = $backupHash
        })
    }

    # После загрузки повторно проверяем процессы и доступ к каждому заменяемому файлу
    Assert-GameStopped
    $handles = New-Object System.Collections.Generic.List[System.IO.FileStream]
    try
    {
        foreach ($file in $plan)
        {
            if ($file.existed)
            {
                $handles.Add([System.IO.File]::Open($file.target, [System.IO.FileMode]::Open,
                    [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None))
            }
        }
    }
    finally
    {
        foreach ($handle in $handles) { $handle.Dispose() }
    }
    foreach ($file in $plan)
    {
        if ($file.existed -and
            (Get-FileHash -LiteralPath $file.target -Algorithm SHA256).Hash -ne $file.backupHash)
        {
            throw "Файл изменился перед установкой: $($file.relative)"
        }
    }
    Assert-GameStopped
    [void](Assert-PlainPath -Path $journalPath -Boundary $modPath)
    $journal = [PSCustomObject]@{
        schemaVersion = 1; project = 'half-life_coop/hlcoop'; transactionId = $TransactionId
        oldVersion = $OldVersion; newVersion = $Manifest.version
        backupPath = $BackupPath; startedUtc = [DateTime]::UtcNow.ToString('o'); files = @($plan.ToArray())
    }
    $journalText = $journal | ConvertTo-Json -Depth 8
    $journalStream = [System.IO.File]::Open($journalPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try
    {
        $journalBytes = (New-Object System.Text.UTF8Encoding($true)).GetBytes($journalText)
        $journalStream.Write($journalBytes, 0, $journalBytes.Length)
        $journalStream.Flush($true)
    }
    finally
    {
        $journalStream.Dispose()
    }
    try
    {
        # Манифест записываем последним, после обеих DLL и остальных файлов версии
        foreach ($file in $plan)
        {
            $parentPath = Split-Path -Parent $file.target
            if (!(Test-Path -LiteralPath $parentPath -PathType Container))
            {
                [void](Assert-PlainPath -Path $parentPath -Boundary $modPath)
                [void][System.IO.Directory]::CreateDirectory($parentPath)
                $createdDirectories.Add($parentPath)
            }
            Set-FileAtomically -Source (Join-Path $StagePath $file.relative) -Destination $file.target -TransactionId $TransactionId
            if ((Get-FileHash -LiteralPath $file.target -Algorithm SHA256).Hash -ne
                (Get-FileHash -LiteralPath (Join-Path $StagePath $file.relative) -Algorithm SHA256).Hash)
            {
                throw "Файл не прошёл проверку после установки: $($file.relative)"
            }
        }
        [void](Assert-InstalledMod)
        [System.IO.File]::Delete($journalPath)
        Write-Host "Мод обновлён до $($Manifest.version). Настройки и сохранения сохранены"
    }
    catch
    {
        $installMessage = $_.Exception.Message
        $rollbackErrors = New-Object System.Collections.Generic.List[string]
        for ($index = $plan.Count - 1; $index -ge 0; $index--)
        {
            $file = $plan[$index]
            try
            {
                [void](Assert-PlainPath -Path $file.target -Boundary $modPath)
                if ($file.existed)
                {
                    Set-FileAtomically -Source $file.backup -Destination $file.target -TransactionId $TransactionId
                    if ((Get-FileHash -LiteralPath $file.target -Algorithm SHA256).Hash -ne $file.backupHash)
                    {
                        throw 'Не совпала контрольная сумма восстановленного файла'
                    }
                }
                elseif (Test-Path -LiteralPath $file.target -PathType Leaf)
                {
                    [System.IO.File]::Delete($file.target)
                }
            }
            catch
            {
                $rollbackErrors.Add("$($file.relative): $($_.Exception.Message)")
            }
        }
        foreach ($directoryPath in @($createdDirectories.ToArray()) | Sort-Object -Descending)
        {
            try
            {
                [void](Assert-PlainPath -Path $directoryPath -Boundary $modPath)
                if ((Test-Path -LiteralPath $directoryPath -PathType Container) -and
                    @(Get-ChildItem -LiteralPath $directoryPath -Force).Count -eq 0)
                {
                    [System.IO.Directory]::Delete($directoryPath, $false)
                }
            }
            catch
            {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($rollbackErrors.Count -gt 0)
        {
            $script:blockLaunch = $true
            throw "Ошибка установки: $installMessage. Восстановление не завершено: $($rollbackErrors -join '; '). Резервная копия: $BackupPath. Запуск игры заблокирован"
        }
        [System.IO.File]::Delete($journalPath)
        throw "Обновление не установлено: $installMessage. Предыдущая версия полностью восстановлена"
    }
}

function Remove-OwnWorkDirectory
{
    param([string]$Path, [string]$TransactionId)

    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
    $expectedPath = Join-Path $tempRoot "hlcoop-update-$TransactionId"
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (!$fullPath.Equals($expectedPath, [System.StringComparison]::OrdinalIgnoreCase) -or
        $TransactionId -notmatch '^[a-f0-9]{32}$')
    {
        throw 'Нельзя подтвердить принадлежность временной папки обновлятору'
    }
    [void](Assert-PlainPath -Path $fullPath -Boundary $tempRoot)
    $directories = New-Object 'System.Collections.Generic.Queue[string]'
    $directories.Enqueue($fullPath)
    while ($directories.Count -gt 0)
    {
        $directoryPath = $directories.Dequeue()
        foreach ($item in Get-ChildItem -LiteralPath $directoryPath -Force)
        {
            [void](Assert-PlainPath -Path $item.FullName -Boundary $fullPath)
            if ($item.PSIsContainer)
            {
                $directories.Enqueue($item.FullName)
            }
        }
    }
    Remove-Item -LiteralPath $fullPath -Recurse -Force
}

if ($LibraryOnly)
{
    return
}

$transactionId = [guid]::NewGuid().ToString('N')
try
{
    $script:blockLaunch = $true
    $hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
    try
    {
        $mutexHash = [BitConverter]::ToString($hashAlgorithm.ComputeHash(
            [System.Text.Encoding]::UTF8.GetBytes($modPath.ToUpperInvariant()))).Replace('-', '')
    }
    finally
    {
        $hashAlgorithm.Dispose()
    }
    $mutex = New-Object System.Threading.Mutex($false, "Global\HalfLifeCoopUpdate-$mutexHash")
    try
    {
        $mutexOwned = $mutex.WaitOne(0)
    }
    catch [System.Threading.AbandonedMutexException]
    {
        $mutexOwned = $true
    }
    if (!$mutexOwned)
    {
        $script:blockLaunch = $true
        throw 'Другой обновлятор уже работает с этой папкой. Дождитесь его завершения'
    }
    [void](Assert-PlainPath -Path $journalPath -Boundary $modPath)
    if (Test-Path -LiteralPath $journalPath)
    {
        $script:blockLaunch = $true
        throw "Обнаружено незавершённое обновление. Запуск заблокирован, чтобы не смешать версии DLL. Сведения о резервной копии: $journalPath"
    }
    $script:blockLaunch = $false
    if (!$SkipUpdate)
    {
        $configPath = Assert-PlainPath -Path (Join-Path $modPath 'update.json') -Boundary $modPath
        if (!(Test-Path -LiteralPath $configPath -PathType Leaf))
        {
            Write-Host 'Автообновление не настроено: отсутствует update.json'
        }
        else
        {
            if ((Get-Item -LiteralPath $configPath).Length -gt 16384)
            {
                throw 'Слишком большой файл update.json'
            }
            $config = [System.IO.File]::ReadAllText($configPath) | ConvertFrom-Json
            $repository = Get-PropertyValue $config 'repository'
            $enabled = Get-PropertyValue $config 'enabled'
            $mode = Get-PropertyValue $config 'mode'
            $assetPrefix = Get-PropertyValue $config 'assetPrefix'
            $timeout = Get-PropertyValue $config 'timeoutSeconds'
            if ((Get-PropertyValue $config 'schemaVersion') -ne 1 -or $enabled -isnot [bool] -or
                $mode -notin @('automatic', 'notify') -or $assetPrefix -isnot [string] -or
                $assetPrefix -notmatch '^[a-zA-Z0-9][a-zA-Z0-9._-]{0,48}$' -or
                ($timeout -isnot [int] -and $timeout -isnot [long]) -or $timeout -lt 3 -or $timeout -gt 120 -or
                $repository -isnot [string])
            {
                throw 'Неподдерживаемый формат настроек update.json'
            }
            if (!$enabled)
            {
                Write-Host 'Автообновление отключено в update.json'
            }
            elseif ([string]::IsNullOrWhiteSpace($repository))
            {
                Write-Host 'GitHub-репозиторий пока не указан в update.json. Запускается установленная версия'
            }
            else
            {
                if ($repository -notmatch '^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,38})/[a-zA-Z0-9_.-]{1,100}$' -or
                    $repository.EndsWith('/.') -or $repository.EndsWith('/..') -or $repository.EndsWith('.git'))
                {
                    throw 'repository должен иметь вид owner/repo для публичного GitHub-репозитория'
                }
                $currentVersion = Assert-InstalledMod
                [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor
                    [System.Net.SecurityProtocolType]::Tls12
                Write-Host "Проверка обновлений Half-Life Co-op $currentVersion..."
                $releaseText = Invoke-GitHubDownload -Uri "https://api.github.com/repos/$repository/releases/latest" -MaximumBytes 4194304 -TimeoutSeconds $timeout
                $release = $releaseText | ConvertFrom-Json
                $tag = Get-PropertyValue $release 'tag_name'
                if ((Get-PropertyValue $release 'draft') -ne $false -or
                    (Get-PropertyValue $release 'prerelease') -ne $false -or
                    $tag -isnot [string] -or $tag -notmatch '^v?([0-9]+\.[0-9]+\.[0-9]+)$')
                {
                    throw 'GitHub не вернул стабильный релиз с числовой версией'
                }
                $releaseVersion = [version]$Matches[1]
                if ($releaseVersion -le $currentVersion)
                {
                    Write-Host "Установлена актуальная версия $currentVersion"
                }
                else
                {
                    $versionText = $releaseVersion.ToString()
                    $assetName = "$assetPrefix-$versionText-win32.zip"
                    $asset = Get-ReleaseAsset -Release $release -Name $assetName -Repository $repository -Tag $tag
                    $assetSize = Get-PropertyValue $asset 'size'
                    if (($assetSize -isnot [int] -and $assetSize -isnot [long]) -or
                        $assetSize -lt 1 -or $assetSize -gt 134217728)
                    {
                        throw 'Размер архива релиза вне разрешённых пределов'
                    }
                    if ($CheckOnly -or $mode -eq 'notify')
                    {
                        Write-Host "Доступна версия $versionText : https://github.com/$repository/releases/tag/$tag"
                    }
                    else
                    {
                        Assert-GameStopped
                        $expectedHash = Get-PropertyValue $asset 'digest'
                        if ([string]::IsNullOrWhiteSpace($expectedHash))
                        {
                            $checksumAsset = Get-ReleaseAsset -Release $release -Name "$assetName.sha256" -Repository $repository -Tag $tag
                            $checksumText = Invoke-GitHubDownload -Uri $checksumAsset.browser_download_url -MaximumBytes 4096 -TimeoutSeconds $timeout
                            $checksumPattern = '^\s*([a-fA-F0-9]{64})(?:[ \t]+\*?' + [regex]::Escape($assetName) + ')?\s*$'
                            $checksumMatch = [regex]::Match($checksumText, $checksumPattern)
                            if (!$checksumMatch.Success)
                            {
                                throw 'Не удалось прочитать SHA256 архива из файла контрольной суммы'
                            }
                            $expectedHash = $checksumMatch.Groups[1].Value
                        }
                        elseif ($expectedHash -match '^sha256:([a-fA-F0-9]{64})$')
                        {
                            $expectedHash = $Matches[1]
                        }
                        else
                        {
                            throw 'GitHub вернул неподдерживаемую контрольную сумму архива'
                        }
                        $workPath = Join-Path ([System.IO.Path]::GetTempPath()) "hlcoop-update-$transactionId"
                        [void](Assert-PlainPath -Path $workPath -Boundary ([System.IO.Path]::GetTempPath()))
                        if (Test-Path -LiteralPath $workPath)
                        {
                            throw 'Временная папка обновления уже существует'
                        }
                        [void][System.IO.Directory]::CreateDirectory($workPath)
                        $archivePath = Join-Path $workPath $assetName
                        Write-Host "Загрузка версии $versionText..."
                        Invoke-GitHubDownload -Uri $asset.browser_download_url -Path $archivePath -MaximumBytes 134217728 -TimeoutSeconds $timeout
                        if ((Get-Item -LiteralPath $archivePath).Length -ne $assetSize -or
                            (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $expectedHash)
                        {
                            throw 'Контрольная сумма или размер загруженного архива не совпали'
                        }
                        $stagePath = Join-Path $workPath 'stage'
                        $manifest = Expand-VerifiedPackage -ArchivePath $archivePath -StagePath $stagePath -Version $versionText
                        Install-VerifiedPackage -Manifest $manifest -StagePath $stagePath -BackupPath (Join-Path $workPath 'backup') -TransactionId $transactionId -OldVersion $currentVersion.ToString()
                    }
                }
            }
        }
    }
}
catch
{
    Write-Host "Обновление: $($_.Exception.Message)" -ForegroundColor Yellow
    if ($script:blockLaunch -or (Test-Path -LiteralPath $journalPath))
    {
        $script:blockLaunch = $true
        $keepWork = $true
        $exitCode = 1
    }
    else
    {
        Write-Host 'Сохраняется установленная версия мода'
    }
}
finally
{
    if ($null -ne $workPath -and (Test-Path -LiteralPath $workPath) -and !$keepWork)
    {
        try
        {
            Remove-OwnWorkDirectory -Path $workPath -TransactionId $transactionId
        }
        catch
        {
            Write-Host "Временные файлы оставлены: $workPath. $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
}

if ($Launch -and !$CheckOnly -and !$script:blockLaunch)
{
    try
    {
        # Запускаем движок напрямую, чтобы не вызвать обновлятор повторно через cmd
        [void](Assert-InstalledMod)
        Assert-GameStopped
        Start-Process -FilePath (Join-Path $gamePath 'hl.exe') -WorkingDirectory $gamePath -ArgumentList @('-game', 'hlcoop', '-console', '-insecure')
    }
    catch
    {
        Write-Host "Не удалось запустить Half-Life: $($_.Exception.Message)" -ForegroundColor Red
        $exitCode = 1
    }
}
if ($null -ne $mutex)
{
    if ($mutexOwned) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
exit $exitCode
