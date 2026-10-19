Set-StrictMode -Version Latest

function Get-CoopUpdateKeyId
{
    param([byte[]]$Modulus, [byte[]]$Exponent)
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try
    {
        $bytes = New-Object byte[] ($Modulus.Length + $Exponent.Length)
        [Array]::Copy($Modulus, 0, $bytes, 0, $Modulus.Length)
        [Array]::Copy($Exponent, 0, $bytes, $Modulus.Length, $Exponent.Length)
        return 'rsa3072-' + [BitConverter]::ToString($hash.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant().Substring(0, 32)
    }
    finally { $hash.Dispose() }
}

function Get-CoopUpdatePublicKey
{
    param([string]$Path = (Join-Path (Split-Path -Parent $PSScriptRoot) 'launcher\update-public-key.json'))
    $data = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    $modulus = [Convert]::FromBase64String([string]$data.modulus)
    $exponent = [Convert]::FromBase64String([string]$data.exponent)
    if ($data.algorithm -cne 'RSA-SHA256' -or $modulus.Length -ne 384 -or
        [Convert]::ToBase64String($exponent) -cne 'AQAB' -or
        $data.keyId -cne (Get-CoopUpdateKeyId -Modulus $modulus -Exponent $exponent))
    {
        throw 'Некорректный открытый ключ обновлений: требуется RSA 3072 / SHA256'
    }
    return $data
}

function Open-CoopUpdateRsa
{
    param([switch]$Private, [string]$PrivateKeyPath, [string]$PublicKeyPath)
    $public = if ([string]::IsNullOrWhiteSpace($PublicKeyPath)) { Get-CoopUpdatePublicKey } else { Get-CoopUpdatePublicKey -Path $PublicKeyPath }
    $rsa = New-Object System.Security.Cryptography.RSACryptoServiceProvider
    $rsa.PersistKeyInCsp = $false
    try
    {
        if ($Private)
        {
            if (![string]::IsNullOrWhiteSpace($env:HLC_UPDATE_PRIVATE_KEY_BASE64))
            {
                $xml = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($env:HLC_UPDATE_PRIVATE_KEY_BASE64))
            }
            else
            {
                if ([string]::IsNullOrWhiteSpace($PrivateKeyPath))
                {
                    $PrivateKeyPath = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'HalfLifeCoopDeveloper\update-signing-key.xml'
                }
                $repositoryRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
                $PrivateKeyPath = [IO.Path]::GetFullPath($PrivateKeyPath)
                if ($PrivateKeyPath.StartsWith($repositoryRoot + '\', [StringComparison]::OrdinalIgnoreCase))
                {
                    throw 'Закрытый ключ не должен находиться в репозитории'
                }
                if (!(Test-Path -LiteralPath $PrivateKeyPath -PathType Leaf))
                {
                    throw 'Не задан ключ подписи: добавьте HLC_UPDATE_PRIVATE_KEY_BASE64 в GitHub Secrets или создайте локальный ключ через init-update-signing.ps1'
                }
                $xml = [IO.File]::ReadAllText($PrivateKeyPath, [Text.Encoding]::UTF8)
            }
            try { $rsa.FromXmlString($xml) }
            catch { throw 'Не удалось прочитать закрытый ключ подписи обновлений' }
            finally { $xml = $null }
            if ($rsa.PublicOnly) { throw 'Вместо закрытого ключа передан открытый' }
        }
        else
        {
            $parameters = New-Object System.Security.Cryptography.RSAParameters
            $parameters.Modulus = [Convert]::FromBase64String([string]$public.modulus)
            $parameters.Exponent = [Convert]::FromBase64String([string]$public.exponent)
            $rsa.ImportParameters($parameters)
        }
        $actual = $rsa.ExportParameters($false)
        if ($rsa.KeySize -ne 3072 -or [Convert]::ToBase64String($actual.Modulus) -cne $public.modulus -or
            [Convert]::ToBase64String($actual.Exponent) -cne $public.exponent)
        {
            throw 'Закрытый ключ не соответствует открытому ключу, встроенному в запускатель'
        }
        return $rsa
    }
    catch
    {
        $rsa.Dispose()
        throw
    }
}

function Assert-CoopLauncherUpdateSignature
{
    param([string]$ManifestPath, [string]$SignaturePath, [string]$LauncherPath)
    $bytes = [IO.File]::ReadAllBytes($ManifestPath)
    $signature = [IO.File]::ReadAllBytes($SignaturePath)
    if ($bytes.Length -gt 16384 -or $signature.Length -ne 384 -or
        ($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191))
    {
        throw 'Некорректный формат подписанного манифеста или подписи'
    }
    $rsa = Open-CoopUpdateRsa
    try
    {
        if (!$rsa.VerifyData($bytes, [Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'), $signature))
        {
            throw 'Подпись манифеста обновления не прошла проверку'
        }
    }
    finally { $rsa.Dispose() }
    $manifest = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes) | ConvertFrom-Json
    $public = Get-CoopUpdatePublicKey
    $file = Get-Item -LiteralPath $LauncherPath
    $version = $file.VersionInfo.ProductVersion
    if ($manifest.schemaVersion -ne 1 -or $manifest.keyId -cne $public.keyId -or
        $manifest.file -cne 'HLCoopLauncher.exe' -or $manifest.version -cne $version -or
        $manifest.tag -cne "v$version" -or $manifest.repository -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$' -or
        $manifest.bytes -ne $file.Length -or $manifest.sha256 -cnotmatch '^[a-f0-9]{64}$' -or
        $manifest.sha256 -cne (Get-FileHash -LiteralPath $LauncherPath -Algorithm SHA256).Hash.ToLowerInvariant())
    {
        throw 'Подписанный манифест не соответствует готовому EXE'
    }
    return $manifest
}
