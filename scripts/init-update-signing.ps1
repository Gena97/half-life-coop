[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'update-signing.ps1')
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$privateDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'HalfLifeCoopDeveloper'
$privatePath = [IO.Path]::GetFullPath((Join-Path $privateDirectory 'update-signing-key.xml'))
$publicPath = Join-Path $repoRoot 'launcher\update-public-key.json'
if ($privatePath.StartsWith($repoRoot + '\', [StringComparison]::OrdinalIgnoreCase))
{
    throw 'Закрытый ключ должен находиться за пределами репозитория'
}
if (Test-Path -LiteralPath $privatePath -PathType Leaf)
{
    if (!(Test-Path -LiteralPath $publicPath -PathType Leaf))
    {
        throw 'Закрытый ключ уже существует, а открытый отсутствует: восстановите соответствующий открытый ключ, новый ключ автоматически не создаётся'
    }
    $existing = Open-CoopUpdateRsa -Private -PrivateKeyPath $privatePath
    $existing.Dispose()
}
else
{
    if (Test-Path -LiteralPath $publicPath)
    {
        throw 'Открытый ключ уже существует: создание другого закрытого ключа запрещено'
    }
    [void][IO.Directory]::CreateDirectory($privateDirectory)
    if (((Get-Item -LiteralPath $privateDirectory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw 'Каталог закрытого ключа не должен быть ссылкой'
    }
    # Доступ к закрытому ключу получают только текущий пользователь и SYSTEM
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($identity in @([Security.Principal.WindowsIdentity]::GetCurrent().User, (New-Object Security.Principal.SecurityIdentifier('S-1-5-18'))))
    {
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        [void]$acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $privateDirectory -AclObject $acl
    $rsa = New-Object Security.Cryptography.RSACryptoServiceProvider(3072)
    $rsa.PersistKeyInCsp = $false
    try
    {
        $parameters = $rsa.ExportParameters($false)
        $public = [ordered]@{
            algorithm = 'RSA-SHA256'
            keyId = Get-CoopUpdateKeyId -Modulus $parameters.Modulus -Exponent $parameters.Exponent
            modulus = [Convert]::ToBase64String($parameters.Modulus)
            exponent = [Convert]::ToBase64String($parameters.Exponent)
        }
        $stream = [IO.File]::Open($privatePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try
        {
            $privateBytes = [Text.Encoding]::UTF8.GetBytes($rsa.ToXmlString($true))
            $stream.Write($privateBytes, 0, $privateBytes.Length)
            $stream.Flush($true)
            [Array]::Clear($privateBytes, 0, $privateBytes.Length)
        }
        finally { $stream.Dispose() }
        $fileAcl = New-Object Security.AccessControl.FileSecurity
        $fileAcl.SetAccessRuleProtection($true, $false)
        foreach ($identity in @([Security.Principal.WindowsIdentity]::GetCurrent().User, (New-Object Security.Principal.SecurityIdentifier('S-1-5-18'))))
        {
            [void]$fileAcl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($identity, 'FullControl', 'Allow')))
        }
        Set-Acl -LiteralPath $privatePath -AclObject $fileAcl
        $publicBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($public | ConvertTo-Json) + "`n")
        $publicStream = [IO.File]::Open($publicPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $publicStream.Write($publicBytes, 0, $publicBytes.Length) }
        finally { $publicStream.Dispose() }
    }
    finally { $rsa.Dispose() }
}
$public = Get-CoopUpdatePublicKey
[PSCustomObject]@{
    KeyId = $public.keyId
    PublicKeyPath = $publicPath
    PublicKeySha256 = (Get-FileHash -LiteralPath $publicPath -Algorithm SHA256).Hash.ToLowerInvariant()
    PrivateKeyPath = $privatePath
}
