[CmdletBinding()]
param(
    [string]$HalfLifePath,

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateRange(1024, 65535)]
    [int]$Port = 28016,

    [ValidateSet('hlcoop_tc', 'hlcoop_tc_2', 'hlcoop_tc_3', 'hlcoop_tc_4')]
    [string]$CheckName = 'hlcoop_tc',

    [ValidateRange(0, 2147483647)]
    [int]$FirstIndex = 0,

    [ValidateRange(1, 2147483647)]
    [int]$Count = 1,

    [ValidateRange(2, 120)]
    [int]$DwellSeconds = 5,

    [ValidateSet(0, 1)]
    [int]$FarMode = 0,

    [string]$CatalogPath,

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scriptsPath = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptsPath
. (Join-Path $scriptsPath 'halflife-path.ps1')

if ([string]::IsNullOrWhiteSpace($CatalogPath))
{
    $CatalogPath = Join-Path $repoRoot 'artifacts\transition-check\transition-edges.tsv'
}
if (!(Test-Path -LiteralPath $CatalogPath -PathType Leaf))
{
    throw "Не найден каталог переходов: $CatalogPath"
}
$CatalogPath = [System.IO.Path]::GetFullPath((Get-Item -LiteralPath $CatalogPath).FullName)
$catalogRows = @(Get-Content -LiteralPath $CatalogPath -Encoding UTF8 | Where-Object {
    ![string]::IsNullOrWhiteSpace($_) -and !$_.StartsWith('#')
})
$edgeCount = $catalogRows.Count
if ($edgeCount -eq 0)
{
    throw 'Каталог переходов пуст'
}
for ($index = 0; $index -lt $edgeCount; $index++)
{
    $columns = $catalogRows[$index].Split([char]9)
    $edgeId = 0
    if ($columns.Length -ne 37 -or ![int]::TryParse($columns[0], [ref]$edgeId) -or $edgeId -ne $index)
    {
        throw "Неверная строка каталога переходов с индексом ${index}: ожидаются 37 полей и последовательный edgeId"
    }
}
if ($FirstIndex -ge $edgeCount)
{
    throw "FirstIndex выходит за пределы каталога из $edgeCount переходов"
}
if (!$PSBoundParameters.ContainsKey('Count'))
{
    $Count = $edgeCount - $FirstIndex
}
if ($Count -gt $edgeCount - $FirstIndex)
{
    throw "Диапазон FirstIndex и Count выходит за пределы $edgeCount переходов"
}
if (!$PSBoundParameters.ContainsKey('Port') -and $CheckName -match '_([2-4])$')
{
    $Port = 28016 + ([int]$Matches[1] - 1) * 100
}

$gamePath = Resolve-HalfLifePath -HalfLifePath $HalfLifePath
$serverPath = Join-Path $gamePath 'hlds.exe'
$sourcePath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'game\hlcoop'))
$modPath = [System.IO.Path]::GetFullPath((Join-Path $gamePath $CheckName))
$markerPath = Join-Path $modPath '.hlcoop-transition-check.json'
$metadataPath = Join-Path $modPath 'transition-check-meta.json'
$projectId = 'half-life_coop/transition-check'
$reportPath = Join-Path $modPath 'transition-check.jsonl'
$reportDirectory = Join-Path $repoRoot 'artifacts\transition-check'
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
    farMode = $FarMode
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
            $entry.Name -in @('config.cfg', '.hlcoop-install.json', '.hlcoop-check.json', '.hlcoop-transition-check.json',
                'map-check.jsonl', 'transition-check.jsonl', 'transition-check-meta.json'))
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

# Каталог и метаданные относятся только к этому изолированному прогону
$installedCatalogPath = Join-Path $modPath 'transition-edges.tsv'
Assert-CheckDestination -Path $installedCatalogPath
Copy-Item -LiteralPath $CatalogPath -Destination $installedCatalogPath -Force
Assert-CheckDestination -Path $metadataPath
$metadataData = [ordered]@{
    project = $projectId
    format = 1
    checkName = $CheckName
    firstIndex = $FirstIndex
    count = $Count
    dwellSeconds = $DwellSeconds
    farMode = $FarMode
    catalogEdges = $edgeCount
    catalogSha256 = (Get-FileHash -LiteralPath $installedCatalogPath -Algorithm SHA256).Hash.ToLowerInvariant()
    serverDllSha256 = (Get-FileHash -LiteralPath (Join-Path $modPath 'dlls\hl.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    clientDllSha256 = (Get-FileHash -LiteralPath (Join-Path $modPath 'cl_dlls\client.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    preparedAtUtc = [DateTime]::UtcNow.ToString('o')
}
$metadataData | ConvertTo-Json | Set-Content -LiteralPath $metadataPath -Encoding UTF8

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

    # Наблюдаем только созданный HLDS до его завершения и сохраняем текст его аварийного диалога
    $runtimeDiagnosticPath = Join-Path $modPath '.runtime-dialogs.private.jsonl'
    Assert-CheckDestination -Path $runtimeDiagnosticPath
    $serverStartTicks = $server.StartTime.ToUniversalTime().Ticks
    $hideScript = @'
$ErrorActionPreference = 'Stop'
$ownedId = __HLDS_PID__
$ownedStart = [long]__HLDS_START_TICKS__
$ownedPath = '__HLDS_PATH__'
$diagnosticPath = '__DIAGNOSTIC_PATH__'
function Write-PrivateDiagnostic {
    param([System.Collections.IDictionary]$Entry)
    $Entry['utc'] = [DateTime]::UtcNow.ToString('o')
    $Entry['processId'] = $ownedId
    $Entry['processStartTicks'] = $ownedStart
    [System.IO.File]::AppendAllText($diagnosticPath, (($Entry | ConvertTo-Json -Depth 5 -Compress) + "`r`n"), (New-Object System.Text.UTF8Encoding -ArgumentList $false))
}
$stage = 'compile_helper'
try {
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class HLCoopCheckConsole {
    public sealed class IdentityResult {
        public bool Matches;
        public string Reason = "";
        public int Win32Error;
        public long ActualStartTicks;
        public string ActualPath = "";
        public uint ExitCode;
        public bool ExitCodeAvailable;
        public uint WaitResult = 0xFFFFFFFF;
        public int WaitError;
        public bool HandleSignaled;
    }
    [StructLayout(LayoutKind.Sequential)] struct FileTime { public uint Low, High; }
    public delegate bool EnumWindow(IntPtr window, IntPtr parameter);
    [DllImport("kernel32.dll")] public static extern bool FreeConsole();
    [DllImport("kernel32.dll")] public static extern bool AttachConsole(uint id);
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int id);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetProcessTimes(IntPtr process, out FileTime created, out FileTime exited, out FileTime kernel, out FileTime user);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder path, ref int count);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindow callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumWindow callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr word, StringBuilder text, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr word, IntPtr parameter);
    public static IdentityResult Identity(int id, long started, string path) {
        IdentityResult result = new IdentityResult();
        IntPtr process = OpenProcess(0x101000, false, id);
        if (process == IntPtr.Zero) {
            result.Reason = "open_process_failed"; result.Win32Error = Marshal.GetLastWin32Error(); return result;
        }
        try {
            FileTime created, exited, kernel, user;
            if (!GetProcessTimes(process, out created, out exited, out kernel, out user)) {
                result.Reason = "creation_time_failed"; result.Win32Error = Marshal.GetLastWin32Error(); return result;
            }
            result.ActualStartTicks = DateTime.FromFileTimeUtc((long)(((ulong)created.High << 32) | created.Low)).Ticks;
            StringBuilder actualPath = new StringBuilder(32768); int count = actualPath.Capacity;
            if (!QueryFullProcessImageName(process, 0, actualPath, ref count)) {
                result.Reason = "image_path_failed"; result.Win32Error = Marshal.GetLastWin32Error(); return result;
            }
            result.ActualPath = actualPath.ToString();
            result.ExitCodeAvailable = GetExitCodeProcess(process, out result.ExitCode);
            result.WaitResult = WaitForSingleObject(process, 0);
            if (result.WaitResult == 0xFFFFFFFF) result.WaitError = Marshal.GetLastWin32Error();
            result.HandleSignaled = result.WaitResult == 0;
            if (result.ActualStartTicks != started) result.Reason = "start_time_mismatch";
            else if (!String.Equals(result.ActualPath, path, StringComparison.OrdinalIgnoreCase)) result.Reason = "image_path_mismatch";
            else { result.Matches = true; result.Reason = "matched"; }
        } catch (Exception error) {
            result.Reason = "identity_exception:" + error.GetType().Name; result.Win32Error = error.HResult;
        } finally { CloseHandle(process); }
        return result;
    }
    public static bool Owns(int id, long started, string path) { return Identity(id, started, path).Matches; }
    static string Text(IntPtr window, bool className) {
        StringBuilder text = new StringBuilder(16384);
        if (className) GetClassName(window, text, text.Capacity); else GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }
    static bool OwnedWindow(IntPtr window, int id) {
        uint owner; GetWindowThreadProcessId(window, out owner); return owner == (uint)id;
    }
    public static IntPtr[] RuntimeDialogs(int id) {
        List<IntPtr> result = new List<IntPtr>();
        EnumWindows(delegate(IntPtr window, IntPtr parameter) {
            if (OwnedWindow(window, id) && Text(window, true) == "#32770" &&
                String.Equals(Text(window, false), "Microsoft Visual C++ Runtime Library", StringComparison.OrdinalIgnoreCase))
                result.Add(window);
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static string[] ChildTexts(IntPtr dialog, int id) {
        List<string> result = new List<string>();
        EnumChildWindows(dialog, delegate(IntPtr window, IntPtr parameter) {
            if (OwnedWindow(window, id)) {
                StringBuilder text = new StringBuilder(16384); UIntPtr received;
                SendMessageTimeout(window, 0x000D, new UIntPtr((uint)text.Capacity), text, 0x22, 250, out received);
                if (text.Length > 0) result.Add(text.ToString());
            }
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static bool AbortOwned(IntPtr dialog, int id, long started, string path) {
        if (!Owns(id, started, path) || !OwnedWindow(dialog, id) || Text(dialog, true) != "#32770" ||
            !String.Equals(Text(dialog, false), "Microsoft Visual C++ Runtime Library", StringComparison.OrdinalIgnoreCase)) return false;
        IntPtr button = GetDlgItem(dialog, 3);
        return button != IntPtr.Zero && OwnedWindow(button, id) && Text(button, true) == "Button" &&
            PostMessage(dialog, 0x0111, new IntPtr(3), button);
    }
}
"@
$observedDialogs = @{}
$stage = 'observe_owned_process'
Write-PrivateDiagnostic -Entry ([ordered]@{ event = 'helper_started'; expectedPath = $ownedPath })
while ($true) {
    $identity = [HLCoopCheckConsole]::Identity($ownedId, $ownedStart, $ownedPath)
    if (!$identity.Matches) {
        Write-PrivateDiagnostic -Entry ([ordered]@{
            event = 'helper_stopped'; reason = $identity.Reason; win32Error = $identity.Win32Error
            actualStartTicks = $identity.ActualStartTicks; actualPath = $identity.ActualPath
        })
        break
    }
    $dialogs = @([HLCoopCheckConsole]::RuntimeDialogs($ownedId))
    if ($identity.HandleSignaled) {
        Write-PrivateDiagnostic -Entry ([ordered]@{ event = 'helper_stopped'; reason = 'process_handle_signaled'; waitResult = $identity.WaitResult; exitCode = $identity.ExitCode })
        break
    }
    [void][HLCoopCheckConsole]::FreeConsole()
    if ([HLCoopCheckConsole]::AttachConsole($ownedId)) {
        if ([HLCoopCheckConsole]::Owns($ownedId, $ownedStart, $ownedPath)) {
            [void][HLCoopCheckConsole]::ShowWindow([HLCoopCheckConsole]::GetConsoleWindow(), 0)
        }
        [void][HLCoopCheckConsole]::FreeConsole()
    }
    foreach ($dialog in $dialogs) {
        if (![HLCoopCheckConsole]::Owns($ownedId, $ownedStart, $ownedPath)) { break }
        $childTexts = @([HLCoopCheckConsole]::ChildTexts($dialog, $ownedId))
        $dialogKey = [string]$dialog.ToInt64() + ':' + ($childTexts -join '|')
        if ($observedDialogs.ContainsKey($dialogKey)) { continue }
        $entry = [ordered]@{
            event = 'runtime_dialog'
            window = $dialog.ToInt64()
            title = 'Microsoft Visual C++ Runtime Library'
            childTexts = $childTexts
            action = 'request_abort_owned_runtime_dialog'
            exitCode = $identity.ExitCode
            exitCodeAvailable = $identity.ExitCodeAvailable
            waitResult = $identity.WaitResult
            waitError = $identity.WaitError
        }
        Write-PrivateDiagnostic -Entry $entry
        $observedDialogs[$dialogKey] = $true
        $abortPosted = [HLCoopCheckConsole]::AbortOwned($dialog, $ownedId, $ownedStart, $ownedPath)
        Write-PrivateDiagnostic -Entry ([ordered]@{ event = 'runtime_dialog_abort_result'; window = $dialog.ToInt64(); posted = $abortPosted })
    }
    Start-Sleep -Milliseconds 250
}
} catch {
    try { Write-PrivateDiagnostic -Entry ([ordered]@{
        event = 'helper_error'; stage = $stage; exceptionType = $_.Exception.GetType().FullName; errorCode = $_.Exception.HResult
    }) } catch { }
}
'@
    $hideScript = $hideScript.Replace('__HLDS_PID__', [string]$server.Id)
    $hideScript = $hideScript.Replace('__HLDS_START_TICKS__', [string]$serverStartTicks)
    $hideScript = $hideScript.Replace('__HLDS_PATH__', $serverPath.Replace("'", "''"))
    $hideScript = $hideScript.Replace('__DIAGNOSTIC_PATH__', $runtimeDiagnosticPath.Replace("'", "''"))
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
    $startResponse = Invoke-CheckRcon -Command "hctcheck_start $FirstIndex $Count $DwellSeconds $FarMode"
    if ($startResponse -match '(?i)unknown command|bad rcon|bad challenge')
    {
        throw "Сервер не принял hctcheck_start: $startResponse"
    }
    $startRecord = Get-Content -LiteralPath $reportPath -TotalCount 1 -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $startRecord -or $startRecord.type -ne 'start')
    {
        throw 'hctcheck_start не создал начальную запись отчёта'
    }
    $metadataData['runId'] = $startRecord.run
    $metadataData | ConvertTo-Json | Set-Content -LiteralPath $metadataPath -Encoding UTF8
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

# Цикл переходов продолжается в отдельном HLDS после выхода этого скрипта
[PSCustomObject]@{
    ServerProcessId = $server.Id
    CheckName = $CheckName
    FirstIndex = $FirstIndex
    Count = $Count
    DwellSeconds = $DwellSeconds
    FarMode = $FarMode
    HalfLifePath = $gamePath
    ModPath = $modPath
    Port = $Port
    MarkerPath = $markerPath
    MetadataPath = $metadataPath
    ReportPath = $reportPath
    CatalogEdges = $edgeCount
    CatalogPath = $installedCatalogPath
    BuildPath = $build.BuildPath
}
