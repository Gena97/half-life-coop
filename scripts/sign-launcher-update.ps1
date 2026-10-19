[CmdletBinding()]
param([string]$LauncherPath, [string]$OutputDirectory, [string]$PrivateKeyPath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'update-signing.ps1')
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $repoRoot 'artifacts' }
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if ([string]::IsNullOrWhiteSpace($LauncherPath)) { $LauncherPath = Join-Path $outputPath 'HLCoopLauncher.exe' }
$file = Get-Item -LiteralPath $LauncherPath
$version = $file.VersionInfo.ProductVersion
$metadata = Get-Content -LiteralPath (Join-Path $repoRoot 'game\hlcoop\liblist.gam') -Raw -Encoding UTF8
if ($version -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or
    $metadata -notmatch ('(?m)^version\s+"' + [regex]::Escape($version) + '"\s*$'))
{
    throw 'Версия EXE не соответствует версии исходников'
}
$config = Get-Content -LiteralPath (Join-Path $repoRoot 'game\hlcoop\update.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($config.repository -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$')
{
    throw 'В update.json не задан корректный репозиторий'
}
$public = Get-CoopUpdatePublicKey
$hash = (Get-FileHash -LiteralPath $LauncherPath -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = [ordered]@{
    schemaVersion = 1
    repository = $config.repository
    version = $version
    tag = "v$version"
    file = 'HLCoopLauncher.exe'
    bytes = $file.Length
    sha256 = $hash
    keyId = $public.keyId
}
$bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($manifest | ConvertTo-Json -Compress) + "`n")
$rsa = Open-CoopUpdateRsa -Private -PrivateKeyPath $PrivateKeyPath
try { $signature = $rsa.SignData($bytes, [Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256')) }
finally { $rsa.Dispose() }
[void][IO.Directory]::CreateDirectory($outputPath)
$manifestPath = Join-Path $outputPath 'launcher-update.json'
$signaturePath = Join-Path $outputPath 'launcher-update.sig'
[IO.File]::WriteAllBytes($manifestPath, $bytes)
[IO.File]::WriteAllBytes($signaturePath, $signature)
[IO.File]::WriteAllText("$LauncherPath.sha256", "$hash  HLCoopLauncher.exe`n", [Text.Encoding]::ASCII)
[void](Assert-CoopLauncherUpdateSignature -ManifestPath $manifestPath -SignaturePath $signaturePath -LauncherPath $LauncherPath)
Write-Host "Манифест обновления подписан: $manifestPath"
Write-Host "Ключ: $($public.keyId)"
