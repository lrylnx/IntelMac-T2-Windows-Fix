param([ValidateSet('Status','Apply','Restore','Diagnose','MarkShutdown')][string]$Action='Status')
. "$PSScriptRoot\Common.ps1"
$powerPath='HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power'
$script:DataRoot=Join-Path $env:ProgramData 'IntelMacT2Shutdown'
$script:StatePath=Join-Path $script:DataRoot 'shutdown-state.json'
function Registry-Snapshot {
    $key=Get-Item -LiteralPath $powerPath
    $present='HiberbootEnabled' -in $key.GetValueNames()
    [pscustomobject]@{Present=$present;Value=$(if($present){$key.GetValue('HiberbootEnabled')}else{$null});Kind=$(if($present){$key.GetValueKind('HiberbootEnabled').ToString()}else{$null})}
}
if($Action -eq 'Status') {
    $value=Registry-Snapshot
    [pscustomobject]@{FastStartup=$value;HasBackup=(Test-Path $script:StatePath);AutoReboot=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl').AutoReboot} | ConvertTo-Json -Depth 4
    return
}
Require-Admin
if(-not (Get-Target).Supported){throw '此选项仅适用于受支持的 Intel T2 MacBook / Windows 10。'}
if((Test-Path $script:DataRoot) -and ((Get-Item $script:DataRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw '关机备份目录包含重定向链接，请联系维护者。'}
if($Action -eq 'Apply') {
    if(-not (Test-Path $script:DataRoot)){Secure-Folder $script:DataRoot}
    $before=Registry-Snapshot
    if($before.Present -and ($before.Kind -ne 'DWord' -or $before.Value -notin @(0,1))){throw '快速启动注册值类型异常，请先采集诊断。'}
    if(Test-Path $script:StatePath){
        $state=Read-State
        if($state.Status -eq 'Restored'){
            Move-Item -LiteralPath $script:StatePath -Destination (Join-Path $script:DataRoot ('backup-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'.json'))
            $state=$null
        }
    } else {$state=$null}
    if(-not $state){$state=[pscustomobject]@{Version='0.1';Status='Preparing';Original=$before;Time=(Get-Date -Format o)};Save-State $state}
    New-ItemProperty -LiteralPath $powerPath -Name HiberbootEnabled -Value 0 -PropertyType DWord -Force | Out-Null
    if((Get-ItemProperty $powerPath).HiberbootEnabled -ne 0){throw '快速启动设置核验失败。'}
    $state.Status='Applied';Save-State $state
    Write-Host '快速启动已关闭。已保存原设置，可单独恢复。完整关机后再测试。'
    Write-Host '这项设置不能保证消除固件、电源或电池故障造成的自动启动和耗电。'
    return
}
if($Action -eq 'Restore') {
    if(-not(Test-Path $script:StatePath)){Write-Host '没有此关机选项的备份，未修改系统。';return}
    $state=Read-State
    if($state.Original.Present){
        New-ItemProperty -LiteralPath $powerPath -Name HiberbootEnabled -Value ([int]$state.Original.Value) -PropertyType DWord -Force | Out-Null
    } else {Remove-ItemProperty -LiteralPath $powerPath -Name HiberbootEnabled -ErrorAction SilentlyContinue}
    $after=Registry-Snapshot
    if($after.Present -ne $state.Original.Present -or ($after.Present -and $after.Value -ne $state.Original.Value)){throw '恢复原设置核验失败。'}
    $state.Status='Restored';Save-State $state
    Write-Host '已恢复启用此关机选项前的快速启动设置。'
    return
}
$report=Join-Path (Split-Path $PSScriptRoot -Parent) ('reports\power-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $report -Force | Out-Null
$os=Get-CimInstance Win32_OperatingSystem
$snapshot=[pscustomobject]@{Time=(Get-Date -Format o);Machine=(Get-Target);LastBoot=$os.LastBootUpTime;FastStartup=(Registry-Snapshot);CrashControl=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' | Select-Object AutoReboot,CrashDumpEnabled);Battery=@(Get-CimInstance Win32_Battery | Select-Object EstimatedChargeRemaining,BatteryStatus)}
$snapshot | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $report 'power-state.json') -Encoding UTF8
foreach($nativeArgs in @(@('/a'),@('/devicequery','wake_armed'),@('/lastwake'),@('/waketimers'),@('/requests'))){
    try {$name=($nativeArgs -join '-').Replace('/','');Run-Native powercfg.exe $nativeArgs | Set-Content (Join-Path $report ($name+'.txt')) -Encoding UTF8}catch{"FAILED: $_" | Set-Content (Join-Path $report ('failure-'+[guid]::NewGuid().ToString()+'.txt')) -Encoding UTF8}
}
try {Get-WinEvent -FilterHashtable @{LogName='System';Id=12,13,41,42,45,107,1074,6005,6006,6008,1001;StartTime=(Get-Date).AddDays(-14)} -MaxEvents 250 | Select-Object TimeCreated,ProviderName,Id,Message | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $report 'shutdown-events.json') -Encoding UTF8}catch{"FAILED or no events: $_" | Set-Content (Join-Path $report 'events-error.txt') -Encoding UTF8}
if($Action -eq 'MarkShutdown'){'Full shutdown requested by the user; this is a request marker, not proof of reaching S5.' | Set-Content (Join-Path $report 'shutdown-request.txt') -Encoding UTF8}
Write-Host "REPORT=$report"
Write-Host '关机记录已保存；不会自动上传。事件记录不能单独证明主板已经断电。'
