[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$VisualStudioPath,

    [string]$WindowsSdkVersion,

    [switch]$MapCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# Используем установленные инструменты Visual Studio без изменения проектов SDK
if ([string]::IsNullOrWhiteSpace($VisualStudioPath))
{
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswherePath -PathType Leaf))
    {
        throw 'Не найден vswhere.exe: установите Visual Studio 2022 или Build Tools 2022 с компонентом «Разработка классических приложений на C++» либо передайте -VisualStudioPath'
    }

    $VisualStudioPath = & $vswherePath -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($VisualStudioPath))
    {
        throw 'Не найдена Visual Studio 2022 с компилятором C++: добавьте компонент «Разработка классических приложений на C++» через Visual Studio Installer'
    }
}

$msbuildPath = Join-Path $VisualStudioPath 'MSBuild\Current\Bin\MSBuild.exe'
$toolsetPath = Join-Path $VisualStudioPath 'MSBuild\Microsoft\VC\v170\Platforms\Win32\PlatformToolsets\v143'
if (!(Test-Path -LiteralPath $msbuildPath -PathType Leaf) -or !(Test-Path -LiteralPath $toolsetPath -PathType Container))
{
    throw "В $VisualStudioPath отсутствуют MSBuild или MSVC v143: добавьте инструменты C++ для x86/x64 через Visual Studio Installer"
}

# GoldSrc использует 32-битные библиотеки независимо от разрядности Windows
$kitsKey = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10 -ErrorAction SilentlyContinue
$kitsRoot = if ($null -ne $kitsKey) { $kitsKey.KitsRoot10 } else { Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$sdkIncludePath = Join-Path $kitsRoot 'Include'
if (!(Test-Path -LiteralPath $sdkIncludePath -PathType Container))
{
    throw 'Не найден Windows SDK: добавьте Windows 10 SDK или Windows 11 SDK через Visual Studio Installer'
}

$availableSdks = @(Get-ChildItem -LiteralPath $sdkIncludePath -Directory | Where-Object {
    $_.Name -match '^10\.0\.\d+\.\d+$' -and
    (Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $kitsRoot "Lib\$($_.Name)\um\x86\kernel32.lib") -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $kitsRoot "Lib\$($_.Name)\ucrt\x86\ucrt.lib") -PathType Leaf)
} | Sort-Object { [version]$_.Name } -Descending)

if ([string]::IsNullOrWhiteSpace($WindowsSdkVersion))
{
    if ($availableSdks.Count -eq 0)
    {
        throw 'Не найден полный Windows SDK с библиотеками x86: добавьте Windows SDK через Visual Studio Installer'
    }
    $WindowsSdkVersion = $availableSdks[0].Name
}
elseif ($WindowsSdkVersion -notin @($availableSdks | ForEach-Object { $_.Name }))
{
    throw "Windows SDK $WindowsSdkVersion не установлен или не содержит библиотек x86"
}

$requiredFiles = @(
    'projects\vs2019\hldll.vcxproj',
    'projects\vs2019\hl_cdll.vcxproj',
    'utils\vgui\lib\win32_vc16\vgui.lib',
    'lib\public\sdl2.lib',
    'lib\public\game_controls.lib'
)
foreach ($relativePath in $requiredFiles)
{
    if (!(Test-Path -LiteralPath (Join-Path $repoRoot $relativePath) -PathType Leaf))
    {
        throw "Отсутствует файл SDK ${relativePath}: проверьте полноту исходников Half-Life SDK"
    }
}

$buildRoot = if ($MapCheck) { Join-Path $repoRoot "build\MapCheck\$Configuration" } else { Join-Path $repoRoot "build\$Configuration" }
$targets = @(
    @{ Project = 'hldll'; Binary = 'hl'; Destination = 'dlls' },
    @{ Project = 'hl_cdll'; Binary = 'client'; Destination = 'cl_dlls' }
)

Write-Host "Сборка $Configuration, Win32, MSVC v143, Windows SDK $WindowsSdkVersion"
foreach ($target in $targets)
{
    $outputPath = Join-Path $buildRoot $target.Project
    $intermediatePath = Join-Path $outputPath 'obj'
    New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
    $projectPath = Join-Path $repoRoot "projects\vs2019\$($target.Project).vcxproj"
    $logPath = Join-Path $outputPath 'build.log'

    # Отключаем старое копирование за пределы репозитория и складываем результаты локально
    $buildArguments = @(
        $projectPath,
        '/nologo',
        '/m',
        '/clp:ErrorsOnly;Summary',
        "/p:Configuration=$Configuration",
        '/p:Platform=Win32',
        '/p:PlatformToolset=v143',
        "/p:WindowsTargetPlatformVersion=$WindowsSdkVersion",
        '/p:PostBuildEventUseInBuild=false',
        "/p:OutDir=$outputPath/",
        "/p:IntDir=$intermediatePath/",
        '/fl',
        "/flp:logfile=$logPath;verbosity=normal"
    )
    if ($MapCheck)
    {
        $buildArguments += '/p:CoopMapCheck=true'
    }
    Write-Host "Собираем $($target.Project)"
    & $msbuildPath @buildArguments | Out-Host
    if ($LASTEXITCODE -ne 0)
    {
        throw "Сборка $($target.Project) завершилась с ошибкой: $logPath"
    }
    if (!(Test-Path -LiteralPath (Join-Path $outputPath "$($target.Binary).dll") -PathType Leaf))
    {
        throw "После сборки отсутствует $($target.Binary).dll: $logPath"
    }
}

# Проверочные библиотеки не попадают в обычный пакет мода
if ($MapCheck)
{
    Write-Host "Проверочная сборка готова: $buildRoot"
    [PSCustomObject]@{
        Configuration = $Configuration
        MapCheck = $true
        BuildPath = $buildRoot
        BinaryPaths = [PSCustomObject]@{
            ServerDll = Join-Path $buildRoot 'hldll\hl.dll'
            ServerPdb = Join-Path $buildRoot 'hldll\hl.pdb'
            ClientDll = Join-Path $buildRoot 'hl_cdll\client.dll'
            ClientPdb = Join-Path $buildRoot 'hl_cdll\client.pdb'
        }
    }
    return
}

# Обновляем пакет мода только после успешной сборки обеих библиотек
foreach ($target in $targets)
{
    $outputPath = Join-Path $buildRoot $target.Project
    $destinationPath = Join-Path $repoRoot "game\hlcoop\$($target.Destination)"
    New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $outputPath "$($target.Binary).dll") -Destination $destinationPath -Force
    $symbolsPath = Join-Path $outputPath "$($target.Binary).pdb"
    if (Test-Path -LiteralPath $symbolsPath -PathType Leaf)
    {
        Copy-Item -LiteralPath $symbolsPath -Destination $destinationPath -Force
    }
}

Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $repoRoot 'game\hlcoop\LICENSE') -Force

Write-Host "Готово: $(Join-Path $repoRoot 'game\hlcoop')"
