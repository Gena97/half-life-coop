[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [switch]$IncludeSymbols
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-manifest.ps1')
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourcePath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'game\hlcoop'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory))
{
    $OutputDirectory = Join-Path $repoRoot 'artifacts'
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
if ($outputPath.Equals($sourcePath, [System.StringComparison]::OrdinalIgnoreCase) -or
    $outputPath.StartsWith($sourcePath.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase))
{
    throw 'Каталог архива должен находиться за пределами папки готового мода'
}

foreach ($relativePath in @('liblist.gam', 'LICENSE', 'dlls\hl.dll', 'cl_dlls\client.dll', 'coop.cfg', 'update-hlcoop.ps1', 'update.json', 'start-hlcoop.cmd'))
{
    if (!(Test-Path -LiteralPath (Join-Path $sourcePath $relativePath) -PathType Leaf))
    {
        throw "Отсутствует ${relativePath}: сначала выполните scripts\build.ps1"
    }
}
$metadata = Get-Content -LiteralPath (Join-Path $sourcePath 'liblist.gam') -Raw
$versionMatch = [regex]::Match($metadata, '(?m)^version\s+"([0-9]+\.[0-9]+(?:\.[0-9]+)?)"')
if (!$versionMatch.Success)
{
    throw 'В liblist.gam не найдена версия мода'
}
$version = $versionMatch.Groups[1].Value
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$archivePath = Join-Path $outputPath "hlcoop-$version-win32.zip"
$temporaryPath = "$archivePath.tmp"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Манифест описывает все файлы архива, кроме самого себя
$release = New-CoopReleaseManifest -SourcePath $sourcePath -IncludeSymbols:$IncludeSymbols
$files = @($release.Files) + @([PSCustomObject]@{
    File = Get-Item -LiteralPath $release.ManifestPath
    Relative = 'update-manifest.json'
})

$stream = [System.IO.File]::Open($temporaryPath, [System.IO.FileMode]::Create)
try
{
    $archive = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create, $true)
    try
    {
        foreach ($entry in $files)
        {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $entry.File.FullName, "hlcoop/$($entry.Relative)",
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
    finally
    {
        $archive.Dispose()
    }
}
finally
{
    $stream.Dispose()
}
Move-Item -LiteralPath $temporaryPath -Destination $archivePath -Force
$hash = Get-FileHash -LiteralPath $archivePath -Algorithm SHA256
$checksumPath = "$archivePath.sha256"
[System.IO.File]::WriteAllText($checksumPath, $hash.Hash.ToLowerInvariant() + '  ' + [System.IO.Path]::GetFileName($archivePath) + "`n", [System.Text.Encoding]::ASCII)
Write-Host "Пакет готов: $archivePath"
Write-Host "SHA256: $($hash.Hash)"
Write-Host "Контрольная сумма: $checksumPath"
Get-Item -LiteralPath $archivePath | Select-Object FullName, Length
