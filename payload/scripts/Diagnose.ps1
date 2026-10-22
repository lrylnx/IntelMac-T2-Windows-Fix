. "$PSScriptRoot\Common.ps1"
$report=Join-Path (Split-Path $PSScriptRoot -Parent) ('reports\'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $report -Force | Out-Null
(Get-Target) | ConvertTo-Json | Set-Content (Join-Path $report 'machine.json') -Encoding UTF8
Get-CimInstance Win32_PnPSignedDriver | Where-Object {$_.DeviceName -match 'Apple|Bluetooth|FaceTime|Broadcom'} | Select-Object DeviceName,DeviceID,DriverProviderName,DriverVersion,DriverDate,InfName | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $report 'drivers.json') -Encoding UTF8
Get-CimInstance Win32_PnPEntity | Where-Object {$_.ConfigManagerErrorCode -ne 0 -or $_.Name -match 'Apple|Bluetooth|FaceTime|Broadcom'} | Select-Object Name,PNPDeviceID,Service,ConfigManagerErrorCode | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $report 'devices.json') -Encoding UTF8
foreach($option in @('a','getactivescheme','requests','waketimers','lastwake','query')){
    try { $text=Run-Native powercfg.exe @("/$option");$text | Set-Content (Join-Path $report "power-$option.txt") -Encoding UTF8 } catch { "FAILED: $_" | Set-Content (Join-Path $report "power-$option.txt") -Encoding UTF8 }
}
try {Run-Native powercfg.exe @('/batteryreport','/output',(Join-Path $report 'battery.html')) | Out-Null}catch{"FAILED: $_" | Set-Content (Join-Path $report 'battery-error.txt') -Encoding UTF8}
$providers=@('Microsoft-Windows-Kernel-Power','Microsoft-Windows-Power-Troubleshooter','Microsoft-Windows-Kernel-PnP')
try {Get-WinEvent -FilterHashtable @{LogName='System';StartTime=(Get-Date).AddDays(-7);ProviderName=$providers} -MaxEvents 300 | Select-Object TimeCreated,ProviderName,Id,LevelDisplayName,Message | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $report 'events.json') -Encoding UTF8}catch{"FAILED or no events: $_" | Set-Content (Join-Path $report 'events-error.txt') -Encoding UTF8}
Get-ScheduledTask | Where-Object {$_.TaskName -match 'IntelMacT2Patch|SleepResume-BluetoothRepair'} | Select-Object TaskName,State,Actions,Triggers,Settings | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $report 'tasks.json') -Encoding UTF8
if(Test-Path $script:StatePath){Copy-Item $script:StatePath $report}
if(Test-Path (Join-Path $script:DataRoot 'patch.log')){Copy-Item (Join-Path $script:DataRoot 'patch.log') $report}
Write-Host "诊断已保存：$report。不会开启摄像头，不会自动上传。" -ForegroundColor Green
