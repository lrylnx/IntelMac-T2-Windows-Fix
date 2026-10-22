param([switch]$CheckOnly)
. "$PSScriptRoot\Common.ps1"
Require-Admin
$target=Get-Target
Write-Host ($target | Format-List | Out-String)
if(-not $target.Supported){throw '只支持名单内的 Intel T2 MacBook 和 Windows 10 x64 19041–19045。其他机器仅可运行诊断。'}
$package=Split-Path $PSScriptRoot -Parent
$manifest=Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$required=@('scripts/Common.ps1','scripts/Install.ps1','scripts/Uninstall.ps1','scripts/Resume.ps1','bin/VirtualCam.dll','config/VirtualCam.ini')
foreach($name in $required){if($name -notin @($manifest.Files.Path)){throw "清单缺少必要文件：$name"}}
foreach($file in $manifest.Files){
    if($file.Path -notmatch '^(scripts|bin|config)/[^/\\]+$'){throw '无效的清单路径。'}
    $path=[IO.Path]::GetFullPath((Join-Path $package $file.Path))
    if(-not $path.StartsWith([IO.Path]::GetFullPath($package)+'\',[StringComparison]::OrdinalIgnoreCase)){throw '清单路径超出补丁包。'}
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.SHA256){throw "文件校验失败：$($file.Path)"}
}
if($CheckOnly){Write-Host '机型与包内文件校验通过。尚未安装或修改系统。';exit 0}
Assert-NoCameraApps
if(Test-Path $script:StatePath){throw '已存在安装或回滚记录。请先运行卸载回滚；不会覆盖初始备份。'}
if(Test-Path $script:InstallRoot){throw '安装目录已存在但状态不完整，请联系维护者检查。'}
if(Test-Path $script:DataRoot){
    $last=Join-Path $script:DataRoot 'last-uninstalled.json'
    if(-not (Test-Path $last)){throw '备份目录已存在但状态不完整，请联系维护者检查。'}
    $previous=Get-Content $last -Raw -Encoding UTF8 | ConvertFrom-Json
    if($previous.Status -ne 'Uninstalled' -or $previous.PlanTouched -or $previous.CameraTouched -or $previous.TaskTouched -or $previous.LegacyTouched){throw '上次回滚未完成，请先检查备份。'}
    $source=[IO.Path]::GetFullPath($script:DataRoot)
    $expected=[IO.Path]::GetFullPath((Join-Path $env:ProgramData 'IntelMacT2Patch'))
    $dest=$expected+'-history-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')
    if($source -ne $expected -or (Split-Path $dest -Parent) -ne [IO.Path]::GetFullPath($env:ProgramData) -or ((Get-Item $source).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw '备份路径校验失败。'}
    Move-Item -LiteralPath $source -Destination $dest
}
if(Test-Path 'HKCU:\Software\Classes\CLSID\{7F3C1A20-9D54-4C1B-A37E-518B2D449F01}'){throw '检测到用户级同名摄像头注册，请由维护者处理后安装。'}
$oldCam=$null
if(Test-Path $script:CamKey){$oldCam=(Get-Item $script:CamKey).GetValue('');if(-not (Test-Path -LiteralPath $oldCam)){throw '旧摄像头注册指向不存在的 DLL，请先修复旧安装。'}}
if(Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue){throw '同名计划任务已存在，拒绝覆盖。'}
$legacy=Get-ScheduledTask -TaskName 'SleepResume-BluetoothRepair' -ErrorAction SilentlyContinue
if($legacy -and $legacy.State -eq 'Running'){throw '旧蓝牙恢复任务正在运行，请稍后重试。'}
Secure-Folder $script:InstallRoot
Secure-Folder $script:DataRoot
$state=[ordered]@{Version='0.2.0-rc1';Status='Preparing';Model=$target.Model;OriginalPlan=(Active-Plan);PatchPlan=([guid]::NewGuid().ToString());OldCamera=$oldCam;CameraTouched=$false;PlanTouched=$false;TaskTouched=$false;LegacyEnabled=($null -ne $legacy -and $legacy.State -ne 'Disabled');LegacyTouched=$false;InstalledAt=(Get-Date -Format o)}
Save-State $state
try {
    foreach($file in $manifest.Files){
        $destination=Join-Path $script:InstallRoot $file.Path
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $package $file.Path) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $script:InstallRoot 'config\VirtualCam.ini') -Destination (Join-Path $script:InstallRoot 'bin\VirtualCam.ini')
    Run-Native "$env:SystemRoot\System32\powercfg.exe" @('/export',(Join-Path $script:DataRoot 'original-power.pow'),$state.OriginalPlan) | Out-Null
    if($legacy){Export-ScheduledTask -TaskName $legacy.TaskName | Set-Content -LiteralPath (Join-Path $script:DataRoot 'legacy-task.xml') -Encoding Unicode}
    $state.PlanTouched=$true;Save-State $state
    Run-Native powercfg.exe @('/duplicatescheme',$state.OriginalPlan,$state.PatchPlan) | Out-Null
    Run-Native powercfg.exe @('/changename',$state.PatchPlan,'Intel Mac T2 - Sleep compatibility') | Out-Null
    foreach($supply in @('/setacvalueindex','/setdcvalueindex')) {
        # Preserve S3, lid behavior, battery protection and automatic hibernation timeouts.
        Run-Native powercfg.exe @($supply,$state.PatchPlan,'SUB_SLEEP','RTCWAKE','0') | Out-Null
        Run-Native powercfg.exe @($supply,$state.PatchPlan,'SUB_SLEEP','HYBRIDSLEEP','0') | Out-Null
    }
    Run-Native powercfg.exe @('/setactive',$state.PatchPlan) | Out-Null
    $state.CameraTouched=$true;Save-State $state
    $dll=Join-Path $script:InstallRoot 'bin\VirtualCam.dll'
    Camera-Registration $dll
    if((Get-Item $script:CamKey).GetValue('') -ne $dll){throw '摄像头注册结果不匹配。'}
    if($state.LegacyEnabled){$state.LegacyTouched=$true;Save-State $state;Disable-ScheduledTask -TaskName 'SleepResume-BluetoothRepair' | Out-Null}
    $exe=[Security.SecurityElement]::Escape("$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe")
    $arg=[Security.SecurityElement]::Escape('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+(Join-Path $script:InstallRoot 'scripts\Resume.ps1')+'" -Mode Auto')
    $xml=@"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
<RegistrationInfo><Description>Intel T2 MacBook resume recovery; protected files; no wake timer.</Description></RegistrationInfo>
<Triggers><EventTrigger><Enabled>true</Enabled><Subscription>&lt;QueryList&gt;&lt;Query Id="0" Path="System"&gt;&lt;Select Path="System"&gt;*[System[(Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and EventID=1) or (Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=107)]]&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription></EventTrigger></Triggers>
<Principals><Principal id="Author"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable><WakeToRun>false</WakeToRun><ExecutionTimeLimit>PT2M</ExecutionTimeLimit><Enabled>true</Enabled></Settings>
<Actions Context="Author"><Exec><Command>$exe</Command><Arguments>$arg</Arguments></Exec></Actions>
</Task>
"@
    $state.TaskTouched=$true;Save-State $state
    Register-ScheduledTask -TaskName $script:TaskName -Xml $xml | Out-Null
    $state.Status='Installed';Save-State $state
    Log 'Installation complete. Sleep/call acceptance still required.'
    Write-Host '安装完成。请重开微信，在摄像头列表选择 Virtual Camera。保留原睡眠/合盖行为。' -ForegroundColor Green
    Write-Host '本包为候选版：需完成睡眠、蓝牙重连和微信画面验收后再批量交付。'
} catch {
    $reason=$_
    Log "Installation failed: $reason"
    try { & (Join-Path $script:InstallRoot 'scripts\Uninstall.ps1') -Rollback } catch {Write-Warning "自动回滚未完成：$_。请再次运行卸载回滚。"}
    throw $reason
}
