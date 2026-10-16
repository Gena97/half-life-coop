[CmdletBinding()]
param(
    [string]$HalfLifePath,

    [switch]$ForceConfig
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'halflife-path.ps1')
. (Join-Path $PSScriptRoot 'release-manifest.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$sourcePath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'game\hlcoop'))
$gamePath = Resolve-HalfLifePath -HalfLifePath $HalfLifePath
$modPath = [System.IO.Path]::GetFullPath((Join-Path $gamePath 'hlcoop'))
$markerPath = Join-Path $modPath '.hlcoop-install.json'
$projectId = 'half-life_coop/hlcoop'

if (@(Get-Process -Name hl -ErrorAction SilentlyContinue).Count -gt 0)
{
    throw 'Half-Life уже запущена: закройте игру перед обновлением библиотек мода'
}

foreach ($relativePath in @('liblist.gam', 'dlls\hl.dll', 'cl_dlls\client.dll'))
{
    if (!(Test-Path -LiteralPath (Join-Path $sourcePath $relativePath) -PathType Leaf))
    {
        throw "Отсутствует game\hlcoop\${relativePath}: сначала выполните scripts\build.ps1"
    }
}

if ($modPath.Equals($sourcePath, [System.StringComparison]::OrdinalIgnoreCase))
{
    throw 'Каталог установленного мода совпадает с исходным пакетом: укажите папку установленной Half-Life'
}

function Assert-CoopDestination
{
    param([string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $modPrefix = $modPath.TrimEnd('\') + '\'
    if (!$fullPath.Equals($modPath, [System.StringComparison]::OrdinalIgnoreCase) -and
        !$fullPath.StartsWith($modPrefix, [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "Путь установки выходит за пределы hlcoop: $fullPath"
    }

    # Не копируем файлы через ссылки и junction внутри папки мода
    $currentPath = $fullPath
    while ($currentPath.Length -ge $modPath.Length)
    {
        if (Test-Path -LiteralPath $currentPath)
        {
            $entry = Get-Item -LiteralPath $currentPath -Force
            if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                throw "В папке мода обнаружена ссылка или junction: $currentPath"
            }
        }
        if ($currentPath.Equals($modPath, [System.StringComparison]::OrdinalIgnoreCase))
        {
            break
        }
        $currentPath = Split-Path -Parent $currentPath
    }
}

Assert-CoopDestination -Path $modPath
if (Test-Path -LiteralPath (Join-Path $modPath '.hlcoop-update-transaction.json'))
{
    throw 'Предыдущее обновление мода не завершено: восстановите резервную копию перед установкой из исходников'
}
if (Test-Path -LiteralPath $modPath)
{
    if (!(Test-Path -LiteralPath $modPath -PathType Container))
    {
        throw "Вместо папки мода существует файл: $modPath"
    }
    Assert-CoopDestination -Path $markerPath
    if (Test-Path -LiteralPath $markerPath -PathType Leaf)
    {
        try
        {
            $marker = Get-Content -LiteralPath $markerPath -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($marker.project -ne $projectId -or $marker.format -ne 1)
            {
                throw 'Метка принадлежит другому проекту'
            }
        }
        catch
        {
            throw "Нельзя подтвердить принадлежность папки этому проекту: $markerPath"
        }
    }
    else
    {
        # Узнаём точную ручную копию текущего пакета по описанию и обеим библиотекам
        foreach ($relativePath in @('liblist.gam', 'dlls\hl.dll', 'cl_dlls\client.dll'))
        {
            $installedFilePath = Join-Path $modPath $relativePath
            Assert-CoopDestination -Path $installedFilePath
            if (!(Test-Path -LiteralPath $installedFilePath -PathType Leaf) -or
                (Get-FileHash -LiteralPath $installedFilePath -Algorithm SHA256).Hash -ne
                (Get-FileHash -LiteralPath (Join-Path $sourcePath $relativePath) -Algorithm SHA256).Hash)
            {
                throw "Папка $modPath уже существует без метки этого проекта и не совпадает с текущим пакетом: переместите её или выберите другую установку Half-Life"
            }
        }
    }
}

New-CoopReleaseManifest -SourcePath $sourcePath | Out-Null

# Обходим пакет по одному каталогу, чтобы не переходить по ссылкам
$files = New-Object 'System.Collections.Generic.List[System.IO.FileInfo]'
$directories = New-Object 'System.Collections.Generic.Queue[System.IO.DirectoryInfo]'
$directories.Enqueue((Get-Item -LiteralPath $sourcePath))
while ($directories.Count -gt 0)
{
    $directory = $directories.Dequeue()
    if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "В исходном пакете обнаружена ссылка или junction: $($directory.FullName)"
    }
    foreach ($entry in Get-ChildItem -LiteralPath $directory.FullName -Force)
    {
        if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "В исходном пакете обнаружена ссылка или junction: $($entry.FullName)"
        }
        if ($entry.PSIsContainer)
        {
            $directories.Enqueue($entry)
        }
        else
        {
            $relativePath = $entry.FullName.Substring($sourcePath.TrimEnd('\').Length + 1)
            Assert-CoopDestination -Path (Join-Path $modPath $relativePath)
            $files.Add($entry)
        }
    }
}

$markerData = [ordered]@{
    project = $projectId
    format = 1
    source = $sourcePath
    installedAtUtc = $null
}
[System.IO.Directory]::CreateDirectory($modPath) | Out-Null
$markerData | ConvertTo-Json | Set-Content -LiteralPath $markerPath -Encoding UTF8

$copiedFiles = 0
$preservedConfigs = New-Object 'System.Collections.Generic.List[string]'
foreach ($file in $files)
{
    $relativePath = $file.FullName.Substring($sourcePath.TrimEnd('\').Length + 1)
    $destinationPath = Join-Path $modPath $relativePath
    Assert-CoopDestination -Path $destinationPath
    if (Test-Path -LiteralPath $destinationPath -PathType Container)
    {
        throw "Вместо файла назначения существует папка: $destinationPath"
    }

    # Пользовательские настройки сохраняем при обновлении, пока не задан ForceConfig
    $isUserConfig = $file.Extension -ieq '.cfg' -or $relativePath -in @('settings.scr', 'CSBotConfig.vdf')
    if ((($relativePath -eq 'update.json') -or (!$ForceConfig -and $isUserConfig)) -and
        (Test-Path -LiteralPath $destinationPath -PathType Leaf))
    {
        $preservedConfigs.Add($relativePath)
        continue
    }
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $destinationPath)) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destinationPath -Force
    $copiedFiles++
}

$markerData.installedAtUtc = [DateTime]::UtcNow.ToString('o')
$markerData | ConvertTo-Json | Set-Content -LiteralPath $markerPath -Encoding UTF8
Write-Host "Мод установлен: $modPath"
Write-Host "Скопировано файлов: $copiedFiles"
if ($preservedConfigs.Count -gt 0)
{
    Write-Host "Сохранены настройки: $($preservedConfigs -join ', ')"
}

[PSCustomObject]@{
    HalfLifePath = $gamePath
    ModPath = $modPath
    CopiedFiles = $copiedFiles
    PreservedConfigs = @($preservedConfigs.ToArray())
}
