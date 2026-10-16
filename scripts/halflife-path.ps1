function Resolve-HalfLifePath
{
    [CmdletBinding()]
    param(
        [string]$HalfLifePath
    )

    function Get-HalfLifeDirectoryPath
    {
        param(
            [string]$Path,

            [switch]$RequireAbsolute
        )

        if ([string]::IsNullOrWhiteSpace($Path))
        {
            return $null
        }
        if ($RequireAbsolute -and $Path -notmatch '^(?:[A-Za-z]:[\\/]|[\\/]{2}[^\\/]+[\\/][^\\/]+(?:[\\/]|$))')
        {
            return $null
        }

        try
        {
            $directory = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
            if (!$directory.PSIsContainer -or $directory.PSProvider.Name -ne 'FileSystem')
            {
                return $null
            }
            return [System.IO.Path]::GetFullPath($directory.FullName)
        }
        catch
        {
            return $null
        }
    }

    function Test-HalfLifeInstallation
    {
        param(
            [string]$Path
        )

        return (
            (Test-Path -LiteralPath (Join-Path $Path 'hl.exe') -PathType Leaf -ErrorAction SilentlyContinue) -and
            (Test-Path -LiteralPath (Join-Path $Path 'valve\liblist.gam') -PathType Leaf -ErrorAction SilentlyContinue)
        )
    }

    function Read-HalfLifeVdfPairs
    {
        param(
            [string]$Path
        )

        try
        {
            $content = [System.IO.File]::ReadAllText($Path)
        }
        catch
        {
            return
        }

        # Читаем строки отдельно от скобок и комментариев, сохраняя экранирование VDF
        $tokens = [regex]::Matches($content, '//[^\r\n]*|"(?:\\.|[^"\\])*"|[{}]')
        $pendingKey = $null
        foreach ($token in $tokens)
        {
            $value = $token.Value
            if ($value.StartsWith('//'))
            {
                continue
            }
            if (!$value.StartsWith('"'))
            {
                $pendingKey = $null
                continue
            }

            $value = $value.Substring(1, $value.Length - 2)
            $value = [regex]::Replace($value, '\\([\\"])', '$1')
            if ($null -eq $pendingKey)
            {
                $pendingKey = $value
            }
            else
            {
                [pscustomobject]@{ Key = $pendingKey; Value = $value }
                $pendingKey = $null
            }
        }
    }

    if (![string]::IsNullOrWhiteSpace($HalfLifePath))
    {
        $resolvedPath = Get-HalfLifeDirectoryPath -Path $HalfLifePath
        if ($null -eq $resolvedPath)
        {
            throw "Не удалось открыть папку Half-Life: $HalfLifePath — проверьте путь и права доступа"
        }
        if (!(Test-HalfLifeInstallation -Path $resolvedPath))
        {
            throw "В папке $resolvedPath не найдены hl.exe и valve\liblist.gam: передайте -HalfLifePath с путём установленной Half-Life"
        }
        return $resolvedPath
    }

    $steamRoots = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $registrySources = @(
        @{ Path = 'HKCU:\Software\Valve\Steam'; Name = 'SteamPath' },
        @{ Path = 'HKCU:\Software\Valve\Steam'; Name = 'SteamExe' },
        @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Name = 'InstallPath' },
        @{ Path = 'HKLM:\SOFTWARE\Valve\Steam'; Name = 'InstallPath' }
    )
    foreach ($source in $registrySources)
    {
        try
        {
            $registryValue = Get-ItemPropertyValue -LiteralPath $source.Path -Name $source.Name -ErrorAction Stop
            if ($registryValue -isnot [string] -or [string]::IsNullOrWhiteSpace($registryValue))
            {
                continue
            }
            $candidate = [System.Environment]::ExpandEnvironmentVariables($registryValue)
            if ($source.Name -eq 'SteamExe')
            {
                $candidate = Split-Path -Parent $candidate
            }
            $steamRoot = Get-HalfLifeDirectoryPath -Path $candidate -RequireAbsolute
            if ($null -ne $steamRoot)
            {
                [void]$steamRoots.Add($steamRoot)
            }
        }
        catch
        {
            continue
        }
    }

    # Стандартные каталоги доступны и при отсутствии записей Steam в реестре
    foreach ($environmentName in @('ProgramFiles(x86)', 'ProgramFiles'))
    {
        $programFilesPath = [System.Environment]::GetEnvironmentVariable($environmentName)
        if (![string]::IsNullOrWhiteSpace($programFilesPath))
        {
            $steamRoot = Get-HalfLifeDirectoryPath -Path (Join-Path $programFilesPath 'Steam') -RequireAbsolute
            if ($null -ne $steamRoot)
            {
                [void]$steamRoots.Add($steamRoot)
            }
        }
    }

    $libraryRoots = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($steamRoot in $steamRoots)
    {
        [void]$libraryRoots.Add($steamRoot)
        $libraryFoldersPath = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        foreach ($entry in @(Read-HalfLifeVdfPairs -Path $libraryFoldersPath))
        {
            # Новые файлы содержат ключ path, старые используют числовые ключи библиотек
            if ($entry.Key -ne 'path' -and $entry.Key -notmatch '^\d+$')
            {
                continue
            }
            $libraryRoot = Get-HalfLifeDirectoryPath -Path $entry.Value -RequireAbsolute
            if ($null -ne $libraryRoot)
            {
                [void]$libraryRoots.Add($libraryRoot)
            }
        }
    }

    $installations = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($libraryRoot in $libraryRoots)
    {
        $manifestPath = Join-Path $libraryRoot 'steamapps\appmanifest_70.acf'
        $installDirectories = @(Read-HalfLifeVdfPairs -Path $manifestPath | Where-Object { $_.Key -eq 'installdir' })
        if ($installDirectories.Count -ne 1)
        {
            continue
        }

        $installDirectory = $installDirectories[0].Value
        # Имя установки должно указывать только на один дочерний каталог steamapps\common
        if ([string]::IsNullOrWhiteSpace($installDirectory) -or
            $installDirectory -ne $installDirectory.Trim() -or
            $installDirectory.EndsWith('.') -or
            $installDirectory.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0)
        {
            continue
        }

        $commonPath = Join-Path $libraryRoot 'steamapps\common'
        $candidate = Get-HalfLifeDirectoryPath -Path (Join-Path $commonPath $installDirectory) -RequireAbsolute
        if ($null -ne $candidate -and (Test-HalfLifeInstallation -Path $candidate))
        {
            [void]$installations.Add($candidate)
        }
    }

    if ($installations.Count -eq 0)
    {
        throw 'Установленная Half-Life не найдена в библиотеках Steam: установите Half-Life или передайте -HalfLifePath с путём папки, содержащей hl.exe и valve\liblist.gam'
    }
    if ($installations.Count -gt 1)
    {
        $installationList = ($installations | Sort-Object | ForEach-Object { "  $_" }) -join [System.Environment]::NewLine
        throw "Найдено несколько установок Half-Life:$([System.Environment]::NewLine)$installationList$([System.Environment]::NewLine)Укажите нужную папку через -HalfLifePath"
    }

    foreach ($installation in $installations)
    {
        return $installation
    }
}
