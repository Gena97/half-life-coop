[CmdletBinding()]
param(
    [string]$PackagePath,
    [string]$OutputDirectory,
    [string]$VisualStudioPath,
    [string]$WindowsSdkVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourcePath = Join-Path $repoRoot 'launcher'
$metadata = Get-Content -LiteralPath (Join-Path $repoRoot 'game\hlcoop\liblist.gam') -Raw -Encoding UTF8
$versionMatch = [regex]::Match($metadata, '(?m)^version\s+"([0-9]+\.[0-9]+\.[0-9]+)"\s*$')
if (!$versionMatch.Success)
{
    throw 'В liblist.gam не найдена версия мода в формате X.Y.Z'
}
$version = $versionMatch.Groups[1].Value
$versionParts = @($version.Split('.') | ForEach-Object { [int]$_ })
if (@($versionParts | Where-Object { $_ -gt 65535 }).Count -gt 0)
{
    throw 'Части версии должны помещаться в VERSIONINFO Windows: от 0 до 65535'
}
if ([string]::IsNullOrWhiteSpace($PackagePath))
{
    $PackagePath = Join-Path $repoRoot "artifacts\hlcoop-$version-win32.zip"
}
$PackagePath = [System.IO.Path]::GetFullPath($PackagePath)
if (!(Test-Path -LiteralPath $PackagePath -PathType Leaf))
{
    throw "Не найден архив мода ${PackagePath}: сначала выполните scripts\package.ps1"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory))
{
    $OutputDirectory = Join-Path $repoRoot 'artifacts'
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
$buildPath = Join-Path $repoRoot 'build\Launcher\Release'
foreach ($requiredPath in @(
    (Join-Path $sourcePath 'Launcher.cpp'),
    (Join-Path $sourcePath 'launcher.manifest'),
    (Join-Path $sourcePath 'bootstrap.ps1'),
    (Join-Path $PSScriptRoot 'halflife-path.ps1')
))
{
    if (!(Test-Path -LiteralPath $requiredPath -PathType Leaf))
    {
        throw "Не найден исходный файл: $requiredPath"
    }
}

# Не встраиваем архив другой версии по случайно переданному пути
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($PackagePath)
try
{
    $entry = $archive.GetEntry('hlcoop/liblist.gam')
    if ($null -eq $entry -or $entry.Length -gt 65536)
    {
        throw 'В архиве отсутствует корректный hlcoop/liblist.gam'
    }
    $reader = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8, $true)
    try
    {
        $packageMetadata = $reader.ReadToEnd()
    }
    finally
    {
        $reader.Dispose()
    }
    $packageVersion = [regex]::Match($packageMetadata, '(?m)^version\s+"([0-9]+\.[0-9]+\.[0-9]+)"\s*$')
    if (!$packageVersion.Success -or $packageVersion.Groups[1].Value -cne $version)
    {
        throw "Версия архива не совпадает с версией исходников $version"
    }
}
finally
{
    $archive.Dispose()
}

# Используем тот же установленный MSVC v143, что и для библиотек мода
if ([string]::IsNullOrWhiteSpace($VisualStudioPath))
{
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswherePath -PathType Leaf))
    {
        throw 'Не найден vswhere.exe: установите Visual Studio 2022 с компонентом «Разработка классических приложений на C++» либо передайте -VisualStudioPath'
    }
    $VisualStudioPath = & $vswherePath -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($VisualStudioPath))
    {
        throw 'Не найдены инструменты C++ Visual Studio 2022 для x86/x64'
    }
}
$msbuildPath = Join-Path $VisualStudioPath 'MSBuild\Current\Bin\MSBuild.exe'
$toolsetPath = Join-Path $VisualStudioPath 'MSBuild\Microsoft\VC\v170\Platforms\Win32\PlatformToolsets\v143'
if (!(Test-Path -LiteralPath $msbuildPath -PathType Leaf) -or !(Test-Path -LiteralPath $toolsetPath -PathType Container))
{
    throw "В $VisualStudioPath отсутствуют MSBuild или MSVC v143"
}
$kitsKey = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10 -ErrorAction SilentlyContinue
$kitsRoot = if ($null -ne $kitsKey) { $kitsKey.KitsRoot10 } else { Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$sdkIncludePath = Join-Path $kitsRoot 'Include'
if (!(Test-Path -LiteralPath $sdkIncludePath -PathType Container))
{
    throw 'Не найден Windows SDK: установите Windows 10 SDK или Windows 11 SDK'
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
        throw 'Не найден полный Windows SDK с библиотеками x86'
    }
    $WindowsSdkVersion = $availableSdks[0].Name
}
elseif ($WindowsSdkVersion -notin @($availableSdks | ForEach-Object { $_.Name }))
{
    throw "Windows SDK $WindowsSdkVersion не установлен или не содержит библиотек x86"
}

New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($true)
$packageHash = (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash.ToLowerInvariant()
$updateConfig = Get-Content -LiteralPath (Join-Path $repoRoot 'game\hlcoop\update.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$repository = [string]$updateConfig.repository
if ($repository -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$')
{
    throw 'В update.json нужен адрес репозитория в формате владелец/репозиторий'
}
$bootstrap = [System.IO.File]::ReadAllText((Join-Path $sourcePath 'bootstrap.ps1'), [System.Text.Encoding]::UTF8)
$pathResolver = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'halflife-path.ps1'), [System.Text.Encoding]::UTF8).TrimStart([char]0xFEFF)
foreach ($placeholder in @('__HLC_PACKAGE_SHA256__', '__HLC_PACKAGE_VERSION__', '__HLC_REPOSITORY__', '# __HLC_PATH_RESOLVER__'))
{
    if (!$bootstrap.Contains($placeholder))
    {
        throw "В шаблоне bootstrap.ps1 отсутствует маркер $placeholder"
    }
}
$bootstrap = $bootstrap.Replace('__HLC_PACKAGE_SHA256__', $packageHash).
    Replace('__HLC_PACKAGE_VERSION__', $version).
    Replace('__HLC_REPOSITORY__', $repository).
    Replace('# __HLC_PATH_RESOLVER__', $pathResolver)
if ($bootstrap -match '__HLC_[A-Z0-9_]+__')
{
    throw 'В bootstrap.ps1 остались незаполненные маркеры сборки'
}

# Windows PowerShell 5.1 читает русский текст из файла по BOM UTF-8
[System.IO.File]::WriteAllText((Join-Path $buildPath 'bootstrap.ps1'), $bootstrap, $utf8)
[System.IO.File]::WriteAllText((Join-Path $buildPath 'launcher-version.h'), "#pragma once`r`n#define HLC_LAUNCHER_VERSION `"$version`"`r`n", $utf8)
Copy-Item -LiteralPath $PackagePath -Destination (Join-Path $buildPath 'payload.zip') -Force
$launcherManifest = New-Object System.Xml.XmlDocument
$launcherManifest.PreserveWhitespace = $true
$launcherManifest.Load((Join-Path $sourcePath 'launcher.manifest'))
$manifestIdentity = $launcherManifest.SelectSingleNode("/*[local-name()='assembly']/*[local-name()='assemblyIdentity']")
if ($null -eq $manifestIdentity)
{
    throw 'В launcher.manifest отсутствует assemblyIdentity'
}
$manifestIdentity.SetAttribute('version', "$version.0")
[System.IO.File]::WriteAllText((Join-Path $buildPath 'launcher.manifest'), $launcherManifest.OuterXml, $utf8)
$versionNumbers = ($versionParts + @(0)) -join ','
$resourceText = @"
#include <windows.h>
101 RCDATA "payload.zip"
102 RCDATA "bootstrap.ps1"
CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST "launcher.manifest"
1 VERSIONINFO
FILEVERSION $versionNumbers
PRODUCTVERSION $versionNumbers
FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
FILEFLAGS 0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "CompanyName", "HalfLifeCoop\0"
            VALUE "FileDescription", "Half-Life Co-op Launcher\0"
            VALUE "FileVersion", "$version.0\0"
            VALUE "InternalName", "HLCoopLauncher\0"
            VALUE "OriginalFilename", "HLCoopLauncher.exe\0"
            VALUE "ProductName", "Half-Life Co-op\0"
            VALUE "ProductVersion", "$version\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0409, 1200
    END
END
"@
[System.IO.File]::WriteAllText((Join-Path $buildPath 'launcher.rc'), $resourceText, $utf8)

# Временный проект собирает один EXE и не меняет проекты Half-Life SDK
$escapedSource = [System.Security.SecurityElement]::Escape((Join-Path $sourcePath 'Launcher.cpp'))
$projectText = @"
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|Win32">
      <Configuration>Release</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{87E4ED16-5AD5-4632-8157-033758BC65A8}</ProjectGuid>
    <Keyword>Win32Proj</Keyword>
    <WindowsTargetPlatformVersion>$WindowsSdkVersion</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <PropertyGroup>
    <TargetName>HLCoopLauncher</TargetName>
    <GenerateManifest>false</GenerateManifest>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <WarningLevel>Level4</WarningLevel>
      <Optimization>MaxSpeed</Optimization>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <LanguageStandard>stdcpp17</LanguageStandard>
      <ExceptionHandling>Sync</ExceptionHandling>
      <PreprocessorDefinitions>UNICODE;_UNICODE;WIN32;NDEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ForcedIncludeFiles>`$(ProjectDir)launcher-version.h;%(ForcedIncludeFiles)</ForcedIncludeFiles>
      <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>
    </ClCompile>
    <ResourceCompile>
      <AdditionalIncludeDirectories>`$(ProjectDir);%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ResourceCompile>
    <Link>
      <SubSystem>Windows</SubSystem>
      <GenerateManifest>false</GenerateManifest>
      <GenerateDebugInformation>false</GenerateDebugInformation>
      <AdditionalDependencies>user32.lib;gdi32.lib;shell32.lib;ole32.lib;advapi32.lib;comctl32.lib;version.lib;uuid.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="$escapedSource" />
    <ResourceCompile Include="launcher.rc" />
  </ItemGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@
$projectPath = Join-Path $buildPath 'launcher.vcxproj'
[System.IO.File]::WriteAllText($projectPath, $projectText, $utf8)
$logPath = Join-Path $buildPath 'build.log'
Write-Host "Сборка HLCoopLauncher $version, Win32, MSVC v143, Windows SDK $WindowsSdkVersion"
$buildArguments = @(
    $projectPath,
    '/nologo',
    '/m',
    '/clp:ErrorsOnly;Summary',
    '/p:Configuration=Release',
    '/p:Platform=Win32',
    "/p:OutDir=$outputPath/",
    "/p:IntDir=$buildPath/obj/",
    '/fl',
    "/flp:logfile=$logPath;verbosity=normal"
)
& $msbuildPath @buildArguments | Out-Host
if ($LASTEXITCODE -ne 0)
{
    throw "Сборка лаунчера завершилась с ошибкой: $logPath"
}
$launcherPath = Join-Path $outputPath 'HLCoopLauncher.exe'
if (!(Test-Path -LiteralPath $launcherPath -PathType Leaf))
{
    throw "После сборки отсутствует $launcherPath"
}
$launcherHash = (Get-FileHash -LiteralPath $launcherPath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$launcherPath.sha256"
[System.IO.File]::WriteAllText($checksumPath, "$launcherHash  HLCoopLauncher.exe`n", [System.Text.Encoding]::ASCII)
Write-Host "Готово: $launcherPath"
Write-Host "SHA256: $launcherHash"
Write-Host "Контрольная сумма: $checksumPath"
Get-Item -LiteralPath $launcherPath | Select-Object FullName, Length
