[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Подготовка разрешена только на одноразовой Windows-машине GitHub
if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_OS -cne 'Windows' -or
    $env:RUNNER_ENVIRONMENT -cne 'github-hosted')
{
    throw 'Подготовка Defender разрешена только на GitHub-hosted Windows runner'
}
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
{
    throw 'Для подготовки Defender нужны права администратора одноразового runner'
}

$passivePolicyPath = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Advanced Threat Protection'
$passivePolicyName = 'ForceDefenderPassiveMode'

function Write-DefenderPreparationState
{
    param([string]$Stage)
    $snapshot = [ordered]@{ stage = $Stage; imageOS = $env:ImageOS; imageVersion = $env:ImageVersion }
    try
    {
        $snapshot.status = Get-MpComputerStatus -ErrorAction Stop | Select-Object AMServiceEnabled,
            AntivirusEnabled, RealTimeProtectionEnabled, AMRunningMode, AMEngineVersion,
            AntivirusSignatureVersion, AntivirusSignatureLastUpdated, IsTamperProtected
    }
    catch { $snapshot.statusError = $_.Exception.Message }
    try
    {
        $snapshot.preferences = Get-MpPreference -ErrorAction Stop | Select-Object DisableRealtimeMonitoring,
            DisableBehaviorMonitoring, DisableIOAVProtection, DisableScriptScanning,
            DisableArchiveScanning, SignatureDisableUpdateOnStartupWithoutEngine,
            ExclusionPath, MAPSReporting, SubmitSamplesConsent
    }
    catch { $snapshot.preferencesError = $_.Exception.Message }
    try
    {
        $snapshot.service = Get-Service -Name WinDefend -ErrorAction Stop | Select-Object Name, Status, StartType
    }
    catch { $snapshot.serviceError = $_.Exception.Message }
    $snapshot.forceDefenderPassiveMode = Get-ItemPropertyValue -LiteralPath $passivePolicyPath -Name $passivePolicyName -ErrorAction SilentlyContinue
    $snapshot.disableAntiSpywarePolicy = Get-ItemPropertyValue -LiteralPath 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender' -Name DisableAntiSpyware -ErrorAction SilentlyContinue
    Write-Host ($snapshot | ConvertTo-Json -Depth 5 -Compress)
}

function Test-DefenderReleaseReadiness
{
    param($Status)
    return $null -ne $Status -and $Status.AMServiceEnabled -and $Status.AntivirusEnabled -and
        $Status.RealTimeProtectionEnabled -and $Status.AMRunningMode -ceq 'Normal' -and
        ![string]::IsNullOrWhiteSpace([string]$Status.AMEngineVersion) -and
        $Status.AMEngineVersion -ne '0.0.0.0' -and
        ![string]::IsNullOrWhiteSpace([string]$Status.AntivirusSignatureVersion) -and
        $Status.AntivirusSignatureVersion -ne '0.0.0.0'
}

try
{
    Write-DefenderPreparationState -Stage 'before'
    Get-Command Get-MpComputerStatus, Get-MpPreference, Set-MpPreference, Remove-MpPreference, Update-MpSignature -ErrorAction Stop | Out-Null

    # Возвращаем в активный режим только уже существующий флаг образа
    $passiveValue = Get-ItemPropertyValue -LiteralPath $passivePolicyPath -Name $passivePolicyName -ErrorAction SilentlyContinue
    if ($null -ne $passiveValue -and $passiveValue -eq 1)
    {
        Set-ItemProperty -LiteralPath $passivePolicyPath -Name $passivePolicyName -Value 0 -Type DWord -ErrorAction Stop
    }

    # Сначала поднимаем службу, иначе команды настроек могут завершиться до её запуска
    $service = Get-Service -Name WinDefend -ErrorAction Stop
    if ($service.Status -eq 'Stopped')
    {
        Start-Service -Name WinDefend -ErrorAction Stop
    }
    $service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(30))

    # Включаем компоненты защиты, отключённые при подготовке образа runner
    Set-MpPreference -DisableRealtimeMonitoring $false -DisableBehaviorMonitoring $false `
        -DisableIOAVProtection $false -DisableScriptScanning $false -DisableArchiveScanning $false `
        -SignatureDisableUpdateOnStartupWithoutEngine $false -ErrorAction Stop

    # Снимаем только два точных исключения корней из официального образа
    $preferences = Get-MpPreference -ErrorAction Stop
    foreach ($path in @($preferences.ExclusionPath))
    {
        if ($path -in @('C:\', 'D:\'))
        {
            Remove-MpPreference -ExclusionPath $path -ErrorAction Stop
        }
    }

    # Ожидание ограничено по времени, успешное выполнение команд не заменяет проверку состояния
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    $updated = $false
    $lastStatus = $null
    $lastStatusError = $null
    while ($true)
    {
        try
        {
            $lastStatus = Get-MpComputerStatus -ErrorAction Stop
            $lastStatusError = $null
        }
        catch
        {
            $lastStatus = $null
            $lastStatusError = $_.Exception.Message
        }

        if (!$updated -and $null -ne $lastStatus -and $lastStatus.AMServiceEnabled -and
            (Get-Service -Name WinDefend -ErrorAction Stop).Status -eq 'Running')
        {
            # Загружаем только базы, настройки MAPS и отправки образцов не меняются
            Update-MpSignature -UpdateSource MMPC -ErrorAction Stop
            $updated = $true
            $lastStatus = Get-MpComputerStatus -ErrorAction Stop
        }

        if ($updated -and (Test-DefenderReleaseReadiness -Status $lastStatus))
        {
            $signatureAge = [DateTime]::UtcNow - $lastStatus.AntivirusSignatureLastUpdated.ToUniversalTime()
            if ($signatureAge.TotalHours -gt 24 -or $signatureAge.TotalHours -lt -1)
            {
                throw 'После обновления базы Defender отсутствуют, устарели или имеют некорректную дату'
            }
            break
        }
        if ([DateTime]::UtcNow -ge $deadline)
        {
            $detail = if ($null -ne $lastStatusError) { " Ошибка состояния: $lastStatusError" } else { '' }
            throw "Defender не перешёл в Normal с активной защитой и доступным движком за 90 секунд.$detail"
        }
        Start-Sleep -Seconds 2
    }

    $preferences = Get-MpPreference -ErrorAction Stop
    if ($preferences.DisableRealtimeMonitoring -or $preferences.DisableBehaviorMonitoring -or
        $preferences.DisableIOAVProtection -or $preferences.DisableScriptScanning -or
        $preferences.DisableArchiveScanning -or $preferences.SignatureDisableUpdateOnStartupWithoutEngine -or
        @($preferences.ExclusionPath | Where-Object { $_ -in @('C:\', 'D:\') }).Count -gt 0)
    {
        throw 'Настройки защиты runner не подтвердились после подготовки'
    }
    Write-Host 'Defender включён, работает в Normal, базы обновлены; строгая проверка готового релиза остаётся обязательной'
}
finally
{
    Write-DefenderPreparationState -Stage 'after'
}
