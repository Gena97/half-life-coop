[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackagePath,
    [Parameter(Mandatory = $true)][string]$LauncherPath,
    [Parameter(Mandatory = $true)][string]$LauncherVersion,
    [Parameter(Mandatory = $true)][int]$ParentProcessId,
    [string]$HalfLifePath,
    [switch]$InstallOnly,
    [switch]$SkipUpdate,
    [switch]$AfterUpdate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$OutputEncoding = [Console]::OutputEncoding
$embeddedPackageHash = '__HLC_PACKAGE_SHA256__'
$embeddedVersion = '__HLC_PACKAGE_VERSION__'
$launcherRepository = '__HLC_REPOSITORY__'
$requestedSkipUpdate = [bool]$SkipUpdate
$requestedInstallOnly = [bool]$InstallOnly
$bootstrapRoot = $PSScriptRoot
$installationMutex = $null
$installationMutexOwned = $false
$installationWork = $null
$installationTransaction = $null
$installationFailed = $false
$bootstrapExitCode = 0
$transcriptStarted = $false

# __HLC_PATH_RESOLVER__

function ConvertTo-NativeArgument
{
    param([string]$Value)
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Get-LauncherGamePath
{
    param([string]$ExplicitPath, [string]$StatePath)

    if (![string]::IsNullOrWhiteSpace($ExplicitPath))
    {
        return Resolve-HalfLifePath -HalfLifePath $ExplicitPath
    }
    if (Test-Path -LiteralPath $StatePath -PathType Leaf)
    {
        try
        {
            if ((Get-Item -LiteralPath $StatePath).Length -lt 16384)
            {
                $state = [System.IO.File]::ReadAllText($StatePath) | ConvertFrom-Json
                return Resolve-HalfLifePath -HalfLifePath ([string]$state.halfLifePath)
            }
        }
        catch { }
    }
    try { return Resolve-HalfLifePath }
    catch { Write-Host $_.Exception.Message }

    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    try
    {
        $dialog.Description = 'Выберите папку оригинальной Half-Life с hl.exe и папкой valve'
        $dialog.ShowNewFolderButton = $false
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK)
        {
            throw 'Папка Half-Life не выбрана'
        }
        return Resolve-HalfLifePath -HalfLifePath $dialog.SelectedPath
    }
    finally { $dialog.Dispose() }
}

function Assert-LauncherExecutable
{
    param([string]$Path, [string]$Version)

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try
    {
        if ($stream.Length -lt 4096 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Повреждён EXE запускателя' }
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt ($stream.Length - 26)) { throw 'Повреждён заголовок EXE' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x014c)
        {
            throw 'Обновление запускателя должно быть Win32 EXE'
        }
        $stream.Position = $offset + 22
        $flags = $reader.ReadUInt16()
        if (($flags -band 0x0002) -eq 0 -or ($flags -band 0x2000) -ne 0 -or
            $reader.ReadUInt16() -ne 0x010b) { throw 'Полученный файл не является EXE PE32' }
    }
    finally { $reader.Dispose() }
    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    if ($info.ProductName -ne 'Half-Life Co-op' -or $info.CompanyName -ne 'HalfLifeCoop' -or
        [version]$info.ProductVersion -ne [version]$Version)
    {
        throw 'Версия или описание EXE не соответствуют релизу'
    }
}

function Start-LauncherSelfUpdate
{
    param([string]$GameDirectory)

    $releaseText = Invoke-GitHubDownload -Uri "https://api.github.com/repos/$launcherRepository/releases/latest" -MaximumBytes 4194304 -TimeoutSeconds 15
    $release = $releaseText | ConvertFrom-Json
    $tag = Get-PropertyValue $release 'tag_name'
    if ((Get-PropertyValue $release 'draft') -ne $false -or
        (Get-PropertyValue $release 'prerelease') -ne $false -or
        $tag -isnot [string] -or $tag -notmatch '^v?([0-9]+\.[0-9]+\.[0-9]+)$')
    {
        throw 'GitHub не вернул стабильный релиз'
    }
    $releaseVersion = $Matches[1]
    if ([version]$releaseVersion -le [version]$LauncherVersion) { return $false }
    $asset = Get-ReleaseAsset -Release $release -Name 'HLCoopLauncher.exe' -Repository $launcherRepository -Tag $tag
    $assetSize = Get-PropertyValue $asset 'size'
    if (($assetSize -isnot [int] -and $assetSize -isnot [long]) -or $assetSize -lt 4096 -or $assetSize -gt 134217728)
    {
        throw 'Размер EXE обновления вне допустимых пределов'
    }
    $expectedHash = Get-PropertyValue $asset 'digest'
    if ([string]::IsNullOrWhiteSpace($expectedHash))
    {
        $checksum = Get-ReleaseAsset -Release $release -Name 'HLCoopLauncher.exe.sha256' -Repository $launcherRepository -Tag $tag
        $checksumText = Invoke-GitHubDownload -Uri $checksum.browser_download_url -MaximumBytes 4096 -TimeoutSeconds 15
        $match = [regex]::Match($checksumText, '^\s*([a-fA-F0-9]{64})(?:[ \t]+\*?HLCoopLauncher\.exe)?\s*$')
        if (!$match.Success) { throw 'Не найдена контрольная сумма EXE' }
        $expectedHash = $match.Groups[1].Value
    }
    elseif ($expectedHash -match '^sha256:([a-fA-F0-9]{64})$') { $expectedHash = $Matches[1] }
    else { throw 'Неподдерживаемая контрольная сумма EXE' }

    $launcherDirectory = Split-Path -Parent $LauncherPath
    $selfWork = Join-Path $launcherDirectory ('.hlcoop-update-' + [guid]::NewGuid().ToString('N'))
    $selfWorkOwned = $false
    $helperStarted = $false
    try
    {
        [void](Assert-PlainPath -Path $selfWork -Boundary $launcherDirectory)
        if (Test-Path -LiteralPath $selfWork) { throw 'Временная папка обновления уже существует' }
        [void][System.IO.Directory]::CreateDirectory($selfWork)
        $selfWorkOwned = $true
        $selfDirectory = Get-Item -LiteralPath $selfWork -Force
        $selfDirectory.Attributes = $selfDirectory.Attributes -bor [IO.FileAttributes]::Hidden
        $downloadPath = Join-Path $selfWork 'HLCoopLauncher.exe'
        Write-Host "Загрузка запускателя $releaseVersion..."
        Invoke-GitHubDownload -Uri $asset.browser_download_url -Path $downloadPath -MaximumBytes 134217728 -TimeoutSeconds 30
        if ((Get-Item -LiteralPath $downloadPath).Length -ne $assetSize -or
            (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash -ne $expectedHash)
        {
            throw 'Не совпали размер или SHA256 запускателя'
        }
        Assert-LauncherExecutable -Path $downloadPath -Version $releaseVersion
        $parentProcess = [System.Diagnostics.Process]::GetProcessById($ParentProcessId)
        $helperState = [ordered]@{
            target = $LauncherPath; staged = $downloadPath; expectedHash = $expectedHash
            oldHash = (Get-FileHash -LiteralPath $LauncherPath -Algorithm SHA256).Hash
            parentId = $ParentProcessId; parentStart = $parentProcess.StartTime.ToUniversalTime().Ticks.ToString()
            gamePath = $GameDirectory; installOnly = $requestedInstallOnly; skipUpdate = $requestedSkipUpdate
        }
        $encoding = New-Object System.Text.UTF8Encoding($true)
        $helperStatePath = Join-Path $selfWork 'state.json'
        [System.IO.File]::WriteAllText($helperStatePath, ($helperState | ConvertTo-Json), $encoding)
        $helperPath = Join-Path $selfWork 'replace-launcher.ps1'
        $helperText = @'
param([Parameter(Mandatory = $true)][string]$StatePath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$state = [System.IO.File]::ReadAllText($StatePath) | ConvertFrom-Json
$root = [System.IO.Path]::GetFullPath($PSScriptRoot)
$targetPath = [System.IO.Path]::GetFullPath([string]$state.target)
$launcherDirectory = Split-Path -Parent $targetPath
$logDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'HalfLifeCoop\launcher'
if (!(Split-Path -Parent $root).Equals($launcherDirectory, [StringComparison]::OrdinalIgnoreCase) -or
    (Split-Path -Leaf $root) -notmatch '^\.hlcoop-update-[0-9a-f]{32}$' -or
    !$state.target.Equals($targetPath, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetExtension($targetPath) -ine '.exe' -or
    !$state.staged.Equals((Join-Path $root 'HLCoopLauncher.exe'), [StringComparison]::OrdinalIgnoreCase))
{
    exit 1
}
$backup = Join-Path $root 'previous.exe'
$swapped = $false
$canLaunch = $true
try
{
    [System.IO.File]::WriteAllText((Join-Path $root 'ready'), 'ready')
    try { $parent = [Diagnostics.Process]::GetProcessById([int]$state.parentId) } catch { $parent = $null }
    if ($null -ne $parent -and $parent.StartTime.ToUniversalTime().Ticks.ToString() -eq $state.parentStart)
    {
        if (!$parent.WaitForExit(60000)) { throw 'Запускатель не завершился для обновления' }
    }
    foreach ($path in @($root, $state.target, $state.staged))
    {
        $currentPath = $path
        while (![string]::IsNullOrWhiteSpace($currentPath))
        {
            $item = Get-Item -LiteralPath $currentPath -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Обновление через ссылку запрещено' }
            $parentPath = Split-Path -Parent $currentPath
            if ($parentPath -eq $currentPath) { break }
            $currentPath = $parentPath
        }
    }
    if ((Get-FileHash -LiteralPath $state.staged -Algorithm SHA256).Hash -ne $state.expectedHash -or
        (Get-FileHash -LiteralPath $state.target -Algorithm SHA256).Hash -ne $state.oldHash)
    {
        throw 'Файлы запускателя изменились перед заменой'
    }
    [System.IO.File]::Replace($state.staged, $state.target, $backup)
    $swapped = $true
    if ((Get-FileHash -LiteralPath $state.target -Algorithm SHA256).Hash -ne $state.expectedHash)
    {
        throw 'Не совпал SHA256 установленного запускателя'
    }
}
catch
{
    $message = $_.Exception.Message
    if ($swapped)
    {
        try
        {
            [System.IO.File]::Replace($backup, $state.target, [NullString]::Value)
            if ((Get-FileHash -LiteralPath $state.target -Algorithm SHA256).Hash -ne $state.oldHash)
            {
                throw 'Не удалось восстановить прежний запускатель'
            }
        }
        catch { $canLaunch = $false; $message += '. ' + $_.Exception.Message }
    }
    [System.IO.File]::WriteAllText((Join-Path $logDirectory 'last-update-error.txt'), $message)
    Add-Type -AssemblyName System.Windows.Forms
    [void][Windows.Forms.MessageBox]::Show($message, 'Half-Life Co-op: обновление запускателя')
}
if ($canLaunch)
{
    $gameArgument = '"' + [regex]::Replace([regex]::Replace([string]$state.gamePath, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    $arguments = '--after-update --game-dir ' + $gameArgument
    if ($state.installOnly) { $arguments += ' --install-only' }
    if ($state.skipUpdate) { $arguments += ' --skip-update' }
    Start-Process -FilePath $state.target -ArgumentList $arguments -WorkingDirectory $launcherDirectory -WindowStyle Normal
    try
    {
        # Удаляем только файлы собственного завершённого обновления
        foreach ($file in Get-ChildItem -LiteralPath $root -Force)
        {
            if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                throw 'Неожиданный файл во временной папке обновления'
            }
        }
        foreach ($file in Get-ChildItem -LiteralPath $root -Force) { [IO.File]::Delete($file.FullName) }
        [IO.Directory]::Delete($root, $false)
    }
    catch { }
}
'@
        [System.IO.File]::WriteAllText($helperPath, $helperText, $encoding)
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File ' + (ConvertTo-NativeArgument $helperPath) +
            ' -StatePath ' + (ConvertTo-NativeArgument $helperStatePath)
        $helper = Start-Process -FilePath $windowsPowerShell -ArgumentList $arguments -WindowStyle Hidden -PassThru
        $helperStarted = $true
        $wait = [Diagnostics.Stopwatch]::StartNew()
        while (!(Test-Path -LiteralPath (Join-Path $selfWork 'ready')))
        {
            if ($helper.HasExited -or $wait.Elapsed.TotalSeconds -gt 10) { throw 'Не удалось запустить замену запускателя' }
            Start-Sleep -Milliseconds 100
        }
        Write-Host "HLCOOP_RELAUNCH $LauncherPath"
        return $true
    }
    catch
    {
        $selfUpdateError = $_
        if ($selfWorkOwned -and !$helperStarted)
        {
            try
            {
                $cleanupPath = [System.IO.Path]::GetFullPath($selfWork)
                $cleanupBoundary = [System.IO.Path]::GetFullPath($launcherDirectory)
                if (!(Split-Path -Parent $cleanupPath).TrimEnd('\').Equals($cleanupBoundary.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase) -or
                    (Split-Path -Leaf $cleanupPath) -notmatch '^\.hlcoop-update-[0-9a-f]{32}$')
                {
                    throw 'Нельзя подтвердить принадлежность временной папки запускателю'
                }
                [void](Assert-PlainPath -Path $cleanupPath -Boundary $cleanupBoundary)
                $cleanupFiles = @(Get-ChildItem -LiteralPath $cleanupPath -Force)
                foreach ($file in $cleanupFiles)
                {
                    [void](Assert-PlainPath -Path $file.FullName -Boundary $cleanupPath)
                    if ($file.PSIsContainer -or $file.Name -notin @('HLCoopLauncher.exe', 'state.json', 'replace-launcher.ps1'))
                    {
                        throw 'Неожиданный файл во временной папке обновления'
                    }
                }
                foreach ($file in $cleanupFiles) { [System.IO.File]::Delete($file.FullName) }
                [System.IO.Directory]::Delete($cleanupPath, $false)
            }
            catch
            {
                Write-Host "Временная папка обновления оставлена: $selfWork. $($_.Exception.Message)"
            }
        }
        throw $selfUpdateError
    }
}

try
{
    if ($embeddedPackageHash -notmatch '^[0-9a-fA-F]{64}$' -or
        (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash -ne $embeddedPackageHash)
    {
        throw 'Встроенный пакет мода повреждён'
    }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($PackagePath)
    try
    {
        $updaterEntry = $archive.GetEntry('hlcoop/update-hlcoop.ps1')
        if ($null -eq $updaterEntry -or $updaterEntry.Length -gt 262144) { throw 'Нет встроенного обновлятора' }
        $reader = New-Object System.IO.StreamReader($updaterEntry.Open(), [Text.Encoding]::UTF8, $true)
        try { $updaterText = $reader.ReadToEnd() } finally { $reader.Dispose() }
        $libraryPath = Join-Path $bootstrapRoot 'updater-library.ps1'
        [System.IO.File]::WriteAllText($libraryPath, $updaterText, (New-Object System.Text.UTF8Encoding($true)))
    }
    finally { $archive.Dispose() }

    # Библиотечный режим не запускает игру и не обращается к сети
    . $libraryPath -LibraryOnly
    $launcherBase = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'HalfLifeCoop\launcher'
    [void](Assert-PlainPath -Path $launcherBase -Boundary ([Environment]::GetFolderPath('LocalApplicationData')))
    [void][IO.Directory]::CreateDirectory($launcherBase)
    $LauncherPath = [IO.Path]::GetFullPath($LauncherPath)
    [void](Assert-PlainPath -Path $LauncherPath -Boundary (Split-Path -Parent $LauncherPath))
    Assert-LauncherExecutable -Path $LauncherPath -Version $LauncherVersion
    try
    {
        Start-Transcript -LiteralPath (Join-Path $launcherBase 'launcher.log') -Force | Out-Null
        $transcriptStarted = $true
    }
    catch { Write-Host 'Не удалось сохранить журнал запускателя' }
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $statePath = Join-Path (Split-Path -Parent $launcherBase) 'settings.json'
    $gamePath = Get-LauncherGamePath -ExplicitPath $HalfLifePath -StatePath $statePath
    $modPath = [IO.Path]::GetFullPath((Join-Path $gamePath 'hlcoop'))
    $journalPath = Join-Path $modPath '.hlcoop-update-transaction.json'
    Write-Host "Half-Life: $gamePath"
    [void](Assert-PlainPath -Path $gamePath -Boundary $gamePath)
    [void](Assert-PlainPath -Path $modPath -Boundary $gamePath)
    if (!$requestedSkipUpdate -and !$AfterUpdate)
    {
        try
        {
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            if (Start-LauncherSelfUpdate -GameDirectory $gamePath) { exit 0 }
        }
        catch { Write-Host "Обновление запускателя отложено: $($_.Exception.Message)" }
    }

    $hash = [Security.Cryptography.SHA256]::Create()
    try
    {
        $mutexHash = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($modPath.ToUpperInvariant()))).Replace('-', '')
    }
    finally { $hash.Dispose() }
    $installationMutex = New-Object System.Threading.Mutex($false, "Global\HalfLifeCoopUpdate-$mutexHash")
    try { $installationMutexOwned = $installationMutex.WaitOne(0) }
    catch [System.Threading.AbandonedMutexException] { $installationMutexOwned = $true }
    if (!$installationMutexOwned) { throw 'Другой обновлятор уже работает с этой папкой Half-Life' }
    if (Test-Path -LiteralPath $journalPath) { throw "Предыдущее обновление не завершено. Сведения о резервной копии: $journalPath" }
    Assert-GameStopped
    $installedVersion = [version]'0.0.0'
    if (Test-Path -LiteralPath $modPath)
    {
        if (!(Test-Path -LiteralPath $modPath -PathType Container)) { throw 'Вместо папки hlcoop существует файл' }
        if (@(Get-ChildItem -LiteralPath $modPath -Force).Count -gt 0) { $installedVersion = Assert-InstalledMod }
    }
    if ($installedVersion -lt [version]$embeddedVersion)
    {
        Write-Host "Установка Half-Life Co-op $embeddedVersion..."
        $installationTransaction = [guid]::NewGuid().ToString('N')
        $installationWork = Join-Path ([IO.Path]::GetTempPath()) "hlcoop-update-$installationTransaction"
        [void](Assert-PlainPath -Path $installationWork -Boundary ([IO.Path]::GetTempPath()))
        if (Test-Path -LiteralPath $installationWork) { throw 'Временная папка установки уже существует' }
        [void][IO.Directory]::CreateDirectory($installationWork)
        $stage = Join-Path $installationWork 'stage'
        $manifest = Expand-VerifiedPackage -ArchivePath $PackagePath -StagePath $stage -Version $embeddedVersion
        [void][IO.Directory]::CreateDirectory($modPath)
        Install-VerifiedPackage -Manifest $manifest -StagePath $stage -BackupPath (Join-Path $installationWork 'backup') -TransactionId $installationTransaction -OldVersion $installedVersion.ToString()
    }
    else { Write-Host "Установлена версия $installedVersion" }
    [void](Assert-InstalledMod)
    $state = [ordered]@{ schemaVersion = 1; halfLifePath = $gamePath }
    $stateTemporaryPath = Join-Path (Split-Path -Parent $statePath) ('.settings-' + [guid]::NewGuid().ToString('N') + '.json')
    [IO.File]::WriteAllText($stateTemporaryPath, ($state | ConvertTo-Json), (New-Object Text.UTF8Encoding($true)))
    if (Test-Path -LiteralPath $statePath -PathType Leaf) { [IO.File]::Replace($stateTemporaryPath, $statePath, [NullString]::Value) }
    else { [IO.File]::Move($stateTemporaryPath, $statePath) }
}
catch
{
    $installationFailed = $true
    $bootstrapExitCode = 1
    Write-Host "Не удалось подготовить мод: $($_.Exception.Message)"
}
finally
{
    if ($installationMutexOwned) { $installationMutex.ReleaseMutex() }
    if ($null -ne $installationMutex) { $installationMutex.Dispose() }
    if ($null -ne $installationWork -and !(Test-Path -LiteralPath $journalPath))
    {
        try { Remove-OwnWorkDirectory -Path $installationWork -TransactionId $installationTransaction }
        catch { Write-Host "Временные файлы установки оставлены: $installationWork" }
    }
}

if (!$installationFailed)
{
    try
    {
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $modPath 'update-hlcoop.ps1'))
        if (!$requestedInstallOnly) { $arguments += '-Launch' }
        if ($requestedSkipUpdate) { $arguments += '-SkipUpdate' }
        & $windowsPowerShell @arguments
        if ($LASTEXITCODE -ne 0) { throw 'Установка или запуск не завершены, подробности выше' }
        if ($requestedInstallOnly) { Write-Host 'Установка завершена. Для игры откройте этот EXE без параметра --install-only' }
        else { Write-Host 'Half-Life Co-op запущен' }
    }
    catch { $bootstrapExitCode = 1; Write-Host $_.Exception.Message }
}
if ($transcriptStarted) { try { Stop-Transcript | Out-Null } catch { } }
exit $bootstrapExitCode
