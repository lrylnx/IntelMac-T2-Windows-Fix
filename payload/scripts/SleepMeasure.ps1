param([ValidateSet('Start','End')][string]$Phase)
. "$PSScriptRoot\Common.ps1"
$dir=Join-Path (Split-Path $PSScriptRoot -Parent) 'reports'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$path=Join-Path $dir 'sleep-start.json'
$battery=@(Get-CimInstance Win32_Battery)
if($battery.Count -ne 1){throw '无法读取唯一电池。'}
$now=[pscustomobject]@{Time=(Get-Date -Format o);Percent=$battery[0].EstimatedChargeRemaining;BatteryStatus=$battery[0].BatteryStatus;Plan=(Active-Plan)}
if($Phase -eq 'Start') {
    $now | ConvertTo-Json | Set-Content $path -Encoding UTF8
    Write-Host '已记下开始电量。请拔掉充电器，记录当前电量，手动合盖测试；唤醒后运行结束测量。程序不会自行睡眠。'
} else {
    if(-not(Test-Path $path)){throw '请先运行开始测量。'}
    $start=Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json
    $hours=([datetime]$now.Time-[datetime]$start.Time).TotalHours
    if($hours -le 0){throw '时间间隔无效。'}
    $result=[pscustomobject]@{Start=$start;End=$now;ElapsedHours=[math]::Round($hours,3);DropPercentagePoints=([double]$start.Percent-[double]$now.Percent);PercentagePointsPerHour=[math]::Round(([double]$start.Percent-[double]$now.Percent)/$hours,2);Caution='Includes awake time. Invalid if charging occurred. Battery percentage is approximate; correlate with sleep events.'}
    $result | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $dir ('sleep-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.json')) -Encoding UTF8
    $result | Format-List
    Write-Host '本数值含首尾清醒时间；中途充电会使结果无效。请同时运行诊断，核对睡眠与唤醒事件。'
}
