[CmdletBinding()]
param(
    [string]$HalfLifePath,

    [ValidateSet('Campaign', 'Coop')]
    [string]$Mode = 'Coop',

    [ValidatePattern('^[A-Za-z0-9_][A-Za-z0-9_-]{0,30}$')]
    [string]$Map,

    [ValidatePattern('^[A-Za-z0-9_][A-Za-z0-9_-]{0,47}$')]
    [string]$LoadSave,

    [ValidateRange(2, 32)]
    [int]$Slots = 2,

    [ValidateSet('Steam', 'Lan', 'Direct')]
    [string]$Network = 'Steam',

    [switch]$Windowed,

    [ValidateRange(640, 7680)]
    [int]$Width = 1280,

    [ValidateRange(480, 4320)]
    [int]$Height = 720,

    [switch]$SkipUpdate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'halflife-path.ps1')

if (![string]::IsNullOrWhiteSpace($LoadSave))
{
    if ($PSBoundParameters.ContainsKey('Map'))
    {
        throw 'Укажите либо Map для новой игры, либо LoadSave для продолжения сохранения'
    }
}
elseif ([string]::IsNullOrWhiteSpace($Map))
{
    $Map = 'c0a0'
}

$gamePath = Resolve-HalfLifePath -HalfLifePath $HalfLifePath
$modPath = Join-Path $gamePath 'hlcoop'
if (Test-Path -LiteralPath (Join-Path $modPath '.hlcoop-update-transaction.json'))
{
    throw 'Предыдущее обновление мода не завершено: восстановите резервную копию по данным .hlcoop-update-transaction.json перед запуском'
}
if (@(Get-Process -Name hl -ErrorAction SilentlyContinue).Count -gt 0)
{
    throw 'Half-Life уже запущена: используйте открытое окно игры или закройте её перед новым запуском'
}

# Обновляем установленный мод до выбора карты и сохранения, сохраняя аргументы запуска
$updaterPath = Join-Path $modPath 'update-hlcoop.ps1'
if (!$SkipUpdate -and (Test-Path -LiteralPath $updaterPath -PathType Leaf))
{
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $windowsPowerShell -NoProfile -ExecutionPolicy Bypass -File $updaterPath
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Обновление мода не завершено безопасно: запуск остановлен, подробности указаны выше'
    }
}

foreach ($relativePath in @('liblist.gam', 'dlls\hl.dll', 'cl_dlls\client.dll', 'coop.cfg', 'campaign.cfg'))
{
    if (!(Test-Path -LiteralPath (Join-Path $modPath $relativePath) -PathType Leaf))
    {
        throw "Мод не установлен или отсутствует ${relativePath}: сначала выполните scripts\install.ps1"
    }
}

if (![string]::IsNullOrWhiteSpace($LoadSave))
{
    $saveFile = if ($Mode -eq 'Coop') { "coop_$LoadSave.hcs" } else { "$LoadSave.sav" }
    $savePath = Join-Path $modPath "save\$saveFile"
    if ($LoadSave -ieq 'quick' -and !(Test-Path -LiteralPath $savePath -PathType Leaf))
    {
        # Продолжение использует автосохранение, если игрок ещё не нажимал F6
        $autoSaveFile = if ($Mode -eq 'Coop') { 'coop_autosave.hcs' } else { 'autosave.sav' }
        $autoSavePath = Join-Path $modPath "save\$autoSaveFile"
        if (Test-Path -LiteralPath $autoSavePath -PathType Leaf)
        {
            $LoadSave = 'autosave'
            $savePath = $autoSavePath
            Write-Host 'Быстрое сохранение отсутствует, загружаем автосохранение'
        }
        else
        {
            throw "Не найдены $saveFile и ${autoSaveFile}: начните кампанию через start-campaign.cmd и сохраните игру клавишей F6 или дождитесь автосохранения"
        }
    }
    if (!(Test-Path -LiteralPath $savePath -PathType Leaf))
    {
        throw "Сохранение не найдено: $savePath — начните кампанию и сохраните игру клавишей F6"
    }
}

if (@(Get-Process -Name hl -ErrorAction SilentlyContinue).Count -gt 0)
{
    throw 'Half-Life уже запущена: используйте открытое окно игры или закройте её перед новым запуском'
}

# Одиночная кампания использует штатные переходы и сохранения Half-Life
$actualSlots = if ($Mode -eq 'Campaign') { 1 } else { $Slots }
$coopValue = if ($Mode -eq 'Campaign') { '0' } else { '1' }

# Имена карты, сохранения и мода не содержат пробелов или команд консоли
$launchArguments = @(
    '-game', 'hlcoop',
    '-insecure',
    '-console',
    '-condebug',
    '+developer', '1',
    '+maxplayers', [string]$actualSlots,
    '+coop', $coopValue,
    '+deathmatch', '0',
    '+log', 'on'
)
if ($Mode -eq 'Coop')
{
    # Транспорт выбирается до создания сервера и сохраняется при загрузке сессии
    $lanValue = if ($Network -eq 'Lan') { '1' } else { '0' }
    $steamNetworkingValue = if ($Network -eq 'Steam') { '1' } else { '0' }
    $launchArguments += @(
        '+sv_lan', $lanValue,
        '+sv_use_steam_networking', $steamNetworkingValue,
        '+setmaster', 'enable',
        '+exec', 'coop.cfg'
    )
}
else
{
    $launchArguments += @('+exec', 'campaign.cfg')
}
if (![string]::IsNullOrWhiteSpace($LoadSave))
{
    if ($Mode -eq 'Coop')
    {
        # Команда загрузки сессии доступна после создания сервера и первого кадра
        $launchArguments += @('+map', 'c0a0', '+wait', '+wait', '+coop_load', $LoadSave)
    }
    else
    {
        $launchArguments += @('+load', $LoadSave)
    }
}
else
{
    $launchArguments += @('+skill', '2', '+map', $Map)
    if ($Mode -eq 'Coop' -and $Map -ne 'c0a0')
    {
        $launchArguments += @('+coop_start', $Map)
    }
}
if ($Windowed)
{
    $launchArguments = @('-windowed', '-w', [string]$Width, '-h', [string]$Height) + $launchArguments
}

# Пользователь запускает саму игру, поэтому оставляем её окно видимым
$process = Start-Process -FilePath (Join-Path $gamePath 'hl.exe') -ArgumentList $launchArguments -WorkingDirectory $gamePath -PassThru
if (![string]::IsNullOrWhiteSpace($LoadSave))
{
    Write-Host "Half-Life запущена: режим $Mode, сохранение $LoadSave, PID $($process.Id)"
}
else
{
    Write-Host "Half-Life запущена: режим $Mode, карта $Map, игроков $actualSlots, PID $($process.Id)"
}
Write-Host "Журнал консоли: $(Join-Path $gamePath 'qconsole.log')"

[PSCustomObject]@{
    ProcessId = $process.Id
    HalfLifePath = $gamePath
    ModPath = $modPath
    Mode = $Mode
    Map = $Map
    LoadSave = $LoadSave
    Slots = $actualSlots
    Network = if ($Mode -eq 'Coop') { $Network } else { $null }
    ConsoleLogPath = Join-Path $gamePath 'qconsole.log'
}
