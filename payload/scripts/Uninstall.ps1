param([switch]$Rollback)
. "$PSScriptRoot\Common.ps1"
Require-Admin
if(-not (Test-Path $script:StatePath)){Write-Host '没有本补丁的安装记录。';exit 0}
Assert-NoCameraApps
$state=Read-State
$errors=@()
if($state.TaskTouched){try{if(Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue){Stop-ScheduledTask -TaskName $script:TaskName;Unregister-ScheduledTask -TaskName $script:TaskName -Confirm:$false};$state.TaskTouched=$false;Save-State $state}catch{$errors+="任务：$_"}}
if($state.LegacyTouched){try{Enable-ScheduledTask -TaskName 'SleepResume-BluetoothRepair' | Out-Null;$state.LegacyTouched=$false;Save-State $state}catch{$errors+="旧任务：$_"}}
if($state.CameraTouched){try{
    Camera-Registration (Join-Path $script:InstallRoot 'bin\VirtualCam.dll') -Remove
    if($state.OldCamera){Camera-Registration $state.OldCamera;if((Get-Item $script:CamKey).GetValue('') -ne $state.OldCamera){throw '旧注册恢复校验失败'}}
    $state.CameraTouched=$false;Save-State $state
}catch{$errors+="摄像头：$_"}}
if($state.PlanTouched){try{
    $all=Run-Native powercfg.exe @('/list')
    if($all -notmatch [regex]::Escape($state.OriginalPlan)){Run-Native powercfg.exe @('/import',(Join-Path $script:DataRoot 'original-power.pow'),$state.OriginalPlan) | Out-Null}
    if((Active-Plan) -eq $state.PatchPlan){Run-Native powercfg.exe @('/setactive',$state.OriginalPlan) | Out-Null}
    if($all -match [regex]::Escape($state.PatchPlan)){Run-Native powercfg.exe @('/delete',$state.PatchPlan) | Out-Null}
    $state.PlanTouched=$false;Save-State $state
}catch{$errors+="电源：$_"}}
if($errors.Count){$state.Status='RollbackIncomplete';Save-State $state;throw ($errors -join "`n")}
$state.Status='Uninstalled';Save-State $state
# Keep backups/logs for service. Move the owned installation to a uniquely named archive;
# no recursive deletion and never touch the user's previous VirtualCam/WinFix folders.
$archive=Join-Path $script:DataRoot ('files-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$expected=[IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'IntelMacT2Patch'))
if(Test-Path $script:InstallRoot){
    if([IO.Path]::GetFullPath($script:InstallRoot) -ne $expected -or ((Get-Item $script:InstallRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw '安装目录校验失败，保留文件。'}
    Move-Item -LiteralPath $script:InstallRoot -Destination $archive
}
Move-Item -LiteralPath $script:StatePath -Destination (Join-Path $script:DataRoot 'last-uninstalled.json') -Force
Write-Host "已回滚。日志和备份保存在 $script:DataRoot；再次安装会自动归档这份备份。" -ForegroundColor Green
