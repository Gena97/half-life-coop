[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackagePath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$allowlist = @(
    'autoexec.cfg', 'campaign.cfg', 'campaign.json', 'cl_dlls/client.dll',
    'continueauto.cfg', 'continuecoop.cfg', 'coop.cfg', 'CSBotConfig.vdf', 'dlls/hl.dll',
    'game.cfg', 'liblist.gam', 'LICENSE', 'listenserver.cfg', 'mapcycle.txt', 'motd.txt',
    'newcampaign.cfg', 'README.txt', 'resource/CreateMultiplayerGameServerPage.res',
    'resource/GameMenu.res', 'server.cfg', 'settings.scr'
)
$excluded = @('start-hlcoop.cmd', 'update-hlcoop.ps1', 'update.json', 'update-manifest.json')
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($outputPath)
$work = Join-Path $outputPath ('payload-input-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($work)
$archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($PackagePath))
try
{
    $entries = @{}
    foreach ($entry in $archive.Entries)
    {
        if (!$entry.FullName.StartsWith('hlcoop/', [StringComparison]::Ordinal) -or
            $entry.FullName.Length -le 7 -or $entry.Length -gt 67108864)
        {
            throw 'Архив содержит недопустимый файл или размер'
        }
        $path = $entry.FullName.Substring(7)
        if ($entries.ContainsKey($path) -or ($path -cnotin $allowlist -and $path -cnotin $excluded))
        {
            throw "Неожиданный или повторяющийся путь в архиве: $path"
        }
        $entries[$path] = $entry
    }
    if ($entries.Count -ne ($allowlist.Count + $excluded.Count)) { throw 'В архиве отсутствуют обязательные файлы' }
    $reader = New-Object IO.StreamReader($entries['update-manifest.json'].Open(), [Text.Encoding]::UTF8, $true)
    try { $oldManifest = $reader.ReadToEnd() | ConvertFrom-Json }
    finally { $reader.Dispose() }
    if ($oldManifest.schemaVersion -ne 1 -or $oldManifest.version -cne $Version -or $oldManifest.files.Count -ne 24)
    {
        throw 'Некорректный манифест исходного ZIP'
    }
    $sourceHashes = @{}
    foreach ($file in $oldManifest.files)
    {
        if ($sourceHashes.ContainsKey([string]$file.path) -or !$entries.ContainsKey([string]$file.path))
        {
            throw 'Повторяющийся или отсутствующий путь в манифесте ZIP'
        }
        $sourceHashes[[string]$file.path] = $file
    }
    $files = @()
    $directives = @('.OPTION EXPLICIT', '.Set CabinetNameTemplate=payload.cab', '.Set DiskDirectoryTemplate=.', '.Set Cabinet=ON', '.Set Compress=ON', '.Set CompressionType=MSZIP', '.Set MaxDiskSize=0', '.Set MaxCabinetSize=0')
    $index = 0
    foreach ($path in $allowlist)
    {
        if (!$entries.ContainsKey($path) -or !$sourceHashes.ContainsKey($path)) { throw "Не найден $path" }
        $cabinetName = 'f{0:d4}' -f $index
        $destination = Join-Path $work $cabinetName
        $inputStream = $entries[$path].Open()
        $outputStream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew)
        try { $inputStream.CopyTo($outputStream) }
        finally { $outputStream.Dispose(); $inputStream.Dispose() }
        $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        $length = (Get-Item -LiteralPath $destination).Length
        if ($hash -cne $sourceHashes[$path].sha256 -or $length -ne $sourceHashes[$path].bytes)
        {
            throw "Файл не совпадает с манифестом ZIP: $path"
        }
        $files += [ordered]@{ cabinetName = $cabinetName; path = $path; bytes = $length; sha256 = $hash }
        $directives += "`"$cabinetName`" `"$cabinetName`""
        $index++
    }
}
finally { $archive.Dispose() }
$ddfPath = Join-Path $work 'payload.ddf'
[IO.File]::WriteAllLines($ddfPath, $directives, [Text.Encoding]::ASCII)
$makecab = Join-Path $env:SystemRoot 'System32\makecab.exe'
Push-Location -LiteralPath $work
try
{
    $cabinetOutput = & $makecab /F $ddfPath 2>&1
    if ($LASTEXITCODE -ne 0)
    {
        $cabinetOutput | Out-Host
        throw 'Не удалось создать CAB MSZIP'
    }
}
finally { Pop-Location }
$cabinetPath = Join-Path $outputPath 'payload.cab'
Copy-Item -LiteralPath (Join-Path $work 'payload.cab') -Destination $cabinetPath -Force
$manifestPath = Join-Path $outputPath 'payload.json'
$manifest = [ordered]@{ schemaVersion = 2; version = $Version; files = @($files) }
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6) + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Host "CAB MSZIP готов: $($files.Count) файлов, $((Get-Item -LiteralPath $cabinetPath).Length) байт"
[PSCustomObject]@{ CabinetPath = $cabinetPath; ManifestPath = $manifestPath; FileCount = $files.Count }
