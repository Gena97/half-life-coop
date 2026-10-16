[CmdletBinding()]
param(
    [string]$HalfLifePath,

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateRange(1024, 65535)]
    [int]$Port = 27016,

    [ValidateSet('hlcoop_check', 'hlcoop_check_2', 'hlcoop_check_3', 'hlcoop_check_4')]
    [string]$CheckName = 'hlcoop_check',

    [ValidateRange(0, 95)]
    [int]$FirstIndex = 0,

    [ValidateRange(1, 96)]
    [int]$Count = 96,

    [ValidateRange(15, 120)]
    [int]$DwellSeconds = 20,

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scriptsPath = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptsPath
. (Join-Path $scriptsPath 'halflife-path.ps1')

if (!$PSBoundParameters.ContainsKey('Count'))
{
    $Count = 96 - $FirstIndex
}
if ($Count -gt 96 - $FirstIndex)
{
    throw 'Диапазон FirstIndex и Count выходит за пределы 96 карт'
}

$gamePath = Resolve-HalfLifePath -HalfLifePath $HalfLifePath
$serverPath = Join-Path $gamePath 'hlds.exe'
$sourcePath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'game\hlcoop'))
$modPath = [System.IO.Path]::GetFullPath((Join-Path $gamePath $CheckName))
$markerPath = Join-Path $modPath '.hlcoop-check.json'
$projectId = 'half-life_coop/map-check'
$reportPath = Join-Path $modPath 'map-check.jsonl'
$reportDirectory = Join-Path $repoRoot 'artifacts\map-check'
$runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')

foreach ($requiredPath in @($serverPath, (Join-Path $gamePath 'swds.dll'), (Join-Path $sourcePath 'liblist.gam')))
{
    if (!(Test-Path -LiteralPath $requiredPath -PathType Leaf))
    {
        throw "Отсутствует необходимый файл: $requiredPath"
    }
}

function Assert-CheckDestination
{
    param([string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (!$fullPath.Equals($modPath, [System.StringComparison]::OrdinalIgnoreCase) -and
        !$fullPath.StartsWith($modPath.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "Путь выходит за пределы ${CheckName}: $fullPath"
    }
    $currentPath = $fullPath
    while ($currentPath.Length -ge $modPath.Length)
    {
        if (Test-Path -LiteralPath $currentPath)
        {
            $entry = Get-Item -LiteralPath $currentPath -Force
            if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                throw "В проверочном моде обнаружена ссылка или junction: $currentPath"
            }
        }
        if ($currentPath.Equals($modPath, [System.StringComparison]::OrdinalIgnoreCase))
        {
            break
        }
        $currentPath = Split-Path -Parent $currentPath
    }
}

Assert-CheckDestination -Path $modPath
if (Test-Path -LiteralPath $modPath)
{
    if (!(Test-Path -LiteralPath $modPath -PathType Container))
    {
        throw "Вместо проверочной папки существует файл: $modPath"
    }
    Assert-CheckDestination -Path $markerPath
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
        throw "Папка уже существует без подходящей метки проверочного окружения: $modPath"
    }
}

# Работающую игру и другие серверы не закрываем
$checkCommandPattern = '(?i)(?:^|\s)-game\s+"?' + [regex]::Escape($CheckName) + '(?:"|\s|$)'
$runningServers = @(Get-CimInstance -ClassName Win32_Process -Filter "Name = 'hlds.exe'" | Where-Object {
    $_.ExecutablePath -ieq $serverPath -and
    $_.CommandLine -match $checkCommandPattern
})
if ($runningServers.Count -gt 0)
{
    throw "Сервер $CheckName уже запущен: дождитесь его завершения перед обновлением проверочных DLL"
}

# Занятый порт не освобождаем принудительно
$portProbe = New-Object System.Net.Sockets.UdpClient
try
{
    $portProbe.Client.ExclusiveAddressUse = $true
    $portProbe.Client.Bind((New-Object System.Net.IPEndPoint -ArgumentList ([System.Net.IPAddress]::Loopback), $Port))
}
finally
{
    $portProbe.Close()
}

if ($SkipBuild)
{
    # Общая готовая сборка читается без повторного запуска MSBuild для параллельных серверов
    $buildRoot = Join-Path $repoRoot "build\MapCheck\$Configuration"
    $build = [PSCustomObject]@{
        BuildPath = $buildRoot
        BinaryPaths = [PSCustomObject]@{
            ServerDll = Join-Path $buildRoot 'hldll\hl.dll'
            ServerPdb = Join-Path $buildRoot 'hldll\hl.pdb'
            ClientDll = Join-Path $buildRoot 'hl_cdll\client.dll'
        }
    }
}
else
{
    $build = & (Join-Path $scriptsPath 'build.ps1') -Configuration $Configuration -MapCheck
}
foreach ($binaryPath in @($build.BinaryPaths.ServerDll, $build.BinaryPaths.ClientDll))
{
    if (!(Test-Path -LiteralPath $binaryPath -PathType Leaf))
    {
        throw "После сборки отсутствует библиотека: $binaryPath"
    }
}

[System.IO.Directory]::CreateDirectory($modPath) | Out-Null
$markerData = [ordered]@{
    project = $projectId
    format = 1
    checkName = $CheckName
    firstIndex = $FirstIndex
    count = $Count
    dwellSeconds = $DwellSeconds
    source = $sourcePath
    preparedAtUtc = [DateTime]::UtcNow.ToString('o')
}
$markerData | ConvertTo-Json | Set-Content -LiteralPath $markerPath -Encoding UTF8

# Копируем только продуктовые файлы, не переходя по ссылкам и не перенося сохранения игрока
$directories = New-Object 'System.Collections.Generic.Queue[System.IO.DirectoryInfo]'
$directories.Enqueue((Get-Item -LiteralPath $sourcePath))
while ($directories.Count -gt 0)
{
    $directory = $directories.Dequeue()
    if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "В пакете мода обнаружена ссылка или junction: $($directory.FullName)"
    }
    foreach ($entry in Get-ChildItem -LiteralPath $directory.FullName -Force)
    {
        if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "В пакете мода обнаружена ссылка или junction: $($entry.FullName)"
        }
        $relativePath = $entry.FullName.Substring($sourcePath.TrimEnd('\').Length + 1)
        if ($relativePath -match '^(save|logs)(\\|$)')
        {
            continue
        }
        if ($entry.PSIsContainer)
        {
            $directories.Enqueue($entry)
            continue
        }
        if ($entry.Extension -in @('.pdb', '.log', '.hcs', '.sav') -or
            $entry.Name -in @('config.cfg', '.hlcoop-install.json', '.hlcoop-check.json', 'map-check.jsonl'))
        {
            continue
        }
        $destinationPath = Join-Path $modPath $relativePath
        Assert-CheckDestination -Path $destinationPath
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $destinationPath)) | Out-Null
        Copy-Item -LiteralPath $entry.FullName -Destination $destinationPath -Force
    }
}

foreach ($target in @(
    @{ Source = $build.BinaryPaths.ServerDll; Relative = 'dlls\hl.dll' },
    @{ Source = $build.BinaryPaths.ServerPdb; Relative = 'dlls\hl.pdb' },
    @{ Source = $build.BinaryPaths.ClientDll; Relative = 'cl_dlls\client.dll' }
))
{
    if (Test-Path -LiteralPath $target.Source -PathType Leaf)
    {
        $destinationPath = Join-Path $modPath $target.Relative
        Assert-CheckDestination -Path $destinationPath
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $destinationPath)) | Out-Null
        Copy-Item -LiteralPath $target.Source -Destination $destinationPath -Force
    }
}

# Все настройки и журналы этого сервера остаются внутри отдельного мода
$checkConfig = @'
coop 1
deathmatch 0
sv_lan 1
sv_use_steam_networking 0
coop_autosave 0
setmaster disable
'@
foreach ($relativePath in @('autoexec.cfg', 'server.cfg'))
{
    $destinationPath = Join-Path $modPath $relativePath
    Assert-CheckDestination -Path $destinationPath
    [System.IO.File]::WriteAllText($destinationPath, $checkConfig + "`r`n", [System.Text.Encoding]::ASCII)
}
foreach ($relativePath in @('save', 'logs'))
{
    $destinationPath = Join-Path $modPath $relativePath
    Assert-CheckDestination -Path $destinationPath
    [System.IO.Directory]::CreateDirectory($destinationPath) | Out-Null
}
[System.IO.Directory]::CreateDirectory($reportDirectory) | Out-Null
Assert-CheckDestination -Path $reportPath
if (Test-Path -LiteralPath $reportPath -PathType Leaf)
{
    Copy-Item -LiteralPath $reportPath -Destination (Join-Path $reportDirectory "$runId-previous.jsonl")
}
[System.IO.File]::WriteAllText($reportPath, '', (New-Object System.Text.UTF8Encoding -ArgumentList $false))

$rconPassword = [Guid]::NewGuid().ToString('N')
function Invoke-CheckRcon
{
    param([string]$Command)

    $udp = New-Object System.Net.Sockets.UdpClient
    try
    {
        $udp.Client.ReceiveTimeout = 1000
        $udp.Connect([System.Net.IPAddress]::Loopback, $Port)
        $header = [byte[]]@(255, 255, 255, 255)
        $challengeRequest = $header + [System.Text.Encoding]::ASCII.GetBytes("challenge rcon`n")
        [void]$udp.Send($challengeRequest, $challengeRequest.Length)
        $remote = New-Object System.Net.IPEndPoint -ArgumentList ([System.Net.IPAddress]::Any), 0
        $response = [System.Text.Encoding]::ASCII.GetString($udp.Receive([ref]$remote))
        if ($response -notmatch 'challenge rcon (-?\d+)')
        {
            throw 'Сервер не вернул RCON challenge'
        }
        $challenge = $Matches[1]
        $request = $header + [System.Text.Encoding]::ASCII.GetBytes("rcon $challenge `"$rconPassword`" $Command`n")
        [void]$udp.Send($request, $request.Length)
        $response = [System.Text.Encoding]::ASCII.GetString($udp.Receive([ref]$remote))
        return ($response -replace '^[\s\S]{4}l', '').Trim([char[]]@(0, 10, 13))
    }
    finally
    {
        $udp.Close()
    }
}

$launchArguments = @(
    '-console', '-game', $CheckName, '-insecure',
    '+ip', '127.0.0.1', '-port', [string]$Port, '-nomaster',
    '+maxplayers', '4', '+coop', '1', '+deathmatch', '0',
    '+developer', '1', '+coop_autosave', '0',
    '+sv_lan', '1', '+sv_use_steam_networking', '0',
    '+rcon_password', $rconPassword,
    '+log', 'on', '+map', 'c0a0'
)
$server = $null
$hideHelper = $null
$started = $false
try
{
    $server = Start-Process -FilePath $serverPath -ArgumentList $launchArguments -WorkingDirectory $gamePath -WindowStyle Hidden -PassThru
    $markerData['serverProcessId'] = $server.Id
    $markerData['port'] = $Port
    $markerData['rconPassword'] = $rconPassword
    $markerData['reportPath'] = $reportPath
    $markerData | ConvertTo-Json | Set-Content -LiteralPath $markerPath -Encoding UTF8
    Write-Host "HLDS запущен: PID $($server.Id), 127.0.0.1:$Port, $modPath"

    # HLDS может повторно показать созданную консоль, поэтому скрываем только его собственное окно
    $hideScript = @'
$ErrorActionPreference = 'SilentlyContinue'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class HLCoopCheckConsole {
    [DllImport("kernel32.dll")] public static extern bool FreeConsole();
    [DllImport("kernel32.dll")] public static extern bool AttachConsole(uint id);
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
}
"@
for ($attempt = 0; $attempt -lt 50; $attempt++) {
    [void][HLCoopCheckConsole]::FreeConsole()
    if ([HLCoopCheckConsole]::AttachConsole(__HLDS_PID__)) {
        [void][HLCoopCheckConsole]::ShowWindow([HLCoopCheckConsole]::GetConsoleWindow(), 0)
        [void][HLCoopCheckConsole]::FreeConsole()
    }
    Start-Sleep -Milliseconds 100
}
'@
    $hideScript = $hideScript.Replace('__HLDS_PID__', [string]$server.Id)
    $encodedHideScript = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($hideScript))
    $hideHelper = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList @('-NoProfile', '-NonInteractive', '-WindowStyle', 'Hidden', '-EncodedCommand', $encodedHideScript) -WindowStyle Hidden -PassThru

    $bootDeadline = [DateTime]::UtcNow.AddSeconds(60)
    $rconReady = $false
    while ([DateTime]::UtcNow -lt $bootDeadline -and !$server.HasExited)
    {
        try
        {
            $null = Invoke-CheckRcon -Command 'status'
            $rconReady = $true
            break
        }
        catch
        {
            Start-Sleep -Milliseconds 500
        }
    }
    if (!$rconReady)
    {
        throw "HLDS не ответил по RCON после запуска: проверьте $modPath\logs"
    }
    $startResponse = Invoke-CheckRcon -Command "hccheck_start $FirstIndex $Count $DwellSeconds"
    if ($startResponse -match '(?i)unknown command|bad rcon|bad challenge')
    {
        throw "Сервер не принял hccheck_start: $startResponse"
    }
    Write-Host "Проверка началась, журнал: $reportPath"
    $started = $true
}
finally
{
    if (!$started -and $null -ne $server -and !$server.HasExited)
    {
        try { $null = Invoke-CheckRcon -Command 'quit' } catch { }
        if (!$server.WaitForExit(5000))
        {
            # Завершаем только процесс, созданный этим запуском скрипта
            $server.Kill()
            $server.WaitForExit()
        }
    }
    if (!$started -and $null -ne $hideHelper -and !$hideHelper.HasExited)
    {
        $hideHelper.Kill()
    }
}

# Цикл карт продолжается в отдельном HLDS после выхода этого скрипта
[PSCustomObject]@{
    ServerProcessId = $server.Id
    CheckName = $CheckName
    FirstIndex = $FirstIndex
    Count = $Count
    DwellSeconds = $DwellSeconds
    HalfLifePath = $gamePath
    ModPath = $modPath
    Port = $Port
    MarkerPath = $markerPath
    ReportPath = $reportPath
    BuildPath = $build.BuildPath
}
