Set-StrictMode -Version Latest

function Get-CoopReleaseFiles
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [switch]$IncludeSymbols
    )

    $sourceRoot = [System.IO.Path]::GetFullPath($SourcePath).TrimEnd('\')
    $directories = New-Object 'System.Collections.Generic.Queue[System.IO.DirectoryInfo]'
    $directories.Enqueue((Get-Item -LiteralPath $sourceRoot -Force))
    while ($directories.Count -gt 0)
    {
        $directory = $directories.Dequeue()
        if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "В пакете обнаружена ссылка: $($directory.FullName)"
        }
        foreach ($entry in Get-ChildItem -LiteralPath $directory.FullName -Force)
        {
            if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                throw "В пакете обнаружена ссылка: $($entry.FullName)"
            }
            $relative = $entry.FullName.Substring($sourceRoot.Length + 1).Replace('\', '/')
            if ($relative -match '^(save|logs)(/|$)' -or $entry.Name -like '.hlcoop-*')
            {
                continue
            }
            if ($entry.PSIsContainer)
            {
                $directories.Enqueue($entry)
            }
            elseif ($relative -notin @('config.cfg', 'update-manifest.json') -and
                $entry.Extension -notin @('.log', '.dmp') -and
                ($IncludeSymbols -or $entry.Extension -ne '.pdb'))
            {
                [PSCustomObject]@{ File = $entry; Relative = $relative }
            }
        }
    }
}

function New-CoopReleaseManifest
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [switch]$IncludeSymbols
    )

    $metadata = Get-Content -LiteralPath (Join-Path $SourcePath 'liblist.gam') -Raw -Encoding UTF8
    $match = [regex]::Match($metadata, '(?m)^version\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (!$match.Success)
    {
        throw 'В liblist.gam ожидается версия из трёх чисел'
    }
    $files = @(Get-CoopReleaseFiles -SourcePath $SourcePath -IncludeSymbols:$IncludeSymbols | Sort-Object Relative)
    $manifest = [ordered]@{
        schemaVersion = 1
        version = $match.Groups[1].Value
        files = @($files | ForEach-Object {
            [ordered]@{
                path = $_.Relative
                sha256 = (Get-FileHash -LiteralPath $_.File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                bytes = $_.File.Length
            }
        })
    }
    $manifestPath = Join-Path $SourcePath 'update-manifest.json'
    if ((Test-Path -LiteralPath $manifestPath) -and
        ((Get-Item -LiteralPath $manifestPath -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw 'Манифест обновления не должен быть ссылкой'
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6) + "`r`n", $encoding)
    [PSCustomObject]@{ Version = $manifest.version; Files = $files; ManifestPath = $manifestPath }
}
