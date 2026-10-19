[CmdletBinding()]
param([string]$LauncherPath, [string]$TimestampServer = 'http://timestamp.digicert.com')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($LauncherPath)) { $LauncherPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts\HLCoopLauncher.exe' }
if ([string]::IsNullOrWhiteSpace($env:HLC_AUTHENTICODE_PFX_BASE64))
{
    Write-Host 'Сертификат Authenticode не настроен: EXE остаётся без подписи издателя Windows'
    return
}
$certificate = $null
$pfx = $null
try
{
    $pfx = [Convert]::FromBase64String($env:HLC_AUTHENTICODE_PFX_BASE64)
    $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($pfx, $env:HLC_AUTHENTICODE_PFX_PASSWORD, [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::UserKeySet)
    if (!$certificate.HasPrivateKey -or $certificate.NotAfter -lt [DateTime]::Now -or $certificate.NotBefore -gt [DateTime]::Now)
    {
        throw 'Сертификат Authenticode просрочен, ещё не действует или не содержит закрытый ключ'
    }
    if (![string]::IsNullOrWhiteSpace($env:HLC_AUTHENTICODE_THUMBPRINT) -and
        $certificate.Thumbprint -ine $env:HLC_AUTHENTICODE_THUMBPRINT.Replace(' ', ''))
    {
        throw 'Отпечаток сертификата Authenticode не совпал с настройкой'
    }
    $result = Set-AuthenticodeSignature -LiteralPath $LauncherPath -Certificate $certificate -HashAlgorithm SHA256 -TimestampServer $TimestampServer
    $verified = Get-AuthenticodeSignature -LiteralPath $LauncherPath
    if ($result.Status -ne 'Valid' -or $verified.Status -ne 'Valid' -or
        $verified.SignerCertificate.Thumbprint -ine $certificate.Thumbprint)
    {
        throw 'Подпись Authenticode не прошла проверку доверия Windows'
    }
    Write-Host 'Подпись Authenticode проверена Windows'
}
finally
{
    if ($null -ne $certificate) { $certificate.Dispose() }
    if ($null -ne $pfx) { [Array]::Clear($pfx, 0, $pfx.Length) }
}
