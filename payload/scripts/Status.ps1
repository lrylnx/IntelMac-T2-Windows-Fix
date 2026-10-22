. "$PSScriptRoot\Common.ps1"
$target=Get-Target
$status='None';$version='';$state=$null
if(Test-Path $script:StatePath){try{$state=Read-State;$status=$state.Status;$version=$state.Version}catch{$status='Unreadable'}}
$camera='';if(Test-Path $script:CamKey){$camera=(Get-Item $script:CamKey).GetValue('')}
$task=Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
$taskResult=$null;if($task){$taskResult=(Get-ScheduledTaskInfo -TaskName $script:TaskName).LastTaskResult}
$p=Get-Item 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power'
$fast=$p.GetValue('HiberbootEnabled',$null)
$base=Split-Path $PSScriptRoot -Parent
$report=Get-ChildItem -LiteralPath (Join-Path $base 'reports') -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$powerBackup=Join-Path $env:ProgramData 'IntelMacT2Shutdown\shutdown-state.json'
$powerPending=$false
if(Test-Path $powerBackup){try{$powerState=Get-Content $powerBackup -Raw -Encoding UTF8 | ConvertFrom-Json;$powerPending=$powerState.Status -ne 'Restored'}catch{}}
[pscustomobject]@{Model=$target.Model;OS=$target.OS;Build=$target.Build;Supported=$target.Supported;InstallStatus=$status;PatchVersion=$version;CameraPath=$camera;TaskPresent=($null -ne $task);TaskResult=$taskResult;FastStartup=$fast;PowerBackup=$powerPending;NewestReport=$(if($report){$report.FullName}else{''})} | ConvertTo-Json -Depth 4 -Compress
